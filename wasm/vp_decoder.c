// VoidPlayer web fallback decoder core.
//
// Direct libavformat/libavcodec/libswscale access for the browser prototype's
// WASM fallback path. No CLI, no filters: one context per media source, an
// explicit frame index, and exact-PTS frame extraction with decoder-state
// continuation for sequential stepping.

#include <libavcodec/avcodec.h>
#include <libavformat/avformat.h>
#include <libavformat/avio.h>
#include <libavutil/avutil.h>
#include <libavutil/pixdesc.h>
#include <libswscale/swscale.h>

#include <emscripten.h>

#include <stdint.h>
#include <limits.h>
#include <stddef.h>
#include <stdlib.h>
#include <string.h>

#ifdef VP_MT
#include <emscripten/threading.h>
#endif

#define VP_EOF 0
#define VP_OK 1
#define VP_MISMATCH 2
#define VP_ERR (-1)

// Seeks instead of walking forward when the target is further ahead than this
// many index frames; GOP-length agnostic approximation.
#define VP_MAX_FORWARD_WALK 8

// Versioned, immutable-until-next-output descriptor for the RGBA buffer.
// All values describe the converted AVFrame, never a prefetched codec context.
typedef struct VPFrameInfo {
    int64_t pts;
    int64_t duration;
    uint32_t abi_version;
    uint32_t descriptor_bytes;
    int32_t width, height, stride, bytes;
    int32_t source_format, primaries, transfer, matrix, range;
    int32_t sar_num, sar_den;
    uint32_t revision;
} VPFrameInfo;
_Static_assert(sizeof(VPFrameInfo) == 72, "frame ABI size");
_Static_assert(offsetof(VPFrameInfo, revision) == 68, "frame ABI offsets");

// Versioned binary record used to move a complete container index across the
// JS/WASM boundary. The WASM ABI is little-endian and fixed at 24 bytes.
typedef struct VPIndexRecordV1 {
    int64_t ticks;
    int64_t duration;
    uint32_t key;
    uint32_t reserved;
} VPIndexRecordV1;
_Static_assert(sizeof(VPIndexRecordV1) == 24, "index ABI v1 record size");
_Static_assert(offsetof(VPIndexRecordV1, key) == 16, "index ABI v1 key offset");
_Static_assert(offsetof(VPIndexRecordV1, reserved) == 20, "index ABI v1 reserved offset");

typedef struct VPContext {
    AVFormatContext *fmt;
    AVCodecContext *dec;
    AVPacket *pkt;
    AVFrame *frame;
    struct SwsContext *sws;
    int stream_idx;
    int sws_width;
    int sws_height;
    enum AVPixelFormat sws_fmt;

    int64_t *index_ticks;
    int *index_key;
    int64_t *index_duration;
    size_t index_count;
    size_t index_capacity;

    uint8_t *pixels;
    size_t pixels_size;
    VPFrameInfo output;
    int64_t last_ticks;      // pts of the frame currently in pixels
    int have_frame;          // pixels holds last_ticks
    int decode_eof;          // decoder drained; further reads yield nothing

    // Custom-IO input (blob chunks read on demand from JS); NULL for MEMFS.
    struct VpBlobIO *io;
    AVIOContext *pb;
} VPContext;

// Custom AVIO over a JS-side Blob: reads are synchronous in the hosting worker
// (FileReaderSync), so no whole-file MEMFS copy ever exists.
typedef struct VpBlobIO {
    int handle;
    int64_t size;
    int64_t pos;
} VpBlobIO;

EM_JS(long, vp_js_read, (int handle, double offset, uint8_t *buf, int len), {
    const entry = Module.vpBlobs.get(handle);
    if (!entry) return -1;
    const bytes = new Uint8Array(entry.reader.readAsArrayBuffer(entry.blob.slice(offset, offset + len)));
    HEAPU8.set(bytes, buf);
    return bytes.length;
});

