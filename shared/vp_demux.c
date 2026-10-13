#include "vp_demux.h"
#include <stdlib.h>
#include <string.h>
#include <limits.h>
#include <libavutil/mem.h>
#include <emscripten.h>
EM_JS(long, vp_js_read, (int handle, double offset, uint8_t *buf, int len), {
    if (Module.vpDemuxRead && handle < 0) {
        const bytes = Module.vpDemuxRead(-handle, offset, len);
        if (!bytes || bytes.length > len) return -1;
        HEAPU8.set(bytes, buf); return bytes.length;
    }
    const entry = Module.vpBlobs && Module.vpBlobs.get(handle);
    if (!entry) return -1;
    const bytes = new Uint8Array(entry.reader.readAsArrayBuffer(entry.blob.slice(offset, offset + len)));
    HEAPU8.set(bytes, buf); return bytes.length;
});
int vp_avio_read(void *opaque, uint8_t *buf, int size) {
    VpBlobIO *io = opaque;
    if (size <= 0 || io->pos < 0 || io->pos > io->size) return AVERROR(EINVAL);
    int want = io->size - io->pos < size ? (int)(io->size - io->pos) : size;
    if (!want) return AVERROR_EOF;
    long got = vp_js_read(io->handle, (double)io->pos, buf, want);
    if (got <= 0 || got > want) return AVERROR(EIO);
    io->pos += got; io->bytes += got; return got;
}
int64_t vp_avio_seek(void *opaque, int64_t offset, int whence) {
    VpBlobIO *io = opaque;
    if (whence == AVSEEK_SIZE) return io->size;
    int base = whence & ~AVSEEK_FORCE;
    int64_t origin = base == SEEK_SET ? 0 : base == SEEK_CUR ? io->pos : base == SEEK_END ? io->size : -1;
    if (origin < 0 || (offset > 0 && origin > INT64_MAX - offset) || (offset < 0 && offset < -origin)) return AVERROR(EINVAL);
    int64_t pos = origin + offset;
    if (pos < 0 || pos > io->size) return AVERROR(EINVAL);
    io->pos = pos; return pos;
}
void vp_blob_close(AVFormatContext **fmt, AVIOContext **pb, VpBlobIO **io) {
    avformat_close_input(fmt);
    if (*pb) { av_freep(&(*pb)->buffer); avio_context_free(pb); }
    free(*io); *io = NULL;
}
int vp_blob_open(AVFormatContext **fmt, AVIOContext **pb, VpBlobIO **io, int handle, int64_t size) {
    if (size <= 0) return AVERROR(EINVAL);
    *io = calloc(1, sizeof(**io)); if (!*io) return AVERROR(ENOMEM);
    (*io)->handle = handle; (*io)->size = size;
    uint8_t *buffer = av_malloc(256*1024);
    if (!buffer) goto failed;
    *pb = avio_alloc_context(buffer, 256*1024, 0, *io, vp_avio_read, NULL, vp_avio_seek);
    if (!*pb) { av_free(buffer); goto failed; }
    *fmt = avformat_alloc_context(); if (!*fmt) goto failed;
    (*fmt)->pb = *pb; (*fmt)->flags |= AVFMT_FLAG_CUSTOM_IO;
    if (avformat_open_input(fmt, NULL, NULL, NULL) < 0) goto failed;
    return 0;
failed:
    vp_blob_close(fmt, pb, io); return AVERROR(EIO);
}
int vp_select_video(AVFormatContext *fmt, const AVCodec **codec) {
    if (avformat_find_stream_info(fmt, NULL) < 0) return AVERROR_INVALIDDATA;
    int selected=av_find_best_stream(fmt, AVMEDIA_TYPE_VIDEO, -1, -1, codec, 0);
    // Preserve missing PES timestamp state for the admitted AVC/TS path.
    // Parsing still reassembles AU payload; no guessed PTS/DTS enters identity.
    if(selected>=0 && !strcmp(fmt->iformat->name,"mpegts") && fmt->streams[selected]->codecpar->codec_id==AV_CODEC_ID_H264)
        fmt->flags |= AVFMT_FLAG_NOFILLIN;
    return selected;
}
int vp_packet_tuple(const AVPacket *p, const VPIndexRecordV3 *r) {
    return p->pts == r->pts && p->dts == r->dts && p->pos == r->pos && p->size == r->packet_size;
}
uint64_t vp_packet_hash(const uint8_t *p, int n) {
    uint64_t hash = UINT64_C(14695981039346656037);
    for (int i=0; i<n; i++) { hash ^= p[i]; hash *= UINT64_C(1099511628211); }
    return hash;
}
static int prefix(const uint8_t *p, int n, int at) {
    if (at+3 <= n && !p[at] && !p[at+1]) {
        if (p[at+2] == 1) return 3;
        if (at+4 <= n && !p[at+2] && p[at+3] == 1) return 4;
    }
    return 0;
}
int vp_h264_single(const uint8_t *p, int n) {
    int slices=0, at=0;
    while (at<n && !prefix(p,n,at)) { if (p[at++]) return 0; }
    while (at<n) {
        int k=prefix(p,n,at); if (!k) return 0;
        int start=at+k, end=start; while (end<n && !prefix(p,n,end)) end++;
        if (start == end || p[start]&128) return 0;
        int type=p[start]&31;
        if (type==2 || type==3 || type==4 || type==14 || type==20 || type==21) return 0;
        if (type==1 || type==5) { if (end-start<2 || !(p[start+1]&128) || ++slices>1) return 0; }
        at=end;
    }
    return slices==1;
}

int vp_h264_admit(VPH264Admission *state, const uint8_t *p, int n) {
    if(!state || state->changed)return 0;
    int at=0; while(at<n && !prefix(p,n,at)) { if(p[at++]){state->changed=1;return 0;} }
    while(at<n) {
        int start=at+prefix(p,n,at),end=start; while(end<n && !prefix(p,n,end))end++;
        int last=end;while(last>start && !p[last-1])last--;
        if(last>start && ((p[start]&31)==7 || (p[start]&31)==8)) {
            int size=last-start, sps=(p[start]&31)==7;
            uint8_t *saved=sps?state->sps:state->pps; int *saved_size=sps?&state->sps_size:&state->pps_size;
            if(size>4096 || (*saved_size && (*saved_size!=size || memcmp(saved,p+start,size)))){state->changed=1;return 0;}
            if(!*saved_size){memcpy(saved,p+start,size);*saved_size=size;}
        }
        at=end;
    }
    return state->sps_size && state->pps_size && vp_h264_single(p,n);
}

int vp_rewind_start(AVFormatContext *fmt,int stream) {
    // Timestamp seeking can skip an untimed first PES. Full source enumeration
    // must rewind by byte and reset parser buffers for the admitted TS path.
    if(!strcmp(fmt->iformat->name,"mpegts") && fmt->streams[stream]->codecpar->codec_id==AV_CODEC_ID_H264)
        return av_seek_frame(fmt,-1,0,AVSEEK_FLAG_BYTE);
    return av_seek_frame(fmt,stream,0,AVSEEK_FLAG_BACKWARD);
}
