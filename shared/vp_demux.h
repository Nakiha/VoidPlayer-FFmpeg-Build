/* LGPL-2.1-or-later. Shared bounded IO and source packet identity. */
#ifndef VP_DEMUX_H
#define VP_DEMUX_H
#include <stdint.h>
#include <libavformat/avformat.h>
typedef struct VpBlobIO { int handle; int64_t size, pos, bytes; } VpBlobIO;
typedef struct VPIndexRecordV3 {
    int64_t pts, dts, duration, pos;
    int32_t packet_size; uint32_t flags; uint64_t ordinal;
} VPIndexRecordV3;
_Static_assert(sizeof(VPIndexRecordV3) == 48, "index ABI v3 record size");
int vp_avio_read(void *, uint8_t *, int);
int64_t vp_avio_seek(void *, int64_t, int);
int vp_blob_open(AVFormatContext **, AVIOContext **, VpBlobIO **, int, int64_t);
void vp_blob_close(AVFormatContext **, AVIOContext **, VpBlobIO **);
int vp_rewind_start(AVFormatContext *, int);
int vp_select_video(AVFormatContext *, const AVCodec **);
int vp_packet_tuple(const AVPacket *, const VPIndexRecordV3 *);
uint64_t vp_packet_hash(const uint8_t *, int);
/* Conservative first milestone: exactly one AVC VCL slice starting at MB 0.
 * Interlaced output is separately refused at receive. No M:N inference. */
int vp_h264_single(const uint8_t *, int);
typedef struct { uint8_t sps[4096],pps[4096]; int sps_size,pps_size,changed; } VPH264Admission;
int vp_h264_admit(VPH264Admission *, const uint8_t *, int);
#endif
