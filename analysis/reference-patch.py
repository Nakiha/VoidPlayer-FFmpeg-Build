from pathlib import Path
import sys
root=Path(sys.argv[1])
patches={
'libavcodec/h264_mb.c': ('    qp = (uint8_t)av_clip_uint8(h->cur_pic.qscale_table', """    if (frame_info.slice_type != 2)
        for(int list=0;list<sl->list_count;list++)
            for(int r=0;r<sl->ref_count[list];r++) {
                const H264Picture *ref=sl->ref_list[list][r].parent;
                ff_vpa_reference(frame_info.frame_identity, ref&&ref->f?(uintptr_t)ref->f->opaque:0, list);
            }
"""),
'libavcodec/hevc/hevcdec.c': ('    qp = (uint8_t)av_clip_uint8(lc->qp_y);', """    if (s->sh.slice_type != HEVC_SLICE_I)
        for(int list=0;list<(s->sh.slice_type==HEVC_SLICE_B?2:1);list++)
            for(int r=0;r<s->sh.nb_refs[list];r++) {
                const HEVCFrame *ref=ref_pic_list&&r<ref_pic_list[list].nb_refs?ref_pic_list[list].ref[r]:NULL;
                ff_vpa_reference(frame_info.frame_identity, ref&&ref->f?(uintptr_t)ref->f->opaque:0, list);
            }
"""),
'libavcodec/vvc/ctu.c': ('    info->coded_order_key = fc->decode_order + 1;', """    if (info->slice_type != 2)
        for(int list=0;list<(info->slice_type==0?2:1);list++)
            for(int r=0;r<lc->sc->sh.r->num_ref_idx_active[list];r++) {
                const VVCFrame *ref=r<lc->sc->rpl[list].nb_refs?lc->sc->rpl[list].refs[r].ref:NULL;
                ff_vpa_reference(info->frame_identity, ref&&ref->frame?(uintptr_t)ref->frame->opaque:0, list);
            }
""")}
for file,(anchor,code) in patches.items():
 p=root/file;s=p.read_text()
 if 'ff_vpa_reference(' not in s:
  assert anchor in s
  s='#include "voidplayer_vachunk.h"\nextern void ff_vpa_reference(uintptr_t id, uintptr_t ref, int list);\n'.join(s.split('#include "voidplayer_vachunk.h"\n')) if file.endswith('hevcdec.c') or file.endswith('h264_mb.c') else s
  # declaration after includes, with uintptr_t available
  if file.endswith('ctu.c'):s=s.replace('#include "libavcodec/voidplayer_vachunk.h"', '#include "libavcodec/voidplayer_vachunk.h"\nextern void ff_vpa_reference(uintptr_t id, uintptr_t ref, int list);')
  s=s.replace(anchor,code+anchor)
  p.write_text(s)
