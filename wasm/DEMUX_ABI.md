# Independent demux and received picture identity (ABI 1)

`shared/vp_demux.c` owns bounded Blob AVIO and video selection used by playback
and the independent `vpd_*` interface. `shared/vp_packet_demux.c` is linked into
both existing artifacts; the analysis build adds only the MPEG-TS demuxer and
libavformat, retaining its existing block/reference codec hooks and no swscale,
network, filesystem or third WASM product.

The pinned libavformat tree is `740392267b71cb9f0e6bd3b5b52173777b0d0939`.
Both builders check it before assigning compatibility ID
`ffmpeg-9.0.2-avformat-740392267b71-ts-nofill-v1`. `vpd_build_id` additionally
requires the exact build revision used by the read-only index; compatibility
is not permission to accept another indexerBuild. H264/TS uses NOFILLIN so missing
PES PTS/DTS remain AV_NOPTS_VALUE. Packet durations may consequently be zero;
no missing timestamp is replaced by another clock.

`vpd_open(handle, size)` owns a format context, AVIO, packet and cursor separate
from playback. It probes stream info and selects a video stream, but does not
create a playback decoder or output pixels. Stream probing uses FFmpeg's normal
header discovery. Negative handles dispatch to the host's synchronous
`Module.vpDemuxRead`; positive handles use the existing `Module.vpBlobs` map.
The host bounds reads and cancellation; callbacks must return the requested
in-source byte range. Probe and seek IO count in `vpd_bytes`, including rereads.

`vpd_read(context, budget)` accepts 1..4096 demux packets. Return 1 means a
selected packet is held, 2 means the foreign-stream budget was spent, 0 is real
EOF and negative is an error. `vpd_release` unrefs it; no next read is accepted
while a packet is held. The immutable 72-byte `vpd_packet` descriptor contains:

| Offset | Value |
| --- | --- |
| 0 / 8 / 16 / 24 | i64 PTS / DTS / duration / demux position |
| 32 | u64 source ordinal (UINT64_MAX if not restored) |
| 40 / 44 | i32 stream / u32 flags |
| 48 / 52 | u32 payload pointer / i32 payload length |
| 56 / 60 | i32 timebase numerator / denominator |
| 64 | u64 FNV-1a payload diagnostic hash |

Flags: 1 PTS known, 2 DTS known, 4 ordinal known, 8 demux key, 16 corrupt.
Position is a demux locator, not an offset for reading `size` raw file bytes.
FNV is only a diagnostic checksum, not a uniqueness/security guarantee.
`vpd_extra`/`extra_size` expose original codecpar extradata (possibly absent).
AVC SPS/PPS may instead come from in-band NALs.

`vpd_bind_index` copies complete v3 records into original ordinal order, checks
coverage and adds TS seek anchors. `vpd_seek` takes raw DTS, flushes the packet
cursor and withholds ordinal until a unique PTS/DTS/pos/size tuple restores it.
Following packets must match the next original record. The host checks every
record and treats missing/duplicate timestamps, discontinuities or ambiguity as
unsupported exact analysis. No hidden from-start fallback is supplied.

The 160-byte `VPFrameInfo` ABI is unchanged. `vp_picture_info` (picture ABI 1)
is a separate 56-byte immutable-until-next-output descriptor: i64 PTS/DTS/pos,
u64 ordinal, i32 size/stream, u32 flags/ABI, u64 payload diagnostic hash. Flags
1 means admitted. Playback attaches it through AVPacket.opaque_ref + COPY_OPAQUE
and reads it from the actual received AVFrame. This first milestone admits only
single-video TS, static SPS/PPS, exactly one progressive AVC VCL slice starting
at MB 0, unique known raw PTS/DTS, a matching complete index and error-free
non-interlaced frame output. Missing, repeated or M:N output tokens fail closed.
Field/MBAFF, layered/SVC/MVC, multiple slices/pictures and dynamic configuration
are excluded. COPY_OPAQUE alone is not evidence of one packet = one picture.

The existing metadata scan captures static in-band SPS/PPS. The configuration
accessor normalizes only Annex-B start codes and labels its provenance explicitly;
it does not claim these bytes were an avcC container record. Repeated identical
parameter sets do not introduce an epoch; changed sets disable exact admission.
`vpa_feed_known` adds independent PTS/DTS known bits while preserving the old
`vpa_feed` entry for callers with two known clocks.
