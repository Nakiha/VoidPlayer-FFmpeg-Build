/* LGPL-2.1-or-later. Streaming collector for the pinned instrumented FFmpeg.
 * One context per WASM instance. Records seal only on avcodec_receive_frame;
 * decoder reference frames remain owned by FFmpeg after records are taken. */
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include "libavcodec/avcodec.h"
#include "libavcodec/voidplayer_vachunk.h"
#include "libavutil/pixdesc.h"
#include "libavutil/error.h"
#include "libavutil/mem.h"
int vpa_abi(void); void vpa_close(void); int vpa_open(int codec,const uint8_t *extra,int size);
int vpa_feed(const uint8_t *bytes,int size,double pts,double dts,int ordinal);
int vpa_drain(void); int vpa_step(void); int vpa_count(void); int vpa_blocks(void);
int vpa_depth(void); int vpa_ordinal(void); int vpa_width(void); int vpa_height(void); int vpa_invalid(void);
double vpa_pts(void); int vpa_take(int maxBytes); int vpa_record_bytes(void);
#define MAX_PICTURES 32
#define MAX_BLOCKS 131072
#define MAX_RECORD_BYTES (32*1024*1024)
typedef struct { uint16_t x,y,w,h; int16_t qp; uint8_t mode,depth; } Block;
typedef struct { uintptr_t id; Block *blocks; uint32_t count,capacity,width,height; int sealed,invalid,depth; int64_t pts; } Picture;
static Picture pictures[MAX_PICTURES];
static AVCodecContext *decoder;
static AVFrame *frame;
static size_t allocated;
static int failure;
static Picture *ready;
static Picture *find(uintptr_t id,int create) {
    if (!id) { failure=AVERROR_INVALIDDATA; return NULL; }
    for(int i=0;i<MAX_PICTURES;i++) if(pictures[i].id==id) return &pictures[i];
    if(create) for(int i=0;i<MAX_PICTURES;i++) if(!pictures[i].id) { pictures[i].id=id; return &pictures[i]; }
    failure=AVERROR(ENOMEM); return NULL;
}
static void release(Picture *p) { if(!p)return; allocated-=p->capacity*sizeof(Block);free(p->blocks);memset(p,0,sizeof(*p)); }
static void append(const VoidPlayerVachunkFrameInfo *info,uint16_t x,uint16_t y,uint16_t w,uint16_t h,uint8_t qp,uint8_t mode,uint8_t depth) {
    if(!decoder||failure)return;
    if(info->width>4096||info->height>2304){failure=AVERROR(ENOMEM);return;}
    Picture *p=find(info->frame_identity,1); if(!p)return;
    if(p->sealed){failure=AVERROR_INVALIDDATA;return;}
    p->width=info->width;p->height=info->height;
    if(!w||!h||x+w>p->width||y+h>p->height){p->invalid=1;return;}
    if(p->count>=MAX_BLOCKS){failure=AVERROR(ENOMEM);return;}
    if(p->count==p->capacity){
        uint32_t capacity=p->capacity?p->capacity*2:256;
        size_t extra=(capacity-p->capacity)*sizeof(Block);
        if(allocated+extra>MAX_RECORD_BYTES){failure=AVERROR(ENOMEM);return;}
        Block *b=realloc(p->blocks,capacity*sizeof(Block));if(!b){failure=AVERROR(ENOMEM);return;}
        p->blocks=b;p->capacity=capacity;allocated+=extra;
    }
    p->blocks[p->count++]=(Block){x,y,w,h,qp,mode,depth};
}
int ff_voidplayer_vachunk_is_active(void){return decoder!=NULL&&!failure;}
void ff_voidplayer_vachunk_write_intra_cu(const VoidPlayerVachunkFrameInfo *i,uint16_t x,uint16_t y,uint8_t w,uint8_t h,uint8_t d,uint8_t q,uint8_t intra,uint8_t mip,uint8_t isp,uint32_t bits){append(i,x,y,w,h,q,1,d);}
void ff_voidplayer_vachunk_write_inter_cu(const VoidPlayerVachunkFrameInfo *i,uint16_t x,uint16_t y,uint8_t w,uint8_t h,uint8_t d,uint8_t q,uint8_t skip,uint8_t merge,uint8_t dir,int16_t x0,int16_t y0,int16_t x1,int16_t y1,int8_t r0,int8_t r1,uint32_t bits){append(i,x,y,w,h,q,skip?3:2,d);}
void ff_voidplayer_vachunk_write_h264_mb(const VoidPlayerVachunkFrameInfo *i,uint16_t x,uint16_t y,uint8_t q,uint8_t intra,uint8_t im,uint8_t skip,uint8_t merge,uint8_t dir,int16_t x0,int16_t y0,int16_t x1,int16_t y1,int8_t r0,int8_t r1,uint32_t bits){append(i,x,y,(uint16_t)(i->width-x<16?i->width-x:16),(uint16_t)(i->height-y<16?i->height-y:16),q,intra?1:skip?3:2,0);}
int ff_voidplayer_vachunk_write_frame_summary(const VoidPlayerVachunkFrameInfo *i){return 0;}
uint32_t ff_voidplayer_vachunk_frame_count(void){return 0;}
uint32_t ff_voidplayer_vachunk_last_frame_cu_count(void){return 0;}
int vpa_abi(void){return 1;}
void vpa_close(void){ready=NULL;for(int i=0;i<MAX_PICTURES;i++)release(&pictures[i]);av_frame_free(&frame);avcodec_free_context(&decoder);failure=0;}
int vpa_open(int codec,const uint8_t *extra,int size){
    if(decoder)return AVERROR(EBUSY);
    if(size<0||size>1024*1024)return AVERROR(EINVAL);
    enum AVCodecID id=codec==1?AV_CODEC_ID_H264:codec==2?AV_CODEC_ID_HEVC:codec==3?AV_CODEC_ID_VVC:AV_CODEC_ID_NONE;
    const AVCodec *c=avcodec_find_decoder(id);if(!c)return AVERROR_DECODER_NOT_FOUND;
    decoder=avcodec_alloc_context3(c);frame=av_frame_alloc();if(!decoder||!frame){vpa_close();return AVERROR(ENOMEM);}
    decoder->thread_count=1;decoder->thread_type=0;decoder->pkt_timebase=(AVRational){1,1000000};
    decoder->flags|=AV_CODEC_FLAG_COPY_OPAQUE;decoder->err_recognition=AV_EF_EXPLODE|AV_EF_CAREFUL;
    decoder->max_pixels=4096*2304;
    if(size){decoder->extradata=av_mallocz(size+AV_INPUT_BUFFER_PADDING_SIZE);if(!decoder->extradata){vpa_close();return AVERROR(ENOMEM);}memcpy(decoder->extradata,extra,size);decoder->extradata_size=size;}
    int ret=avcodec_open2(decoder,c,NULL);if(ret<0)vpa_close();return ret;
}
/* At most one bounded packet per step. JS owns independent packet iteration,
 * random access planning, cancellation checks and result backpressure. */