static int vp_avio_read(void *opaque, uint8_t *buf, int size) {
    VpBlobIO *io = (VpBlobIO *)opaque;
    long got = vp_js_read(io->handle, (double)io->pos, buf, size);
    if (got < 0) return AVERROR(EIO);
    io->pos += got;
    return got == 0 ? AVERROR_EOF : (int)got;
}

static int64_t vp_avio_seek(void *opaque, int64_t offset, int whence) {
    VpBlobIO *io = (VpBlobIO *)opaque;
    if (whence == AVSEEK_SIZE) return io->size;
    int64_t pos;
    if ((whence & 0xFFFF) == SEEK_SET) pos = offset;
    else if ((whence & 0xFFFF) == SEEK_CUR) pos = io->pos + offset;
    else if ((whence & 0xFFFF) == SEEK_END) pos = io->size + offset;
    else return AVERROR(EINVAL);
    if (pos < 0 || pos > io->size) return AVERROR(EINVAL);
    io->pos = pos;
    return pos;
}

static void vp_reset_decoder_state(VPContext *ctx) {
    ctx->decode_eof = 0;
    ctx->have_frame = 0;
}

void vp_close_input(VPContext *ctx);
static int vp_decode_one(VPContext *ctx);

// Binary-search the ascending index; ticks come from packets in decode order
// and are sorted at build time.
static size_t vp_index_lower_bound(const VPContext *ctx, int64_t ticks) {
    size_t lo = 0, hi = ctx->index_count;
    while (lo < hi) {
        size_t mid = (lo + hi) / 2;
        if (ctx->index_ticks[mid] < ticks) lo = mid + 1; else hi = mid;
    }
    return lo;
}

static int g_thread_count = 0;

// Player-assigned decode thread budget for the NEXT vp_open on this context's
// decoder. av_cpu_count() always reports 1 under Emscripten, so without this
// every decoder would silently run single-threaded.
void vp_set_threads(int count) {
    g_thread_count = count > 0 ? count : 0;
}

VPContext *vp_create(void) {
    VPContext *ctx = calloc(1, sizeof(VPContext));
    if (!ctx) return NULL;
    ctx->stream_idx = -1;
    ctx->pkt = av_packet_alloc();
    ctx->frame = av_frame_alloc();
    if (!ctx->pkt || !ctx->frame) {
        av_packet_free(&ctx->pkt);
        av_frame_free(&ctx->frame);
        free(ctx);
        return NULL;
    }
    return ctx;
}

void vp_destroy(VPContext *ctx) {
    if (!ctx) return;
    vp_close_input(ctx);
    av_packet_free(&ctx->pkt);
    av_frame_free(&ctx->frame);
    free(ctx->index_ticks);
    free(ctx->index_key);
    free(ctx->index_duration);
    free(ctx->pixels);
    free(ctx);
}

void vp_close_input(VPContext *ctx) {
    if (!ctx) return;
    sws_freeContext(ctx->sws);
    ctx->sws = NULL;
    avcodec_free_context(&ctx->dec);
    avformat_close_input(&ctx->fmt);
    if (ctx->pb) {
        av_freep(&ctx->pb->buffer);
        avio_context_free(&ctx->pb);
    }
    free(ctx->io);
    ctx->io = NULL;
    ctx->stream_idx = -1;
    ctx->index_count = 0;
    vp_reset_decoder_state(ctx);
}

