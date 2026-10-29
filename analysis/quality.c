/* LGPL-2.1-or-later. Pixel-only scoring ABI for the existing analysis module.
 * No demux, decoder, timestamps, filesystem or libavfilter. One scoring context
 * per module instance; callers own frame identity and continuous presentation order. */
#include <errno.h>
#include <math.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <libvmaf/libvmaf.h>

#define QUALITY_VMAF 1
#define QUALITY_CAMBI 2
#define QUALITY_SCALES 5
#define QUALITY_MAX_FRAMES 1000000
#define QUALITY_MAX_DIMENSION 4096
#define QUALITY_MAX_PIXELS (4096 * 2304)

typedef struct {
    uint16_t *data;
    unsigned width, height;
} Heatmap;

static struct {
    VmafContext *context;
    VmafModel *model;
    unsigned metrics, width, height, depth, frames, max_frames;
    unsigned input_bytes;
    int flushed, failed, error, heat_frame;
    Heatmap heatmaps[QUALITY_SCALES];
} quality;

int vpa_quality_abi(void) { return 1; }
int vpa_quality_error(void) { return quality.error; }

static int fail(int code) {
    quality.error = code;
    return code;
}

int vpa_quality_close(void) {
    int err = quality.context ? vmaf_close(quality.context) : 0;
    if (quality.model) vmaf_model_destroy(quality.model);
    for (unsigned i = 0; i < QUALITY_SCALES; i++) free(quality.heatmaps[i].data);
    memset(&quality, 0, sizeof(quality));
    quality.heat_frame = -1;
    return fail(err);
}

/* model: 0 = v0.6.1, 1 = v0.6.1neg. Unselected algorithms allocate no state.
 * max_frames is the explicit submission budget, not a temporal chunk size. */
int vpa_quality_open(unsigned metrics, unsigned model, unsigned width,
                     unsigned height, unsigned depth, unsigned max_frames) {
    if (quality.context) return fail(-EBUSY);
    if (!metrics || (metrics & ~(QUALITY_VMAF | QUALITY_CAMBI)) || model > 1 ||
        width < 64 || height < 64 || width > QUALITY_MAX_DIMENSION ||
        height > QUALITY_MAX_DIMENSION || width * height > QUALITY_MAX_PIXELS ||
        (width & 1) || (height & 1) || (depth != 8 && depth != 10) ||
        !max_frames || max_frames > QUALITY_MAX_FRAMES ||
        ((metrics & QUALITY_CAMBI) && width < 320 && height < 320))
        return fail(-EINVAL);
    memset(&quality, 0, sizeof(quality));
    quality.heat_frame = -1;
    quality.metrics = metrics;
    quality.width = width;
    quality.height = height;
    quality.depth = depth;
    quality.max_frames = max_frames;
    quality.input_bytes = width * height * 3 / 2 * (depth == 8 ? 1 : 2);
    VmafConfiguration config = {
        .log_level = VMAF_LOG_LEVEL_NONE, .n_threads = 0,
        .n_subsample = 1, .cpumask = 0,
    };
    int err = vmaf_init(&quality.context, config);
    if (err) goto cleanup;
    if (metrics & QUALITY_VMAF) {
        VmafModelConfig cfg = {.name = "vmaf", .flags = 0};
        err = vmaf_model_load(&quality.model, &cfg,
                             model ? "vmaf_v0.6.1neg" : "vmaf_v0.6.1");
        if (err) goto cleanup;
        err = vmaf_use_features_from_model(quality.context, quality.model);
        if (err) goto cleanup;
    }
    if (metrics & QUALITY_CAMBI) {
        VmafFeatureDictionary *options = NULL;
        err = vmaf_feature_dictionary_set(&options, "window_size", "63");
        if (!err) err = vmaf_use_feature(quality.context, "cambi", options);
        if (err) { vmaf_feature_dictionary_free(&options); goto cleanup; }
        unsigned w = width, h = height;
        for (unsigned i = 0; i < QUALITY_SCALES; i++) {
            quality.heatmaps[i] = (Heatmap){.data = malloc(w * h * sizeof(uint16_t)),
                                          .width = w, .height = h};
            if (!quality.heatmaps[i].data) { err = -ENOMEM; goto cleanup; }
            w = (w + 1) / 2;
            h = (h + 1) / 2;
        }
    }
    return 0;
cleanup:
    vpa_quality_close();
    return fail(err);
}

/* Called synchronously by the pinned CAMBI memory-output patch. Same uint16
 * conversion as official file output; only the destination changes. */
