#include <libavcodec/avcodec.h>
#ifndef VP_NO_CONTAINER_DEMUX
#include <libavformat/avformat.h>
#endif
#ifndef VP_CORE_BUILD_ID
#define VP_CORE_BUILD_ID "unknown"
#endif

/* Bounded, read-only inspection of the registry linked into this exact WASM.
 * This exposes no container/packet input or decoder context. */
int vp_capability_abi(void) { return 1; }
const char *vp_build_id(void) { return VP_CORE_BUILD_ID; }
int vp_demuxer_count(void) {
#ifdef VP_NO_CONTAINER_DEMUX
    return 0;
#else
    void *cursor = NULL; int count = 0;
    while (av_demuxer_iterate(&cursor)) count++;
    return count;
#endif
}
const char *vp_demuxer_name(int index) {
#ifdef VP_NO_CONTAINER_DEMUX
    return NULL;
#else
    if (index < 0 || index >= 1024) return NULL;
    void *cursor = NULL; const AVInputFormat *format;
    while ((format = av_demuxer_iterate(&cursor))) if (!index--) return format->name;
    return NULL;
#endif
}
int vp_decoder_count(void) {
    void *cursor = NULL; const AVCodec *codec; int count = 0;
    while ((codec = av_codec_iterate(&cursor))) if (av_codec_is_decoder(codec)) count++;
    return count;
}
const char *vp_decoder_name(int index) {
    if (index < 0 || index >= 1024) return NULL;
    void *cursor = NULL; const AVCodec *codec;
    while ((codec = av_codec_iterate(&cursor))) if (av_codec_is_decoder(codec) && !index--) return codec->name;
    return NULL;
}
