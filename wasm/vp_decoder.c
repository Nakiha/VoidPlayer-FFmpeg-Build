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
    int32_t layout, bit_depth, bit_shift, subsample_x, subsample_y, chroma_location;
    struct { int32_t offset, stride, width, height; } planes[3];
    int32_t crop_left, crop_top, crop_right, crop_bottom;
} VPFrameInfo;
_Static_assert(sizeof(VPFrameInfo) == 160, "frame ABI size");
_Static_assert(offsetof(VPFrameInfo, revision) == 68, "frame ABI offsets");

// Index ABI v2 preserves presentation order and demux seek metadata.
// flags bit 0 is key; bit 1 is a demuxer-safe seek anchor.
typedef struct VPIndexRecordV2 {
    int64_t pts;
    int64_t dts;
    int64_t duration;
    int64_t pos;
    int32_t packet_size;
    uint32_t flags;
} VPIndexRecordV2;
_Static_assert(sizeof(VPIndexRecordV2) == 40, "index ABI v2 record size");
_Static_assert(offsetof(VPIndexRecordV2, dts) == 8, "index ABI v2 dts offset");
_Static_assert(offsetof(VPIndexRecordV2, duration) == 16, "index ABI v2 duration offset");
_Static_assert(offsetof(VPIndexRecordV2, pos) == 24, "index ABI v2 position offset");
_Static_assert(offsetof(VPIndexRecordV2, packet_size) == 32, "index ABI v2 packet size offset");
_Static_assert(offsetof(VPIndexRecordV2, flags) == 36, "index ABI v2 flags offset");

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
    int64_t *index_dts;
    int64_t *index_pos;
    int *index_packet_size;
    uint64_t *index_order;
    uint64_t index_next_order;
    size_t index_stable_count;
    int index_scan_progressive;
    int64_t index_scan_last_output_ticks;
    int index_import_streaming;
    int index_import_has_safe_ticks;
    int index_import_next_seq;
    int64_t index_import_safe_ticks;
    int index_seek_anchors;
    int index_scan_started;
    int index_scan_active;
    int index_scan_complete;
    int index_scan_error;
    int index_scan_packets;
    int64_t index_scan_bytes;
    int extract_frames, extract_restarts;
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
    free(ctx->index_dts);
    free(ctx->index_pos);
    free(ctx->index_packet_size);
    free(ctx->index_order);
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
    ctx->index_stable_count = 0;
    ctx->index_next_order = 0;
    ctx->index_scan_progressive = 0;
    ctx->index_scan_last_output_ticks = INT64_MIN;
    ctx->index_import_streaming = 0;
    ctx->index_import_has_safe_ticks = 0;
    ctx->index_import_next_seq = 0;
    ctx->index_import_safe_ticks = INT64_MIN;
    ctx->index_seek_anchors = 0;
    ctx->index_scan_started = 0;
    ctx->index_scan_active = 0;
    ctx->index_scan_complete = 0;
    ctx->index_scan_error = 0;
    ctx->index_scan_packets = 0;
    ctx->index_scan_bytes = 0;
    ctx->extract_frames = ctx->extract_restarts = 0;
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

static int vp_index_reserve(VPContext *ctx, size_t needed) {
    if (needed <= ctx->index_capacity) return 0;
    if (needed > (size_t)(INT_MAX / (int)sizeof(int64_t)) ||
        needed > SIZE_MAX / sizeof(uint64_t)) return VP_ERR;
    size_t capacity = ctx->index_capacity ? ctx->index_capacity : 1024;
    while (capacity < needed) {
        if (capacity > SIZE_MAX / 2) return VP_ERR;
        capacity *= 2;
    }
    if (capacity > (size_t)(INT_MAX / (int)sizeof(int64_t))) capacity = needed;
    int64_t *ticks = realloc(ctx->index_ticks, capacity * sizeof(*ticks));
    if (!ticks) return VP_ERR;
    ctx->index_ticks = ticks;
    int *keys = realloc(ctx->index_key, capacity * sizeof(*keys));
    if (!keys) return VP_ERR;
    ctx->index_key = keys;
    int64_t *durations = realloc(ctx->index_duration, capacity * sizeof(*durations));
    if (!durations) return VP_ERR;
    ctx->index_duration = durations;
    int64_t *decode_times = realloc(ctx->index_dts, capacity * sizeof(*decode_times));
    if (!decode_times) return VP_ERR;
    ctx->index_dts = decode_times;
    int64_t *positions = realloc(ctx->index_pos, capacity * sizeof(*positions));
    if (!positions) return VP_ERR;
    ctx->index_pos = positions;
    int *sizes = realloc(ctx->index_packet_size, capacity * sizeof(*sizes));
    if (!sizes) return VP_ERR;
    ctx->index_packet_size = sizes;
    uint64_t *orders = realloc(ctx->index_order, capacity * sizeof(*orders));
    if (!orders) return VP_ERR;
    ctx->index_order = orders;
    ctx->index_capacity = capacity;
    return 0;
}