int vp_cambi_heatmaps_enabled(void) {
    return quality.context && (quality.metrics & QUALITY_CAMBI);
}
int vp_cambi_heatmap_row(const uint16_t *values, int width, int height,
                         int scale, int row, int frame) {
    if (!vp_cambi_heatmaps_enabled() || scale < 0 || scale >= QUALITY_SCALES ||
        frame < 0 || (unsigned)frame != quality.frames || row < 0 || row >= height)
        return -EINVAL;
    Heatmap *map = &quality.heatmaps[scale];
    if ((unsigned)width != map->width || (unsigned)height != map->height || !map->data)
        return -EINVAL;
    memcpy(map->data + (size_t)row * width, values, width * sizeof(uint16_t));
    return 0;
}

static int copy_picture(VmafPicture *picture, const uint8_t *bytes) {
    int err = vmaf_picture_alloc(picture, VMAF_PIX_FMT_YUV420P, quality.depth,
                                quality.width, quality.height);
    if (err) return err;
    for (unsigned plane = 0; plane < 3; plane++) {
        unsigned width = (plane ? quality.width / 2 : quality.width) *
                         (quality.depth == 8 ? 1 : 2);
        unsigned height = plane ? quality.height / 2 : quality.height;
        for (unsigned y = 0; y < height; y++) {
            memcpy((uint8_t *)picture->data[plane] + y * picture->stride[plane], bytes, width);
            bytes += width;
        }
    }
    return 0;
}

/* Packed source Y/U/V, native bit depth, no conversion. The library takes the
 * allocated pictures, never the caller buffers. A failed feed poisons this
 * context: the caller must close it instead of retrying an uncertain index. */
int vpa_quality_feed(const uint8_t *reference, const uint8_t *distorted,
                     unsigned bytes, unsigned index) {
    if (!quality.context || quality.flushed || quality.failed) return fail(-EINVAL);
    if (!reference || !distorted || bytes != quality.input_bytes ||
        index != quality.frames || index >= quality.max_frames) return fail(-EINVAL);
    quality.heat_frame = -1;
    VmafPicture ref = {0}, dist = {0};
    int err = copy_picture(&ref, reference);
    if (err) return fail(err);
    err = copy_picture(&dist, distorted);
    if (err) { vmaf_picture_unref(&ref); return fail(err); }
    err = vmaf_read_pictures(quality.context, &ref, &dist, index);
    if (err) {
        /* Early extractor failures leave caller references owned by us; success
         * clears both pictures. Temporal references are released by close. */
        if (ref.ref) vmaf_picture_unref(&ref);
        if (dist.ref) vmaf_picture_unref(&dist);
        quality.failed = 1;
        return fail(err);
    }
    if (quality.metrics & QUALITY_CAMBI) quality.heat_frame = (int)index;
    quality.frames++;
    return fail(0);
}
int vpa_quality_flush(void) {
    if (!quality.context || quality.failed || !quality.frames) return fail(-EINVAL);
    if (quality.flushed) return 0;
    int err = vmaf_read_pictures(quality.context, NULL, NULL, 0);
    if (err) { quality.failed = 1; return fail(err); }
    quality.flushed = 1;
    return fail(0);
}

double vpa_quality_score(unsigned metric, unsigned index) {
    double score = NAN;
    if (!quality.context || quality.failed || index >= quality.frames ||
        !(quality.metrics & metric) || (metric != QUALITY_VMAF && metric != QUALITY_CAMBI)) {
        fail(-EINVAL); return NAN;
    }
    int err = metric == QUALITY_VMAF ?
        vmaf_score_at_index(quality.context, quality.model, &score, index) :
        vmaf_feature_score_at_index(quality.context, "cambi", &score, index);
    fail(err);
    return err ? NAN : score;
}
/* pool uses the upstream enum: min=1, max=2, mean=3, harmonic_mean=4. */
double vpa_quality_pool(unsigned metric, unsigned pool, unsigned start, unsigned end) {
    double score = NAN;
    if (!quality.context || quality.failed || !quality.flushed || start > end ||
        end >= quality.frames || pool < 1 || pool > 4 || !(quality.metrics & metric) ||
        (metric != QUALITY_VMAF && metric != QUALITY_CAMBI)) {
        fail(-EINVAL); return NAN;
    }
    int err = metric == QUALITY_VMAF ?
        vmaf_score_pooled(quality.context, quality.model, pool, &score, start, end) :
        vmaf_feature_score_pooled(quality.context, "cambi", pool, &score, start, end);
    fail(err);
    return err ? NAN : score;
}
int vpa_quality_heat_frame(void) { return quality.heat_frame; }
uintptr_t vpa_quality_heat_data(unsigned scale) {
    return scale < QUALITY_SCALES && quality.heat_frame >= 0 ?
        (uintptr_t)quality.heatmaps[scale].data : 0;
}
unsigned vpa_quality_heat_width(unsigned scale) {
    return scale < QUALITY_SCALES && quality.heat_frame >= 0 ? quality.heatmaps[scale].width : 0;
}
unsigned vpa_quality_heat_height(unsigned scale) {
    return scale < QUALITY_SCALES && quality.heat_frame >= 0 ? quality.heatmaps[scale].height : 0;
}