// Shared tail: stream selection, decoder open, geometry priming.
static int vp_open_decoders(VPContext *ctx) {
    if (avformat_find_stream_info(ctx->fmt, NULL) < 0) return VP_ERR;

    const AVCodec *codec = NULL;
    ctx->stream_idx = av_find_best_stream(ctx->fmt, AVMEDIA_TYPE_VIDEO, -1, -1, &codec, 0);
    if (ctx->stream_idx < 0 || !codec) return VP_ERR;

    ctx->dec = avcodec_alloc_context3(codec);
    if (!ctx->dec) return VP_ERR;
#ifdef VP_MT
    // Player-managed thread budget: fall back to the host's core count when
    // the player did not assign one.
    ctx->dec->thread_count = g_thread_count > 0 ? g_thread_count : emscripten_num_logical_cores();
#endif
    AVStream *stream = ctx->fmt->streams[ctx->stream_idx];
    if (avcodec_parameters_to_context(ctx->dec, stream->codecpar) < 0) return VP_ERR;
    if (avcodec_open2(ctx->dec, codec, NULL) < 0) return VP_ERR;
    vp_reset_decoder_state(ctx);
    // Prime the decoder with real frames: streams like MPEG-2 in TS keep the
    // sequence header (dimensions) in the bitstream, and the decoder must see
    // it once before any mid-stream seek can succeed. avcodec_flush_buffers
    // retains this state, so one warm-up decode covers all later seeks.
    for (int tries = 0; tries < 200; tries++) {
        if (vp_decode_one(ctx) != VP_OK) break;
        if (ctx->frame->width > 0 && ctx->frame->height > 0) break;
    }
    if (ctx->frame->width <= 0 || ctx->frame->height <= 0) return VP_ERR;
    av_seek_frame(ctx->fmt, ctx->stream_idx, 0, AVSEEK_FLAG_BACKWARD);
    avcodec_flush_buffers(ctx->dec);
    vp_reset_decoder_state(ctx);
    return 0;
}

int vp_open(VPContext *ctx, const char *path) {
    if (!ctx || !path) return VP_ERR;
    vp_close_input(ctx);
    if (avformat_open_input(&ctx->fmt, path, NULL, NULL) < 0) return VP_ERR;
    return vp_open_decoders(ctx);
}

// Opens a JS-side Blob through the custom AVIO (chunked, on-demand reads).
int vp_open_blob(VPContext *ctx, int handle, int64_t size) {
    if (!ctx || size <= 0) return VP_ERR;
    vp_close_input(ctx);
    VpBlobIO *io = malloc(sizeof(*io));
    if (!io) return VP_ERR;
    io->handle = handle;
    io->size = size;
    io->pos = 0;
    const size_t buf_size = 256 * 1024;
    uint8_t *buffer = av_malloc(buf_size);
    if (!buffer) { free(io); return VP_ERR; }
    AVIOContext *pb = avio_alloc_context(buffer, (int)buf_size, 0, io, vp_avio_read, NULL, vp_avio_seek);
    if (!pb) { av_free(buffer); free(io); return VP_ERR; }
    AVFormatContext *fmt = avformat_alloc_context();
    if (!fmt) { avio_context_free(&pb); free(io); return VP_ERR; }
    fmt->pb = pb;
    fmt->flags |= AVFMT_FLAG_CUSTOM_IO;
    if (avformat_open_input(&fmt, NULL, NULL, NULL) < 0) {
        avio_context_free(&pb);
        avformat_free_context(fmt);
        free(io);
        return VP_ERR;
    }
    ctx->io = io;
    ctx->pb = pb;
    ctx->fmt = fmt;
    return vp_open_decoders(ctx);
}

int vp_width(VPContext *ctx) { return ctx && ctx->dec ? ctx->dec->width : 0; }
int vp_height(VPContext *ctx) { return ctx && ctx->dec ? ctx->dec->height : 0; }

// Timebase as rational so JS can compute exact microsecond timestamps.
int vp_tb_num(VPContext *ctx) {
    return ctx && ctx->stream_idx >= 0 ? ctx->fmt->streams[ctx->stream_idx]->time_base.num : 0;
}
int vp_tb_den(VPContext *ctx) {
    return ctx && ctx->stream_idx >= 0 ? ctx->fmt->streams[ctx->stream_idx]->time_base.den : 0;
}

const char *vp_codec_name(VPContext *ctx) {
    return ctx && ctx->dec ? avcodec_get_name(ctx->dec->codec_id) : "";
}