static int vp_index_push(VPContext *ctx, int64_t ticks, int key, int64_t duration,
                         int64_t dts, int64_t pos, int packet_size) {
    if (ctx->index_count == SIZE_MAX || vp_index_reserve(ctx, ctx->index_count + 1) < 0) return VP_ERR;
    size_t i = ctx->index_count++;
    ctx->index_ticks[i] = ticks;
    ctx->index_key[i] = key;
    ctx->index_duration[i] = duration;
    ctx->index_dts[i] = dts;
    ctx->index_pos[i] = pos;
    ctx->index_packet_size[i] = packet_size;
    ctx->index_order[i] = ctx->index_next_order++;
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

typedef struct VPIndexSortKey {
    int64_t ticks;
    uint64_t order;
    size_t source;
} VPIndexSortKey;

static int vp_index_sort_key_compare(const void *left, const void *right) {
    const VPIndexSortKey *a = (const VPIndexSortKey *)left;
    const VPIndexSortKey *b = (const VPIndexSortKey *)right;
    if (a->ticks < b->ticks) return -1;
    if (a->ticks > b->ticks) return 1;
    return a->order < b->order ? -1 : a->order > b->order ? 1 : 0;
}

static int vp_index_sort_range(VPContext *ctx, size_t first, size_t count) {
    if (count < 2) return 0;
    if (count > SIZE_MAX / sizeof(VPIndexSortKey)) return VP_ERR;
    VPIndexSortKey *order = malloc(count * sizeof(*order));
    if (!order) return VP_ERR;
    for (size_t i = 0; i < count; i++) {
        order[i].ticks = ctx->index_ticks[first + i];
        order[i].order = ctx->index_order[first + i];
        order[i].source = i;
    }
    qsort(order, count, sizeof(*order), vp_index_sort_key_compare);

    // Apply the permutation in place, retaining original packet order for ties.
    for (size_t start = 0; start < count; start++) {
        if (order[start].source == SIZE_MAX) continue;
        if (order[start].source == start) {
            order[start].source = SIZE_MAX;
            continue;
        }
        int64_t saved_ticks = ctx->index_ticks[first + start];
        int saved_key = ctx->index_key[first + start];
        int64_t saved_duration = ctx->index_duration[first + start];
        int64_t saved_dts = ctx->index_dts[first + start];
        int64_t saved_pos = ctx->index_pos[first + start];
        int saved_packet_size = ctx->index_packet_size[first + start];
        uint64_t saved_order = ctx->index_order[first + start];
        size_t destination = start;
        for (;;) {
            size_t source = order[destination].source;
            order[destination].source = SIZE_MAX;
            size_t dst = first + destination;
            if (source == start) {
                ctx->index_ticks[dst] = saved_ticks;
                ctx->index_key[dst] = saved_key;
                ctx->index_duration[dst] = saved_duration;
                ctx->index_dts[dst] = saved_dts;
                ctx->index_pos[dst] = saved_pos;
                ctx->index_packet_size[dst] = saved_packet_size;
                ctx->index_order[dst] = saved_order;
                break;
            }
            size_t src = first + source;
            ctx->index_ticks[dst] = ctx->index_ticks[src];
            ctx->index_key[dst] = ctx->index_key[src];
            ctx->index_duration[dst] = ctx->index_duration[src];
            ctx->index_dts[dst] = ctx->index_dts[src];
            ctx->index_pos[dst] = ctx->index_pos[src];
            ctx->index_packet_size[dst] = ctx->index_packet_size[src];
            ctx->index_order[dst] = ctx->index_order[src];
            destination = source;
        }
    }
    free(order);
    return 0;
}

static int vp_index_sort_presentation_order(VPContext *ctx) {
    return vp_index_sort_range(ctx, 0, ctx->index_count);
}

static void vp_index_swap_records(VPContext *ctx, size_t a, size_t b) {
    if (a == b) return;
#define VP_SWAP_FIELD(field, type) do { type value = ctx->field[a]; ctx->field[a] = ctx->field[b]; ctx->field[b] = value; } while (0)
    VP_SWAP_FIELD(index_ticks, int64_t);
    VP_SWAP_FIELD(index_key, int);
    VP_SWAP_FIELD(index_duration, int64_t);
    VP_SWAP_FIELD(index_dts, int64_t);
    VP_SWAP_FIELD(index_pos, int64_t);
    VP_SWAP_FIELD(index_packet_size, int);
    VP_SWAP_FIELD(index_order, uint64_t);
#undef VP_SWAP_FIELD
}

static int vp_index_publish_through(VPContext *ctx, int64_t ticks) {
    size_t first = ctx->index_stable_count;
    size_t write = first;
    for (size_t i = first; i < ctx->index_count; i++) {
        if (ctx->index_ticks[i] <= ticks) {
            vp_index_swap_records(ctx, write, i);
            write++;
        }
    }
    if (write > first && vp_index_sort_range(ctx, first, write - first) < 0) return VP_ERR;
    ctx->index_stable_count = write;
    return VP_OK;
}

static void vp_index_disable_progressive(VPContext *ctx) {
    ctx->index_scan_progressive = 0;
}

static int vp_index_scan_drain(VPContext *ctx) {
    for (;;) {
        av_frame_unref(ctx->frame);
        int ret = avcodec_receive_frame(ctx->dec, ctx->frame);
        if (ret == AVERROR(EAGAIN) || ret == AVERROR_EOF) return VP_OK;
        if (ret < 0) { vp_index_disable_progressive(ctx); return VP_OK; }
        int64_t pts = ctx->frame->best_effort_timestamp;
        if (pts == AV_NOPTS_VALUE || (ctx->index_scan_last_output_ticks != INT64_MIN &&
                                     pts < ctx->index_scan_last_output_ticks)) {
            vp_index_disable_progressive(ctx);
            return VP_OK;
        }
        ctx->index_scan_last_output_ticks = pts;
        if (vp_index_publish_through(ctx, pts) < 0) vp_index_disable_progressive(ctx);
    }
}

static void vp_index_scan_decode_packet(VPContext *ctx) {
    if (!ctx->index_scan_progressive) return;
    int ret = avcodec_send_packet(ctx->dec, ctx->pkt);
    if (ret == AVERROR(EAGAIN)) {
        vp_index_scan_drain(ctx);
        if (!ctx->index_scan_progressive) return;
        ret = avcodec_send_packet(ctx->dec, ctx->pkt);
    }
    if (ret < 0) { vp_index_disable_progressive(ctx); return; }
    vp_index_scan_drain(ctx);
}

static void vp_index_scan_flush_decoder(VPContext *ctx) {
    if (!ctx->index_scan_progressive) return;
    int ret = avcodec_send_packet(ctx->dec, NULL);
    if (ret == AVERROR(EAGAIN)) {
        vp_index_scan_drain(ctx);
        if (!ctx->index_scan_progressive) return;
        ret = avcodec_send_packet(ctx->dec, NULL);
    }
    if (ret >= 0) vp_index_scan_drain(ctx);
}
static int vp_index_scan_finish(VPContext *ctx) {
    vp_index_scan_flush_decoder(ctx);
    if (vp_index_sort_presentation_order(ctx) < 0) {
        ctx->index_scan_error = 1;
        ctx->index_scan_active = 0;
        return VP_ERR;
    }
    if (av_seek_frame(ctx->fmt, ctx->stream_idx, 0, AVSEEK_FLAG_BACKWARD) < 0) {
        ctx->index_scan_error = 1;
        ctx->index_scan_active = 0;
        return VP_ERR;
    }
    avcodec_flush_buffers(ctx->dec);
    vp_reset_decoder_state(ctx);
    ctx->index_scan_active = 0;
    ctx->index_scan_complete = 1;
    if (ctx->index_scan_progressive) ctx->index_stable_count = ctx->index_count;
    return VP_OK;
}

// Begin a demux scan. MPEG-TS may publish only records at or before the last
// timestamp retired by the decoder in presentation order. Other demuxers keep
// the complete-index behavior until they provide their own stability proof. packet_budget passed to step limits all demux
// packets read, including packets outside the selected video stream.
static int vp_index_scan_begin_internal(VPContext *ctx, int progressive) {
    if (!ctx || !ctx->fmt || !ctx->dec || ctx->stream_idx < 0 ||
        ctx->index_scan_started || ctx->index_import_streaming || ctx->index_count != 0) return VP_ERR;
    if (av_seek_frame(ctx->fmt, ctx->stream_idx, 0, AVSEEK_FLAG_BACKWARD) < 0) return VP_ERR;
    ctx->index_count = 0;
    ctx->index_stable_count = 0;
    ctx->index_next_order = 0;
    ctx->index_scan_progressive = progressive && ctx->fmt->iformat &&
        !strcmp(ctx->fmt->iformat->name, "mpegts");
    ctx->index_scan_last_output_ticks = INT64_MIN;
    ctx->index_seek_anchors = 0;
    ctx->index_scan_packets = 0;
    ctx->index_scan_bytes = 0;
    avcodec_flush_buffers(ctx->dec);
    vp_reset_decoder_state(ctx);
    ctx->index_scan_started = 1;
    ctx->index_scan_active = 1;
    ctx->index_scan_complete = 0;
    ctx->index_scan_error = 0;
    return VP_OK;
}

int vp_index_scan_begin(VPContext *ctx) {
    return vp_index_scan_begin_internal(ctx, 0);
}

int vp_index_scan_stream_begin(VPContext *ctx) {
    return vp_index_scan_begin_internal(ctx, 1);
}

// Read at most packet_budget demux packets. A non-negative return is the
// number read in this step; vp_index_scan_complete() reports EOF separately.
// Only MPEG-TS can currently publish stable record ranges during the scan.
int vp_index_scan_step(VPContext *ctx, int packet_budget) {
    if (!ctx || !ctx->index_scan_active || packet_budget <= 0) return VP_ERR;
    int processed = 0;
    while (processed < packet_budget) {
        int ret = av_read_frame(ctx->fmt, ctx->pkt);
        if (ret == AVERROR_EOF) {
            if (vp_index_scan_finish(ctx) < 0) return VP_ERR;
            return processed;
        }
        if (ret < 0) {
            av_packet_unref(ctx->pkt);
            ctx->index_scan_error = 1;
            ctx->index_scan_active = 0;
            return VP_ERR;
        }
        processed++;
        ctx->index_scan_packets++;
        int64_t packet_end = ctx->pkt->pos >= 0 && ctx->pkt->size > 0
            ? ctx->pkt->pos + ctx->pkt->size
            : (ctx->io ? ctx->io->pos : 0);
        if (packet_end > ctx->index_scan_bytes) ctx->index_scan_bytes = packet_end;
        if (ctx->io && ctx->io->pos > ctx->index_scan_bytes) ctx->index_scan_bytes = ctx->io->pos;
        if (ctx->pkt->stream_index == ctx->stream_idx) {
            if (ctx->index_scan_progressive && ctx->pkt->pts != AV_NOPTS_VALUE &&
                ctx->index_stable_count > 0 &&
                ctx->pkt->pts < ctx->index_ticks[ctx->index_stable_count - 1]) {
                // A backward timestamp/discontinuity invalidates the online
                // ordering proof. Continue the legacy full scan, but stop
                // publishing partial ranges so the server can reset clients.
                vp_index_disable_progressive(ctx);
            }
            if (ctx->pkt->pts != AV_NOPTS_VALUE) {
                int key = !!(ctx->pkt->flags & AV_PKT_FLAG_KEY);
                // MPEG-TS uses libavformat's binary timestamp seek. Other demuxers
                // own their index semantics (e.g. Matroska positions name clusters,
                // not packets), so never overwrite their entries with packet offsets.
                if (!strcmp(ctx->fmt->iformat->name, "mpegts") && key &&
                        ctx->pkt->pos >= 0 && ctx->pkt->dts != AV_NOPTS_VALUE) {
                    if (av_add_index_entry(ctx->fmt->streams[ctx->stream_idx], ctx->pkt->pos,
                            ctx->pkt->dts, ctx->pkt->size, 0, AVINDEX_KEYFRAME) < 0) {
                        av_packet_unref(ctx->pkt);
                        ctx->index_scan_error = 1;
                        ctx->index_scan_active = 0;
                        return VP_ERR;
                    }
                    ctx->index_seek_anchors++;
                }
                if (vp_index_push(ctx, ctx->pkt->pts, key, ctx->pkt->duration, ctx->pkt->dts, ctx->pkt->pos, ctx->pkt->size) < 0) {
                    av_packet_unref(ctx->pkt);
                    ctx->index_scan_error = 1;
                    ctx->index_scan_active = 0;
                    return VP_ERR;
                }
            }
            vp_index_scan_decode_packet(ctx);
        }
        av_packet_unref(ctx->pkt);
    }
    return processed;
}
int vp_index_scan_complete(VPContext *ctx) { return ctx && ctx->index_scan_complete; }
int vp_index_scan_failed(VPContext *ctx) { return ctx && ctx->index_scan_error; }
int vp_index_scan_progressive_supported(VPContext *ctx) { return ctx && ctx->index_scan_progressive; }
int vp_index_scan_stable_count(VPContext *ctx) {
    if (!ctx || !ctx->index_scan_progressive) return -1;
    return (int)ctx->index_stable_count;
}
int64_t vp_index_scan_stable_ticks(VPContext *ctx) {
    if (!ctx || !ctx->index_scan_progressive || ctx->index_stable_count == 0) return -1;
    return ctx->index_ticks[ctx->index_stable_count - 1];
}
int vp_index_scan_packets(VPContext *ctx) { return ctx ? ctx->index_scan_packets : 0; }
int64_t vp_index_scan_bytes(VPContext *ctx) { return ctx ? ctx->index_scan_bytes : 0; }

int vp_index_build(VPContext *ctx) {
    if (vp_index_scan_begin(ctx) < 0) return VP_ERR;
    while (!ctx->index_scan_complete && !ctx->index_scan_error) {
        if (vp_index_scan_step(ctx, 4096) < 0) return VP_ERR;
    }
    return ctx->index_scan_complete ? (int)ctx->index_count : VP_ERR;
}

int vp_index_count(VPContext *ctx) {
    return ctx && (ctx->index_scan_complete || ctx->index_import_streaming) ? (int)ctx->index_count : 0;
}
int vp_index_seek_anchors(VPContext *ctx) {
    return ctx && (ctx->index_scan_complete || ctx->index_import_streaming) ? ctx->index_seek_anchors : 0;
}
int vp_extract_frames(VPContext *ctx) { return ctx ? ctx->extract_frames : 0; }
int vp_extract_restarts(VPContext *ctx) { return ctx ? ctx->extract_restarts : 0; }
int64_t vp_index_ticks(VPContext *ctx, int i) {
    return ctx && (ctx->index_scan_complete || ctx->index_import_streaming) &&
        i >= 0 && (size_t)i < ctx->index_count ? ctx->index_ticks[i] : 0;
}
int vp_index_is_key(VPContext *ctx, int i) {
    return ctx && (ctx->index_scan_complete || ctx->index_import_streaming) &&
        i >= 0 && (size_t)i < ctx->index_count ? ctx->index_key[i] : 0;
}
int64_t vp_index_duration(VPContext *ctx, int i) {
    return ctx && (ctx->index_scan_complete || ctx->index_import_streaming) &&
        i >= 0 && (size_t)i < ctx->index_count ? ctx->index_duration[i] : 0;
}

#define VP_INDEX_FLAG_KEY 1u
#define VP_INDEX_FLAG_SEEK_ANCHOR 2u
#define VP_INDEX_KNOWN_FLAGS (VP_INDEX_FLAG_KEY | VP_INDEX_FLAG_SEEK_ANCHOR)

#ifndef VP_CORE_BUILD_ID
#define VP_CORE_BUILD_ID "unknown"
#endif
const char *vp_core_build_id(void) { return VP_CORE_BUILD_ID; }
int vp_stream_index(VPContext *ctx) { return ctx && ctx->stream_idx >= 0 ? ctx->stream_idx : -1; }

int vp_index_abi_version(void) { return 2; }
int vp_index_stream_abi_version(void) { return 1; }
int vp_index_record_bytes(void) { return (int)sizeof(VPIndexRecordV2); }
int vp_index_export_bytes(VPContext *ctx) {
    if (!ctx || !ctx->index_scan_complete || ctx->index_count > (size_t)(INT_MAX / (int)sizeof(VPIndexRecordV2))) return VP_ERR;
    return (int)(ctx->index_count * sizeof(VPIndexRecordV2));
}

static uint32_t vp_index_record_flags(VPContext *ctx, size_t i) {
    uint32_t flags = ctx->index_key[i] ? VP_INDEX_FLAG_KEY : 0u;
    if (flags && ctx->fmt && ctx->fmt->iformat &&
        !strcmp(ctx->fmt->iformat->name, "mpegts") &&
        ctx->index_pos[i] >= 0 && ctx->index_packet_size[i] >= 0 &&
        ctx->index_dts[i] != AV_NOPTS_VALUE) flags |= VP_INDEX_FLAG_SEEK_ANCHOR;
    return flags;
}

int vp_index_export_range(VPContext *ctx, int start, int count, void *output, int capacity_bytes) {
    if (!ctx || !ctx->index_scan_progressive || start < 0 || count <= 0 || !output ||
        count > INT_MAX / (int)sizeof(VPIndexRecordV2) ||
        (size_t)start > ctx->index_stable_count ||
        (size_t)count > ctx->index_stable_count - (size_t)start ||
        capacity_bytes < count * (int)sizeof(VPIndexRecordV2)) return VP_ERR;
    uint8_t *dst = (uint8_t *)output;
    for (int j = 0; j < count; j++) {
        size_t i = (size_t)start + (size_t)j;
        VPIndexRecordV2 record = {
            .pts = ctx->index_ticks[i],
            .dts = ctx->index_dts[i],
            .duration = ctx->index_duration[i],
            .pos = ctx->index_pos[i],
            .packet_size = ctx->index_packet_size[i],
            .flags = vp_index_record_flags(ctx, i)
        };
        memcpy(dst + (size_t)j * sizeof(record), &record, sizeof(record));
    }
    return count;
}

int vp_index_export(VPContext *ctx, void *output, int capacity_bytes) {
    int bytes = vp_index_export_bytes(ctx);
    if (bytes <= 0 || !output || capacity_bytes < bytes) return VP_ERR;
    uint8_t *dst = (uint8_t *)output;
    for (size_t i = 0; i < ctx->index_count; i++) {
        VPIndexRecordV2 record = {
            .pts = ctx->index_ticks[i],
            .dts = ctx->index_dts[i],
            .duration = ctx->index_duration[i],
            .pos = ctx->index_pos[i],
            .packet_size = ctx->index_packet_size[i],
            .flags = vp_index_record_flags(ctx, i)
        };
        memcpy(dst + i * sizeof(record), &record, sizeof(record));
    }
    return (int)ctx->index_count;
}

int vp_index_import_begin(VPContext *ctx) {
    if (!ctx || !ctx->dec || !ctx->fmt || ctx->stream_idx < 0 || ctx->index_count != 0 ||
        ctx->index_seek_anchors != 0 || ctx->index_scan_started || ctx->index_import_streaming) return VP_ERR;
    ctx->index_import_streaming = 1;
    ctx->index_import_has_safe_ticks = 0;
    ctx->index_import_next_seq = 0;
    ctx->index_import_safe_ticks = INT64_MIN;
    return VP_OK;
}

int vp_index_import_batch(VPContext *ctx, const void *input, int count, int seq,
                          int64_t safe_ticks, int final) {
    if (!ctx || !ctx->index_import_streaming || ctx->index_scan_complete ||
        seq != ctx->index_import_next_seq || count < 0 ||
        (count > 0 && !input) || (final != 0 && final != 1) ||
        count > INT_MAX / (int)sizeof(VPIndexRecordV2) ||
        (ctx->index_import_has_safe_ticks && safe_ticks < ctx->index_import_safe_ticks) ||
        (size_t)count > SIZE_MAX - ctx->index_count) return VP_ERR;
    if (count == 0 && !final) return VP_ERR;

    int is_mpegts = ctx->fmt->iformat && !strcmp(ctx->fmt->iformat->name, "mpegts");
    const uint8_t *src = (const uint8_t *)input;
    int64_t previous = ctx->index_count ? ctx->index_ticks[ctx->index_count - 1] : INT64_MIN;
    int anchors = 0;
    for (int j = 0; j < count; j++) {
        VPIndexRecordV2 record;
        memcpy(&record, src + (size_t)j * sizeof(record), sizeof(record));
        if ((record.flags & ~VP_INDEX_KNOWN_FLAGS) != 0 || record.packet_size < 0 ||
            record.pos < -1 ||
            ((record.flags & VP_INDEX_FLAG_SEEK_ANCHOR) &&
                (!(record.flags & VP_INDEX_FLAG_KEY) || !is_mpegts || record.pos < 0 ||
                 record.dts == AV_NOPTS_VALUE)) ||
            record.pts < previous || record.pts > safe_ticks) return VP_ERR;
        if (record.flags & VP_INDEX_FLAG_SEEK_ANCHOR) anchors++;
        previous = record.pts;
    }
    if (ctx->index_count + (size_t)count > (size_t)(INT_MAX / (int)sizeof(int64_t)) ||
        vp_index_reserve(ctx, ctx->index_count + (size_t)count) < 0) return VP_ERR;

    // av_add_index_entry may allocate internally. On failure the caller should
    // discard this context because earlier anchors from this batch may exist.
    if (anchors) {
        AVStream *stream = ctx->fmt->streams[ctx->stream_idx];
        for (int j = 0; j < count; j++) {
            VPIndexRecordV2 record;
            memcpy(&record, src + (size_t)j * sizeof(record), sizeof(record));
            if (!(record.flags & VP_INDEX_FLAG_SEEK_ANCHOR)) continue;
            if (av_add_index_entry(stream, record.pos, record.dts, record.packet_size, 0,
                                   AVINDEX_KEYFRAME) < 0) return VP_ERR;
        }
    }

    for (int j = 0; j < count; j++) {
        VPIndexRecordV2 record;
        memcpy(&record, src + (size_t)j * sizeof(record), sizeof(record));
        size_t i = ctx->index_count++;
        ctx->index_ticks[i] = record.pts;
        ctx->index_key[i] = (record.flags & VP_INDEX_FLAG_KEY) != 0;
        ctx->index_duration[i] = record.duration;
        ctx->index_dts[i] = record.dts;
        ctx->index_pos[i] = record.pos;
        ctx->index_packet_size[i] = record.packet_size;
        ctx->index_order[i] = ctx->index_next_order++;
    }
    ctx->index_seek_anchors += anchors;
    ctx->index_stable_count = ctx->index_count;
    ctx->index_import_safe_ticks = safe_ticks;
    ctx->index_import_has_safe_ticks = 1;
    ctx->index_import_next_seq++;
    ctx->index_scan_started = 1;
    if (final) {
        ctx->index_import_streaming = 0;
        ctx->index_scan_active = 0;
        ctx->index_scan_complete = 1;
        ctx->index_scan_error = 0;
    }
    return count;
}

int vp_index_import(VPContext *ctx, const void *input, int count) {
    if (!ctx || !input || count <= 0 || count > INT_MAX / (int)sizeof(VPIndexRecordV2)) return VP_ERR;
    VPIndexRecordV2 last;
    memcpy(&last, (const uint8_t *)input + ((size_t)count - 1) * sizeof(last), sizeof(last));
    if (vp_index_import_begin(ctx) < 0) return VP_ERR;
    return vp_index_import_batch(ctx, input, count, 0, last.pts, 1) < 0 ? VP_ERR : count;
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

static int vp_convert_rgba(VPContext *ctx) {
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
    return 0;
}

// ABI v2 owns tightly packed planes until the next output/reset/destroy.
// Layout 1 is planar YUV, 2 is semiplanar UV, 0 is explicit swscale RGBA.
static int vp_convert(VPContext *ctx) {
    AVFrame *frame = ctx->frame;
    const AVPixFmtDescriptor *desc = av_pix_fmt_desc_get(frame->format);
    VPFrameInfo planes = {0};
    int planar = desc && desc->nb_components == 3 &&
        !(desc->flags & (AV_PIX_FMT_FLAG_RGB | AV_PIX_FMT_FLAG_BE | AV_PIX_FMT_FLAG_HWACCEL | AV_PIX_FMT_FLAG_PAL | AV_PIX_FMT_FLAG_BITSTREAM)) &&
        (desc->flags & AV_PIX_FMT_FLAG_PLANAR) && desc->comp[0].plane == 0 && desc->comp[1].plane == 1 &&
        (desc->comp[2].plane == 2 || desc->comp[2].plane == 1);
    // Preserve existing HDR behavior until a high precision HDR renderer exists.
    if (frame->color_trc == AVCOL_TRC_SMPTE2084 || frame->color_trc == AVCOL_TRC_ARIB_STD_B67) planar = 0;
    // Formats/colorimetry outside the SDR renderer remain visibly tagged RGBA
    // fallback. Never reinterpret unsupported matrix/transfer as BT.709.
    if (frame->colorspace != AVCOL_SPC_UNSPECIFIED && frame->colorspace != AVCOL_SPC_BT709 && frame->colorspace != AVCOL_SPC_BT470BG && frame->colorspace != AVCOL_SPC_SMPTE170M && frame->colorspace != AVCOL_SPC_BT2020_NCL) planar = 0;
    if (frame->color_primaries != AVCOL_PRI_UNSPECIFIED && frame->color_primaries != AVCOL_PRI_BT709 && frame->color_primaries != AVCOL_PRI_BT470BG && frame->color_primaries != AVCOL_PRI_SMPTE170M && frame->color_primaries != AVCOL_PRI_BT2020) planar = 0;
    if (frame->color_trc != AVCOL_TRC_UNSPECIFIED && frame->color_trc != AVCOL_TRC_BT709 && frame->color_trc != AVCOL_TRC_SMPTE170M && frame->color_trc != AVCOL_TRC_IEC61966_2_1 && frame->color_trc != AVCOL_TRC_BT2020_10 && frame->color_trc != AVCOL_TRC_BT2020_12) planar = 0;
    int depth = desc ? desc->comp[0].depth : 0;
    if (depth < 8 || depth > 16) planar = 0;
    if (planar) {
        int bytes = depth > 8 ? 2 : 1;
        int semi = desc->comp[2].plane == 1;
        for (int c = 0; c < 3; c++) {
            if (desc->comp[c].depth != depth || desc->comp[c].shift != desc->comp[0].shift ||
                desc->comp[c].step != bytes * (semi && c ? 2 : 1) ||
                desc->comp[c].offset != (semi && c == 2 ? bytes : 0)) planar = 0;
        }
    }
    if (planar) {
        int bytes = depth > 8 ? 2 : 1, semi = desc->comp[2].plane == 1;
        planes.layout = semi ? 2 : 1; planes.bit_depth = depth;
        planes.bit_shift = desc->comp[0].shift;
        planes.subsample_x = desc->log2_chroma_w; planes.subsample_y = desc->log2_chroma_h;
        planes.chroma_location = frame->chroma_location;
        int64_t total = 0;
        for (int p = 0; p < (semi ? 2 : 3); p++) {
            int w = p ? AV_CEIL_RSHIFT(frame->width, desc->log2_chroma_w) : frame->width;
            int h = p ? AV_CEIL_RSHIFT(frame->height, desc->log2_chroma_h) : frame->height;
            int64_t stride = (int64_t)w * bytes * (semi && p ? 2 : 1);
            if (w <= 0 || h <= 0 || stride > INT_MAX || !frame->data[p] || llabs((int64_t)frame->linesize[p]) < stride) return VP_ERR;
            planes.planes[p].offset = total; planes.planes[p].stride = stride;
            planes.planes[p].width = w; planes.planes[p].height = h;
            total += stride * h;
            if (total > INT_MAX) return VP_ERR;
        }
        if (ctx->pixels_size < (size_t)total) {
            uint8_t *data = realloc(ctx->pixels, total);
            if (!data) return VP_ERR;
            ctx->pixels = data; ctx->pixels_size = total;
        }
        for (int p = 0; p < (semi ? 2 : 3); p++)
            for (int y = 0; y < planes.planes[p].height; y++)
                memcpy(ctx->pixels + planes.planes[p].offset + (size_t)y * planes.planes[p].stride,
                       frame->data[p] + (ptrdiff_t)y * frame->linesize[p], planes.planes[p].stride);
        planes.bytes = total;
    } else if (vp_convert_rgba(ctx) < 0) return VP_ERR;
    VPFrameInfo next = planes;
    next.pts = frame->best_effort_timestamp != AV_NOPTS_VALUE ? frame->best_effort_timestamp : frame->pts;
    next.duration = frame->duration;
    next.abi_version = 2; next.descriptor_bytes = sizeof(VPFrameInfo);
    next.width = frame->width; next.height = frame->height;
    if (!planar) { next.stride = frame->width * 4; next.bytes = next.stride * frame->height; }
    next.crop_left = frame->crop_left; next.crop_top = frame->crop_top;
    next.crop_right = frame->crop_right; next.crop_bottom = frame->crop_bottom;
    next.source_format = frame->format;
    next.primaries = frame->color_primaries; next.transfer = frame->color_trc;
    next.matrix = frame->colorspace; next.range = frame->color_range;
    next.sar_num = frame->sample_aspect_ratio.num > 0 ? frame->sample_aspect_ratio.num : 1;
    next.sar_den = frame->sample_aspect_ratio.den > 0 ? frame->sample_aspect_ratio.den : 1;
    // Metadata revision survives decoder resets and configuration changes.
    next.revision = ctx->output.revision;
    if (!next.revision || memcmp(&next.width, &ctx->output.width, sizeof(VPFrameInfo) - offsetof(VPFrameInfo, width))) next.revision++;
    ctx->output = next;
    return 0;
}

// Extracts the frame whose pts equals target_ticks exactly, converting it to
// RGBA in the pixel buffer. Sequential forward targets reuse decoder state;
// anything else seeks to the closest keyframe at or before the target.
int vp_extract(VPContext *ctx, int64_t target_ticks) {
    if (!ctx || !ctx->dec) return VP_ERR;
    ctx->extract_frames = ctx->extract_restarts = 0;
    if (ctx->have_frame && ctx->last_ticks == target_ticks) return VP_OK;

    int seek = ctx->decode_eof || target_ticks < ctx->last_ticks || !ctx->have_frame;
    if (!seek && ctx->index_count > 0) {
        // Walk forward only for nearby targets; jump via keyframe otherwise.
        size_t ahead = vp_index_lower_bound(ctx, ctx->last_ticks + 1);
        seek = (vp_index_lower_bound(ctx, target_ticks + 1) - ahead) > VP_MAX_FORWARD_WALK;
    }
    int restarted = 0;
    int64_t seek_ticks = target_ticks;
    if (ctx->index_count && ctx->index_seek_anchors) {
        size_t anchor = vp_index_lower_bound(ctx, target_ticks);
        if (anchor == ctx->index_count || ctx->index_ticks[anchor] > target_ticks) { if (anchor) anchor--; }
        while (anchor && !ctx->index_key[anchor]) anchor--;
        // One earlier GOP supplies open-GOP leading pictures and codec headers.
        // Cost depends on GOP length, not the target's distance from file start.
        if (anchor) { anchor--; while (anchor && !ctx->index_key[anchor]) anchor--; }
        if (ctx->index_key[anchor] && ctx->index_dts[anchor] != AV_NOPTS_VALUE)
            seek_ticks = ctx->index_dts[anchor];
    }
    for (;;) {
        if (seek) {
            // Seek-by-byte-estimation demuxers (e.g. MPEG-TS) can land past
            // the target; on overshoot restart once from the beginning.
            if (av_seek_frame(ctx->fmt, ctx->stream_idx, restarted ? 0 : seek_ticks,
                              AVSEEK_FLAG_BACKWARD) < 0) {
                ctx->extract_restarts++;
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
            if (!restarted) { ctx->extract_restarts++; restarted = 1; seek = 1; continue; }
            return VP_EOF;
        }
        if (ret != VP_OK) return VP_ERR;
        ctx->extract_frames++;
        int64_t ticks = ctx->frame->best_effort_timestamp;
        if (ticks == AV_NOPTS_VALUE || ticks < target_ticks) continue;
        if (ticks > target_ticks) {
            if (!restarted) { ctx->extract_restarts++; restarted = 1; seek = 1; continue; }
            return VP_MISMATCH;
        }
        if (vp_convert(ctx) < 0) return VP_ERR;
        ctx->last_ticks = ticks;
        ctx->have_frame = 1;
        return VP_OK;
    }
}

// Decode and retain the first displayable frame without building a packet index.
// The decoder is already primed by vp_open_decoders(); this advances from the
// start and skips negative-time preroll, matching the browser timeline origin.
int vp_prime_first_presentable(VPContext *ctx) {
    if (!ctx || !ctx->dec) return VP_ERR;
    if (ctx->have_frame) return VP_OK;
    for (;;) {
        int ret = vp_decode_one(ctx);
        if (ret != VP_OK) return ret;
        int64_t ticks = ctx->frame->best_effort_timestamp;
        if (ticks == AV_NOPTS_VALUE || ticks < 0) continue;
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
