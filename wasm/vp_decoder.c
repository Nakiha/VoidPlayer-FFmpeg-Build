// VoidPlayer web fallback decoder core.
//
// Direct libavformat/libavcodec/libswscale access for the browser prototype's
// WASM fallback path. No CLI, no filters: one context per media source, an
// explicit frame index, and exact-PTS frame extraction with decoder-state
// continuation for sequential stepping.

#include <libavcodec/avcodec.h>
#include <libavformat/avformat.h>
#include <libavutil/avutil.h>
#include <libswscale/swscale.h>

#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#define VP_EOF 0
#define VP_OK 1
#define VP_MISMATCH 2
#define VP_ERR (-1)

// Seeks instead of walking forward when the target is further ahead than this
// many index frames; GOP-length agnostic approximation.
#define VP_MAX_FORWARD_WALK 8

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
    int64_t last_ticks;      // pts of the frame currently in pixels
    int have_frame;          // pixels holds last_ticks
    int decode_eof;          // decoder drained; further reads yield nothing
} VPContext;

static void vp_reset_decoder_state(VPContext *ctx) {
    ctx->decode_eof = 0;
    ctx->have_frame = 0;
}

void vp_close_input(VPContext *ctx);
static int vp_decode_one(VPContext *ctx);

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
    ctx->stream_idx = -1;
    ctx->index_count = 0;
    vp_reset_decoder_state(ctx);
}

int vp_open(VPContext *ctx, const char *path) {
    if (!ctx || !path) return VP_ERR;
    vp_close_input(ctx);
    if (avformat_open_input(&ctx->fmt, path, NULL, NULL) < 0) return VP_ERR;
    if (avformat_find_stream_info(ctx->fmt, NULL) < 0) return VP_ERR;

    const AVCodec *codec = NULL;
    ctx->stream_idx = av_find_best_stream(ctx->fmt, AVMEDIA_TYPE_VIDEO, -1, -1, &codec, 0);
    if (ctx->stream_idx < 0 || !codec) return VP_ERR;

    ctx->dec = avcodec_alloc_context3(codec);
    if (!ctx->dec) return VP_ERR;
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

static int vp_ensure_pixels(VPContext *ctx, int width, int height) {
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
    sws_scale(ctx->sws, (const uint8_t *const *)frame->data, frame->linesize,
              0, frame->height, dst, dst_stride);
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
        size_t ahead = 0;
        for (size_t i = 0; i < ctx->index_count && ctx->index_ticks[i] <= target_ticks; i++) {
            if (ctx->index_ticks[i] > ctx->last_ticks) ahead++;
        }
        seek = ahead > VP_MAX_FORWARD_WALK;
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
uint8_t *vp_pixels(VPContext *ctx) { return ctx ? ctx->pixels : NULL; }