// Report the decoder's source format before the RGBA presentation conversion.
const char *vp_pixel_format(VPContext *ctx) {
    if (!ctx || !ctx->dec) return "";
    enum AVPixelFormat fmt = ctx->frame->format >= 0 ? ctx->frame->format : ctx->dec->pix_fmt;
    const char *name = av_get_pix_fmt_name(fmt);
    return name ? name : "";
}

// Stream color metadata as FFmpeg enum ints; JS maps the handful it names.
// Prefer the primed frame's bitstream values over the codec context, whose
// fields can stay at container defaults.
static int vp_pick(int frame_value, int dec_value, int unspecified) {
    return frame_value != unspecified ? frame_value : dec_value;
}
int vp_color_primaries(VPContext *ctx) { return ctx ? vp_pick(ctx->frame->color_primaries, ctx->dec ? ctx->dec->color_primaries : 2, AVCOL_PRI_UNSPECIFIED) : 2; }
int vp_color_transfer(VPContext *ctx) { return ctx ? vp_pick(ctx->frame->color_trc, ctx->dec ? ctx->dec->color_trc : 2, AVCOL_TRC_UNSPECIFIED) : 2; }
int vp_color_space(VPContext *ctx) { return ctx ? vp_pick(ctx->frame->colorspace, ctx->dec ? ctx->dec->colorspace : 2, AVCOL_SPC_UNSPECIFIED) : 2; }
int vp_color_range(VPContext *ctx) { return ctx ? vp_pick(ctx->frame->color_range, ctx->dec ? ctx->dec->color_range : 0, AVCOL_RANGE_UNSPECIFIED) : 0; }

static int vp_index_push(VPContext *ctx, int64_t ticks, int key, int64_t duration) {
    if (ctx->index_count == ctx->index_capacity) {
        size_t capacity = ctx->index_capacity ? ctx->index_capacity * 2 : 1024;
        int64_t *ticks = realloc(ctx->index_ticks, capacity * sizeof(*ticks));
        int *keys = realloc(ctx->index_key, capacity * sizeof(*keys));
        int64_t *durations = realloc(ctx->index_duration, capacity * sizeof(*durations));
        if (!ticks || !keys || !durations) {
            free(ticks);
            free(keys);
            free(durations);
            return VP_ERR;
        }
        ctx->index_ticks = ticks;
        ctx->index_key = keys;
        ctx->index_duration = durations;
        ctx->index_capacity = capacity;
    }
    ctx->index_ticks[ctx->index_count] = ticks;
    ctx->index_key[ctx->index_count] = key;
    ctx->index_duration[ctx->index_count] = duration;
    ctx->index_count++;
    return 0;
}

// Decodes until one output frame is produced, EOF, or error. Returns
// VP_OK with ctx->frame filled, VP_EOF when drained, VP_ERR on failure.
static int vp_decode_one(VPContext *ctx) {
    while (!ctx->decode_eof) {
        int ret = avcodec_receive_frame(ctx->dec, ctx->frame);
        if (ret == 0) return VP_OK;
        if (ret == AVERROR_EOF) {
            ctx->decode_eof = 1;
            return VP_EOF;
        }
        if (ret != AVERROR(EAGAIN)) return VP_ERR;

        ret = av_read_frame(ctx->fmt, ctx->pkt);
        if (ret == AVERROR_EOF) {
            avcodec_send_packet(ctx->dec, NULL);
            continue;
        }
        if (ret < 0) return VP_ERR;
        if (ctx->pkt->stream_index != ctx->stream_idx) {
            av_packet_unref(ctx->pkt);
            continue;
        }
        ret = avcodec_send_packet(ctx->dec, ctx->pkt);
        av_packet_unref(ctx->pkt);
        if (ret < 0 && ret != AVERROR(EAGAIN)) return VP_ERR;
    }
    return VP_EOF;
}