int vpa_feed(const uint8_t *bytes,int size,double pts,double dts,int ordinal){
    if(!decoder||ready||failure)return failure?failure:2;
    if(size<1||size>8*1024*1024||ordinal<0||ordinal>=2000000)return AVERROR(EINVAL);
    AVPacket *p=av_packet_alloc();if(!p)return AVERROR(ENOMEM);
    int ret=av_new_packet(p,size);if(ret<0){av_packet_free(&p);return ret;}
    memcpy(p->data,bytes,size);p->pts=(int64_t)pts;p->dts=(int64_t)dts;p->opaque=(void*)(uintptr_t)(ordinal+1);
    ret=avcodec_send_packet(decoder,p);av_packet_free(&p);return failure?failure:ret==AVERROR(EAGAIN)?2:ret;
}
int vpa_drain(void){if(!decoder)return AVERROR(EINVAL);int ret=avcodec_send_packet(decoder,NULL);return ret==AVERROR(EAGAIN)?2:ret;}
static int compare(const void *a,const void *b){const Block *x=a,*y=b;return x->y!=y->y?(int)x->y-y->y:(int)x->x-y->x;}
/* Successful decoder output is picture-complete for the single-thread build.
 * Refuse field/multilayer/invalid frames and clipped negative/high-depth QP. */
int vpa_step(void){
    if(!decoder)return AVERROR(EINVAL);if(failure)return failure;if(ready)return 1;
    int ret=avcodec_receive_frame(decoder,frame);if(ret==AVERROR(EAGAIN))return 0;if(ret==AVERROR_EOF)return 2;if(ret<0)return ret;
    Picture *p=find((uintptr_t)frame->opaque,0);
    if(!p){av_frame_unref(frame);return AVERROR_INVALIDDATA;}
    const AVPixFmtDescriptor *fmt=av_pix_fmt_desc_get(frame->format);
    p->invalid|=frame->decode_error_flags!=0||(frame->flags&AV_FRAME_FLAG_CORRUPT)||(frame->flags&AV_FRAME_FLAG_INTERLACED)||!fmt;
    p->depth=fmt?fmt->comp[0].depth:0;
    p->sealed=1;p->pts=frame->pts;
    qsort(p->blocks,p->count,sizeof(Block),compare);ready=p;
    av_frame_unref(frame);return 1;
}
int vpa_count(void){return ready?ready->count:0;}
int vpa_blocks(void){return ready?(int)(uintptr_t)ready->blocks:0;}
int vpa_ordinal(void){return ready?(int)ready->id-1:-1;}
int vpa_width(void){return ready?ready->width:0;}
int vpa_height(void){return ready?ready->height:0;}
int vpa_depth(void){return ready?ready->depth:0;}
int vpa_invalid(void){return !ready||ready->invalid||!ready->count;}
double vpa_pts(void){return ready?(double)ready->pts:0;}
int vpa_take(int maxBytes){if(!ready)return 0;if(maxBytes<0||ready->count*sizeof(Block)>(unsigned)maxBytes)return AVERROR(ENOMEM);int count=ready->count;release(ready);ready=NULL;return count;}
int vpa_record_bytes(void){return allocated;}
