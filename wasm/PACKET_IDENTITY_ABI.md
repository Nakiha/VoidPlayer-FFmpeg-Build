# Packet-output identity ABI v1

This additive packet path carries caller-assigned input tokens through actual
libavcodec output reordering. Existing packet/config/frame/picture ABIs remain
unchanged. Capability manifests declare `packetIdentityAbi: 1` for playback.

`vp_packet_open_identity(ctx, codec, extra, size)` opens a fresh single-thread
H.264 or HEVC packet decoder with `AV_CODEC_FLAG_COPY_OPAQUE`. The host must
independently admit exactly one progressive picture per AU, an actual closed
IDR start and a bounded complete decode window. The API does not certify those
bitstream properties or the coded reference graph.

`vp_packet_send_identity(ctx, pts, dts, key, eof, token)` binds a nonzero uint64
token to each input. Tokens are unique within the context epoch and unrelated
to clocks. Both clocks must be known. There are at most 32 inputs; each output
token may occur once. EOF drains the decoder and accepts no further input.
Receive until EAGAIN after each send, then drain all residual outputs. Missing
outputs must be rejected by the caller, never repaired by choosing nearby PTS.

Use `vp_packet_receive(ctx, INT64_MIN)`; discard thresholds are forbidden in
identity mode. For each success, `vp_packet_identity_info(ctx)` returns a
64-byte immutable-until-next-receive descriptor; otherwise it returns null:

| Offset | Field | Type |
| --- | --- | --- |
| 0 / 4 | ABI=1 / descriptor bytes=64 | uint32 / uint32 |
| 8 / 16 | context generation / input token | uint64 / uint64 |
| 24 / 32 | original input PTS / DTS in microseconds | int64 / int64 |
| 40 | exact packet payload FNV-1a64 | uint64 |
| 48 / 52 | packet byte count / decode_error_flags | int32 / int32 |
| 56 / 60 | AVFrame flags / valid=1 | uint32 / uint32 |

The core checks the entire opaque descriptor against an input in this epoch,
single-use output token and exact `AVFrame.pts`. Missing/unknown/stale/duplicate
identity, reported decode errors, corrupt or interlaced output fail closed and
poison receives until reset/reopen. The host also cross-checks the raw frame's
actual timestamp, byte/geometry/color domain and the selected source/cache/AU.
FNV detects tuple mistakes; it is not content authentication.

`vp_packet_reset` clears input/output membership and advances the generation.
Old packet sends are forbidden in identity mode. Ordinary packet mode keeps
its old behavior and returns no packet identity. Old callers and the existing
160-byte frame ABI are unchanged.

This provides output attribution for the admitted subset, **not reference
integrity certification or proof of no concealment**. Error flags detect only
reported decoder errors. No bitstream repair, alignment, PTS search or automatic
reference pairing is performed. A host must charge all input/output/drain work
and physically terminate its owning Worker before releasing resource admission.

The additive `vp_packet_identity_capacity()` export reports 512 admitted packets per context. Membership uses one byte per slot; slots 32 and above do not alias the former bit mask. Callers without this export retain a 32-packet limit. ABI 1 descriptors and legacy packet entry points are unchanged. Long clips advance through independently verified closed GOPs; decoded raw frames need not be retained.