int vp_index_build(VPContext *ctx) {
    if (!ctx || !ctx->dec) return VP_ERR;
    // Demux-only pass: packet pts/duration/key flags are enough for the index
    // and avoid decoding the entire stream at load time (expensive for VVC).
    av_seek_frame(ctx->fmt, ctx->stream_idx, 0, AVSEEK_FLAG_BACKWARD);
    ctx->index_count = 0;
    while (av_read_frame(ctx->fmt, ctx->pkt) >= 0) {
        if (ctx->pkt->stream_index == ctx->stream_idx && ctx->pkt->pts != AV_NOPTS_VALUE) {
            int key = !!(ctx->pkt->flags & AV_PKT_FLAG_KEY);
            if (vp_index_push(ctx, ctx->pkt->pts, key, ctx->pkt->duration) < 0) {
                av_packet_unref(ctx->pkt);
                return VP_ERR;
            }
        }
        av_packet_unref(ctx->pkt);
    }
    // Sort into presentation order, keeping key/duration paired with ticks.
    for (size_t i = 1; i < ctx->index_count; i++) {
        int64_t ticks = ctx->index_ticks[i];
        int key = ctx->index_key[i];
        int64_t duration = ctx->index_duration[i];
        size_t j = i;
        while (j > 0 && ctx->index_ticks[j - 1] > ticks) {
            ctx->index_ticks[j] = ctx->index_ticks[j - 1];
            ctx->index_key[j] = ctx->index_key[j - 1];
            ctx->index_duration[j] = ctx->index_duration[j - 1];
            j--;
        }
        ctx->index_ticks[j] = ticks;
        ctx->index_key[j] = key;
        ctx->index_duration[j] = duration;
    }
    av_seek_frame(ctx->fmt, ctx->stream_idx, 0, AVSEEK_FLAG_BACKWARD);
    avcodec_flush_buffers(ctx->dec);
    vp_reset_decoder_state(ctx);
    return (int)ctx->index_count;
}

int vp_index_count(VPContext *ctx) { return ctx ? (int)ctx->index_count : 0; }
int64_t vp_index_ticks(VPContext *ctx, int i) {
    return ctx && i >= 0 && (size_t)i < ctx->index_count ? ctx->index_ticks[i] : 0;
}
int vp_index_is_key(VPContext *ctx, int i) {
    return ctx && i >= 0 && (size_t)i < ctx->index_count ? ctx->index_key[i] : 0;
}
int64_t vp_index_duration(VPContext *ctx, int i) {
    return ctx && i >= 0 && (size_t)i < ctx->index_count ? ctx->index_duration[i] : 0;
}

int vp_index_abi_version(void) { return 1; }
int vp_index_record_bytes(void) { return (int)sizeof(VPIndexRecordV1); }

// Number of bytes required to export the current index, or VP_ERR if the
// record count cannot be represented by the wasm32 API.
int vp_index_export_bytes(VPContext *ctx) {
    if (!ctx || ctx->index_count > (size_t)(INT_MAX / (int)sizeof(VPIndexRecordV1))) return VP_ERR;
    return (int)(ctx->index_count * sizeof(VPIndexRecordV1));
}

// Writes fixed-layout records to caller-owned WASM memory. Returns record
// count on success; rejects undersized buffers without writing any records.
int vp_index_export(VPContext *ctx, void *output, int capacity_bytes) {
    int bytes = vp_index_export_bytes(ctx);
    if (bytes <= 0 || !output || capacity_bytes < bytes) return VP_ERR;
    uint8_t *dst = (uint8_t *)output;
    for (size_t i = 0; i < ctx->index_count; i++) {
        VPIndexRecordV1 record = {
            .ticks = ctx->index_ticks[i],
            .duration = ctx->index_duration[i],
            .key = ctx->index_key[i] ? 1u : 0u,
            .reserved = 0u
        };
        memcpy(dst + i * sizeof(record), &record, sizeof(record));
    }
    return (int)ctx->index_count;
}

