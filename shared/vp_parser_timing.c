/* Preserve parser INPUT timing for guarded TS reads. H264's HRD SEI can fill
 * parser->pts/dts even when AVFMT_FLAG_NOFILLIN disables format interpolation.
 * Link wrapping leaves both pinned FFmpeg source trees unchanged. */
#include "vp_demux.h"
#include <libavcodec/avcodec.h>
static _Thread_local int guard;
void vp_parser_timing_guard(int enabled) { guard=enabled; }
int __real_av_parser_parse2(AVCodecParserContext *,AVCodecContext *,uint8_t **,int *,const uint8_t *,int,int64_t,int64_t,int64_t);
int __wrap_av_parser_parse2(AVCodecParserContext *s,AVCodecContext *avctx,uint8_t **out,int *size,
                           const uint8_t *buf,int length,int64_t pts,int64_t dts,int64_t pos) {
    if(!guard || avctx->codec_id!=AV_CODEC_ID_H264)
        return __real_av_parser_parse2(s,avctx,out,size,buf,length,pts,dts,pos);
    /* Mirror only parser.c's public input timestamp descriptor selection,
     * before the codec parser can synthesize time. Payload/parser state and
     * AU reassembly remain exclusively inside av_parser_parse2. */
    AVCodecParserContext raw=*s;
    if(!(raw.flags&PARSER_FLAG_FETCHED_OFFSET))raw.next_frame_offset=raw.cur_offset=pos;
    if(length && raw.cur_offset+length!=raw.cur_frame_end[raw.cur_frame_start_index]) {
        int i=(raw.cur_frame_start_index+1)&(AV_PARSER_PTS_NB-1);
        raw.cur_frame_offset[i]=raw.cur_offset;raw.cur_frame_end[i]=raw.cur_offset+length;
        raw.cur_frame_pts[i]=pts;raw.cur_frame_dts[i]=dts;
    }
    if(raw.fetch_timestamp) {
        raw.pts=raw.dts=AV_NOPTS_VALUE;
        for(int i=0;i<AV_PARSER_PTS_NB;i++) {
            if(raw.cur_offset>=raw.cur_frame_offset[i] &&
               (raw.frame_offset<raw.cur_frame_offset[i] || (!raw.frame_offset&&!raw.next_frame_offset)) &&
               raw.cur_frame_end[i]) {
                raw.pts=raw.cur_frame_pts[i];raw.dts=raw.cur_frame_dts[i];
                if(raw.cur_offset<raw.cur_frame_end[i])break;
            }
        }
    }
    int consumed=__real_av_parser_parse2(s,avctx,out,size,buf,length,pts,dts,pos);
    // Retain raw missing states also while waiting for the next PES segment.
    s->pts=raw.pts;s->dts=raw.dts;
    return consumed;
}
