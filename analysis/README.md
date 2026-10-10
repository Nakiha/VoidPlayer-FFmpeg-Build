# Independent bitstream analysis core (ABI 1)

`bash scripts/build-analysis.sh` builds single-threaded `voidplayer-analysis.js`
and `.wasm` from pinned instrumented FFmpeg `59196ee81d09cd5312acf7d3b3896ad530d1248b`.
The playback build and its collection settings do not change. License: LGPL-2.1-or-later.
Build patches and collector live here; the Web repository only consumes the result.

One decoder context is permitted per module instance. Different modules have separate
heaps and collection state. `vpa_open(codec, extradata, length)`, then feed one bounded
compressed AU with microsecond PTS/DTS and its **source** ordinal. `vpa_step()` returns
0 (needs input), 1 (sealed output), 2 (end), or a negative error. Feed returns 2 for
backpressure, not an accepted packet. Drain sends EOS. `vpa_take(maxBytes)` releases
only analysis records; references stay owned by the decoder. A result exceeding the
byte budget fails explicitly. The caller must cancel/destroy after a resource error.

The 12-byte little-endian block ABI is u16 x/y/w/h, i16 QP, u8 mode/depth.
Modes are 1 intra, 2 inter, 3 skip. QP is usable only for 8-bit luma because the
inherited hooks clip negative QP; higher depth callers must mark QP unsupported.
MV, palette semantics, fields, layers, and unsupported VVC tools are not exported.
The Web input adapter explicitly rejects multi-picture AU and multilayer input.

Record lifecycle: collection keyed by COPY_OPAQUE source packet metadata; successful
single-thread `avcodec_receive_frame` seals a picture; raster order is finalized
before publication; take frees the independent record storage. This replaces the
old `finalize_frame_summaries` and `finish_vachunk` implementations entirely, so no
whole-stream reordering, aggregate QP recomputation or global reset can mutate an
already published picture. VVC's range-local decode_order is replaced with frame
opaque metadata in its hook. H.264/HEVC hooks use coded geometry, preserving crop
origin rather than clipping records to the visible resource. Decoder error flags,
field output and missing records are invalid, never unconditionally COMPLETE/EXACT.
The caller verifies AU admission, closed IDR pre-roll and complete nonoverlapping
block coverage before exposing an exact result.

Limits are checked before growing records: 32 in-flight pictures, 131072 blocks per
picture, 32 MiB record storage, 8 MiB input packet, 4096x2304 coded pixels, 512 MiB
WASM heap. A 4 MiB stack is required by the VVC decoder. Input iteration and yielding
are outside synchronous WASM. Hard cancellation terminates the dedicated worker.
These are initial conservative limits, not a real-time throughput promise.