// Replaces the current table only after every record has passed validation
// and all replacement arrays have been allocated. Decode position is left
// unchanged; normal callers import immediately after vp_open.
int vp_index_import(VPContext *ctx, const void *input, int count) {
    if (!ctx || !ctx->dec || ctx->stream_idx < 0 || !input || count <= 0 ||
        count > INT_MAX / (int)sizeof(VPIndexRecordV1)) return VP_ERR;

    size_t n = (size_t)count;
    int64_t *ticks = malloc(n * sizeof(*ticks));
    int *keys = malloc(n * sizeof(*keys));
    int64_t *durations = malloc(n * sizeof(*durations));
    if (!ticks || !keys || !durations) {
        free(ticks);
        free(keys);
        free(durations);
        return VP_ERR;
    }

    const uint8_t *src = (const uint8_t *)input;
    int64_t previous = 0;
    for (size_t i = 0; i < n; i++) {
        VPIndexRecordV1 record;
        memcpy(&record, src + i * sizeof(record), sizeof(record));
        if (record.key > 1u || record.reserved != 0u || (i > 0 && record.ticks < previous)) {
            free(ticks);
            free(keys);
            free(durations);
            return VP_ERR;
        }
        ticks[i] = record.ticks;
        keys[i] = (int)record.key;
        durations[i] = record.duration;
        previous = record.ticks;
    }

    free(ctx->index_ticks);
    free(ctx->index_key);
    free(ctx->index_duration);
    ctx->index_ticks = ticks;
    ctx->index_key = keys;
    ctx->index_duration = durations;
    ctx->index_count = n;
    ctx->index_capacity = n;
    return count;
}

static int vp_ensure_pixels(VPContext *ctx, int width, int height) {
    if (width <= 0 || height <= 0 || width > INT_MAX / 4 || (uint64_t)width * height * 4 > INT_MAX) return VP_ERR;
    size_t needed = (size_t)width * (size_t)height * 4;
    if (ctx->pixels_size >= needed) return 0;
    uint8_t *pixels = realloc(ctx->pixels, needed);
    if (!pixels) return VP_ERR;
    ctx->pixels = pixels;
    ctx->pixels_size = needed;
    return 0;
}

static int vp_convert(VPContext *ctx) {
    AVFrame *frame = ctx->frame;
    if (!ctx->sws || ctx->sws_width != frame->width || ctx->sws_height != frame->height ||
        ctx->sws_fmt != frame->format) {
        sws_freeContext(ctx->sws);
        ctx->sws = sws_getContext(frame->width, frame->height, frame->format,
                                  frame->width, frame->height, AV_PIX_FMT_RGBA,
                                  SWS_BICUBIC, NULL, NULL, NULL);
        if (!ctx->sws) return VP_ERR;
        ctx->sws_width = frame->width;
        ctx->sws_height = frame->height;
        ctx->sws_fmt = frame->format;
    }
    // Honor tagged colorspace/range when present; fall back to BT.709 for HD
    // and BT.601 below, the same guess browsers make for untagged content.
    enum AVColorSpace space = frame->colorspace;
    if (space == AVCOL_SPC_UNSPECIFIED) space = frame->height >= 720 ? AVCOL_SPC_BT709 : AVCOL_SPC_SMPTE170M;
    const int *inv_table = sws_getCoefficients(space);
    int src_range = frame->color_range == AVCOL_RANGE_JPEG;
    sws_setColorspaceDetails(ctx->sws, inv_table, src_range,
                             sws_getCoefficients(AVCOL_SPC_SMPTE170M), 0, 0, 1 << 16, 1 << 16);
    if (vp_ensure_pixels(ctx, frame->width, frame->height) < 0) return VP_ERR;
    uint8_t *dst[4] = { ctx->pixels, NULL, NULL, NULL };
    int dst_stride[4] = { frame->width * 4, 0, 0, 0 };
    if (sws_scale(ctx->sws, (const uint8_t *const *)frame->data, frame->linesize,
              0, frame->height, dst, dst_stride) != frame->height) return VP_ERR;
    VPFrameInfo next = {0};
    next.pts = frame->best_effort_timestamp != AV_NOPTS_VALUE ? frame->best_effort_timestamp : frame->pts;
    next.duration = frame->duration;
    next.abi_version = 1; next.descriptor_bytes = sizeof(VPFrameInfo);
    next.width = frame->width; next.height = frame->height;
    next.stride = frame->width * 4; next.bytes = next.stride * frame->height;
    next.source_format = frame->format;
    next.primaries = frame->color_primaries; next.transfer = frame->color_trc;
    next.matrix = frame->colorspace; next.range = frame->color_range;
    next.sar_num = frame->sample_aspect_ratio.num > 0 ? frame->sample_aspect_ratio.num : 1;
    next.sar_den = frame->sample_aspect_ratio.den > 0 ? frame->sample_aspect_ratio.den : 1;
    // Metadata revision survives decoder resets and configuration changes.
    next.revision = ctx->output.revision;
    if (!next.revision || memcmp(&next.width, &ctx->output.width, offsetof(VPFrameInfo, revision) - offsetof(VPFrameInfo, width))) next.revision++;
    ctx->output = next;
    return 0;
}

