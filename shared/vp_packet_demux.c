/* Independent format/AVIO/cursor. No playback context or pixel descriptor. */
#include "vp_demux.h"
#include <stdlib.h>
#include <string.h>
#include <limits.h>
typedef struct {
    int64_t pts,dts,duration,pos; uint64_t ordinal;
    int32_t stream; uint32_t flags;
    uint32_t payload; int32_t size,tb_num,tb_den;
    uint64_t hash;
} VPDPacket;
_Static_assert(sizeof(VPDPacket)==72,"demux descriptor ABI");
typedef struct {
    AVFormatContext *fmt; AVIOContext *pb; VpBlobIO *io; AVPacket *pkt;
    int stream,eof; uint64_t next, packets;
    VPIndexRecordV3 *records; int count; VPDPacket out;
} VPDContext;
#ifndef VP_CORE_BUILD_ID
#define VP_CORE_BUILD_ID "unknown"
#endif
const char *vpd_build_id(void) { return VP_CORE_BUILD_ID; }
static int live_contexts;
int vpd_live_contexts(void) { return live_contexts; }
int vpd_abi(void) { return 1; }
const char *vpd_compat_id(void) { return "ffmpeg-9.0.2-avformat-740392267b71-ts-raw-input-v2"; }
VPDContext *vpd_open(int handle, int64_t size) {
    VPDContext *c=calloc(1,sizeof(*c)); if(!c)return NULL;
    live_contexts++;
    c->pkt=av_packet_alloc();
    if(!c->pkt || vp_blob_open(&c->fmt,&c->pb,&c->io,handle,size)<0)goto failed;
    c->stream=vp_select_video(c->fmt,NULL); if(c->stream<0)goto failed;
    if(vp_rewind_start(c->fmt,c->stream)<0)goto failed;
    return c;
failed:
    vp_blob_close(&c->fmt,&c->pb,&c->io); av_packet_free(&c->pkt); free(c); live_contexts--; return NULL;
}
void vpd_close(VPDContext *c) { if(!c)return; vp_blob_close(&c->fmt,&c->pb,&c->io); av_packet_free(&c->pkt); free(c->records); free(c); live_contexts--; }
int vpd_stream(VPDContext *c) { return c?c->stream:-1; }
int vpd_video_count(VPDContext *c) { int n=0; if(c)for(unsigned i=0;i<c->fmt->nb_streams;i++)n+=c->fmt->streams[i]->codecpar->codec_type==AVMEDIA_TYPE_VIDEO; return n; }
const char *vpd_codec(VPDContext *c) { return c?avcodec_get_name(c->fmt->streams[c->stream]->codecpar->codec_id):""; }
const char *vpd_format(VPDContext *c) { return c?c->fmt->iformat->name:""; }
int vpd_extra_size(VPDContext *c) { return c?c->fmt->streams[c->stream]->codecpar->extradata_size:0; }
uint8_t *vpd_extra(VPDContext *c) { return c?c->fmt->streams[c->stream]->codecpar->extradata:NULL; }
int vpd_tb_num(VPDContext *c) { return c?c->fmt->streams[c->stream]->time_base.num:0; }
int vpd_tb_den(VPDContext *c) { return c?c->fmt->streams[c->stream]->time_base.den:0; }
int64_t vpd_bytes(VPDContext *c) { return c?c->io->bytes:0; }
int64_t vpd_packets(VPDContext *c) { return c?c->packets:0; }
void vpd_release(VPDContext *c) { if(c){av_packet_unref(c->pkt);memset(&c->out,0,sizeof(c->out));} }
const VPDPacket *vpd_packet(VPDContext *c) { return c&&c->out.payload?&c->out:NULL; }
int vpd_bind_index(VPDContext *c,const VPIndexRecordV3 *r,int count) {
    if(!c || !r || count<1 || count>2000000 || c->records)return -1;
    VPIndexRecordV3 *copy=calloc(count,sizeof(*copy)); uint8_t *seen=calloc(count,1);
    if(!copy || !seen){free(copy);free(seen);return -1;}
    for(int i=0;i<count;i++) {
        if(r[i].ordinal>=(unsigned)count || seen[r[i].ordinal] || r[i].packet_size<0 || r[i].pos< -1 || (r[i].flags&~3u)){free(copy);free(seen);return -1;}
        seen[r[i].ordinal]=1; copy[r[i].ordinal]=r[i];
    }
    free(seen); c->records=copy;c->count=count;
    if(!strcmp(c->fmt->iformat->name,"mpegts"))for(int i=0;i<count;i++)if(copy[i].flags&2)
        if(av_add_index_entry(c->fmt->streams[c->stream],copy[i].pos,copy[i].dts,copy[i].packet_size,0,AVINDEX_KEYFRAME)<0)return -1;
    return 0;
}
/* 1 packet, 0 EOF, 2 budget spent on other streams, negative error.
 * ordinal flag is withheld unless the source tuple is uniquely restored. */
int vpd_read(VPDContext *c,int budget) {
    if(!c || budget<1 || budget>4096 || c->out.payload)return -1;
    if(c->eof)return 0;
    for(int i=0;i<budget;i++) {
        int ret=vp_demux_read_frame(c->fmt,c->pkt);
        if(ret==AVERROR_EOF){c->eof=1;return 0;}
        if(ret<0)return ret;
        c->packets++;
        if(c->pkt->stream_index!=c->stream){av_packet_unref(c->pkt);continue;}
        if(c->pkt->size<1 || c->pkt->size>8*1024*1024){av_packet_unref(c->pkt);return AVERROR(EINVAL);}
        uint64_t ordinal=c->next;
        if(c->records) {
            if(ordinal<(unsigned)c->count) { if(!vp_packet_tuple(c->pkt,&c->records[ordinal]))ordinal=UINT64_MAX; }
            else {
                ordinal=UINT64_MAX;
                for(int j=0;j<c->count;j++)if(vp_packet_tuple(c->pkt,&c->records[j])) {
                    if(ordinal!=UINT64_MAX){ordinal=UINT64_MAX;break;} ordinal=j;
                }
            }
        }
        c->next=ordinal==UINT64_MAX?UINT64_MAX:ordinal+1;
        AVStream *s=c->fmt->streams[c->stream];
        c->out=(VPDPacket){c->pkt->pts,c->pkt->dts,c->pkt->duration,c->pkt->pos,ordinal,c->stream,
            (c->pkt->pts!=AV_NOPTS_VALUE?1u:0)|(c->pkt->dts!=AV_NOPTS_VALUE?2u:0)|(ordinal!=UINT64_MAX?4u:0)|
            (c->pkt->flags&AV_PKT_FLAG_KEY?8u:0)|(c->pkt->flags&AV_PKT_FLAG_CORRUPT?16u:0),
            (uint32_t)(uintptr_t)c->pkt->data,c->pkt->size,s->time_base.num,s->time_base.den,vp_packet_hash(c->pkt->data,c->pkt->size)};
        return 1;
    }
    return 2;
}
int vpd_seek(VPDContext *c,int64_t dts) {
    if(!c || !c->records || dts==AV_NOPTS_VALUE)return -1;
    vpd_release(c); c->eof=0;c->next=UINT64_MAX;
    return vp_demux_seek_frame(c->fmt,c->stream,dts,AVSEEK_FLAG_BACKWARD);
}