// Extracts the frame whose pts equals target_ticks exactly, converting it to
// RGBA in the pixel buffer. Sequential forward targets reuse decoder state;
// anything else seeks to the closest keyframe at or before the target.
int vp_extract(VPContext *ctx, int64_t target_ticks) {
    if (!ctx || !ctx->dec) return VP_ERR;
    if (ctx->have_frame && ctx->last_ticks == target_ticks) return VP_OK;

    int seek = ctx->decode_eof || target_ticks < ctx->last_ticks || !ctx->have_frame;
    if (!seek && ctx->index_count > 0) {
        // Walk forward only for nearby targets; jump via keyframe otherwise.
        size_t ahead = vp_index_lower_bound(ctx, ctx->last_ticks + 1);
        seek = (vp_index_lower_bound(ctx, target_ticks + 1) - ahead) > VP_MAX_FORWARD_WALK;
    }
    int restarted = 0;
    for (;;) {
        if (seek) {
            // Seek-by-byte-estimation demuxers (e.g. MPEG-TS) can land past
            // the target; on overshoot restart once from the beginning.
            if (av_seek_frame(ctx->fmt, ctx->stream_idx, restarted ? 0 : target_ticks,
                              AVSEEK_FLAG_BACKWARD) < 0) {
                av_seek_frame(ctx->fmt, ctx->stream_idx, 0, AVSEEK_FLAG_BACKWARD);
            }
            avcodec_flush_buffers(ctx->dec);
            vp_reset_decoder_state(ctx);
            seek = 0;
        }
        int ret = vp_decode_one(ctx);
        // Both overshoot and an immediate EOF after a seek can mean the
        // byte-estimated seek landed past the target: retry once from the
        // beginning before giving up.
        if (ret == VP_EOF) {
            if (!restarted) { restarted = 1; seek = 1; continue; }
            return VP_EOF;
        }
        if (ret != VP_OK) return VP_ERR;
        int64_t ticks = ctx->frame->best_effort_timestamp;
        if (ticks == AV_NOPTS_VALUE || ticks < target_ticks) continue;
        if (ticks > target_ticks) {
            if (!restarted) { restarted = 1; seek = 1; continue; }
            return VP_MISMATCH;
        }
        if (vp_convert(ctx) < 0) return VP_ERR;
        ctx->last_ticks = ticks;
        ctx->have_frame = 1;
        return VP_OK;
    }
}

int64_t vp_last_ticks(VPContext *ctx) { return ctx && ctx->have_frame ? ctx->last_ticks : -1; }
uint8_t *vp_pixels(VPContext *ctx) { return ctx && ctx->have_frame ? ctx->pixels : NULL; }
const VPFrameInfo *vp_frame_info(VPContext *ctx) { return ctx && ctx->have_frame ? &ctx->output : NULL; }
const char *vp_frame_format(VPContext *ctx) {
    const char *name = ctx && ctx->have_frame ? av_get_pix_fmt_name(ctx->output.source_format) : NULL;
    return name ? name : "";
}

// Packet-only decoder: TS owns demux, timestamps and seeking. Compressed data
// is written directly into an AVPacket allocation, avoiding a second packet
// copy inside the core. Call receive until EAGAIN before sending more input.
int vp_packet_open(VPContext *ctx, const char *name, const uint8_t *extra, int size) {
    if (!ctx || !name || size < 0 || size > 1024 * 1024) return VP_ERR;
    vp_close_input(ctx);
    const AVCodec *codec = avcodec_find_decoder_by_name(!strcmp(name, "av1") ? "libdav1d" : name);
    if (!codec) return VP_ERR;
    ctx->dec = avcodec_alloc_context3(codec);
    if (!ctx->dec) return VP_ERR;
    ctx->dec->pkt_timebase = (AVRational){1, 1000000};
    ctx->dec->strict_std_compliance = FF_COMPLIANCE_STRICT;
    ctx->dec->thread_count = 1;
#ifdef VP_MT
    ctx->dec->thread_count = g_thread_count > 0 ? g_thread_count : 2;
#endif
    if (size) {
        ctx->dec->extradata = av_mallocz(size + AV_INPUT_BUFFER_PADDING_SIZE);
        if (!ctx->dec->extradata) return VP_ERR;
        memcpy(ctx->dec->extradata, extra, size);
        ctx->dec->extradata_size = size;
    }
    return avcodec_open2(ctx->dec, codec, NULL) < 0 ? VP_ERR : 0;
}

uint8_t *vp_packet_alloc(VPContext *ctx, int size) {
    if (!ctx || size <= 0 || size > 0xffffff) return NULL;
    av_packet_unref(ctx->pkt);
    if (av_new_packet(ctx->pkt, size) < 0) return NULL;
    return ctx->pkt->data;
}

int vp_packet_send(VPContext *ctx, int64_t pts, int64_t dts, int key, int eof) {
    if (!ctx || !ctx->dec) return VP_ERR;
    ctx->pkt->pts = pts; ctx->pkt->dts = dts;
    ctx->pkt->flags = key ? AV_PKT_FLAG_KEY : 0;
    int ret = avcodec_send_packet(ctx->dec, eof ? NULL : ctx->pkt);
    av_packet_unref(ctx->pkt);
    return ret < 0 ? VP_ERR : 0;
}

int vp_packet_receive(VPContext *ctx, int64_t minimum_pts) {
    if (!ctx || !ctx->dec) return VP_ERR;
    for (;;) {
        av_frame_unref(ctx->frame);
        int ret = avcodec_receive_frame(ctx->dec, ctx->frame);
        if (ret == AVERROR(EAGAIN) || ret == AVERROR_EOF) return 0;
        if (ret < 0) return VP_ERR;
        int64_t pts = ctx->frame->best_effort_timestamp;
        if (pts == AV_NOPTS_VALUE) pts = ctx->frame->pts;
        if (pts < minimum_pts) continue;
        if (vp_convert(ctx) < 0) return VP_ERR;
        ctx->last_ticks = pts;
        ctx->have_frame = 1;
        return 1;
    }
}

// Some decoders keep a presentation FIFO after flush (HEVC in the pinned
// FFmpeg revision). Empty residual output before accepting a new seek epoch.
void vp_packet_reset(VPContext *ctx) {
    if (!ctx || !ctx->dec) return;
    avcodec_flush_buffers(ctx->dec);
    while (avcodec_receive_frame(ctx->dec, ctx->frame) >= 0) av_frame_unref(ctx->frame);
    avcodec_flush_buffers(ctx->dec);
    av_packet_unref(ctx->pkt);
    av_frame_unref(ctx->frame);
    vp_reset_decoder_state(ctx);
}
