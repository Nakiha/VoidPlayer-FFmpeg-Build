# Independent bitstream analysis core (ABI 1)

`bash scripts/build-analysis.sh` builds single-threaded `voidplayer-analysis.js`
and `.wasm` from pinned instrumented FFmpeg `c160c1bb3f0faf3ac59b8fbf907a8e3fb604632e`.
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

Optional reference ABI 1 exports `vpa_reference_abi`, `vpa_ref_count`,
`vpa_refs`, `vpa_refs_invalid`, `vpa_picture_type` and `vpa_poc`. Reference
records are little-endian u32 source AU and u32 list (0/1). They union the
active reference lists across slices, using decoder AVFrame COPY_OPAQUE IDs;
they do not claim per-block usage. Unknown identities or >64 edges mark only
references unsupported. Records live until take. Build manifests include the
WASM hash in buildId and a dirty flag for unpublished local development.

The analysis build selects FFmpeg's WASM architecture and existing SIMD128 HEVC
IDCT/SAO backend. Compiler `-msimd128` alone does not select that backend:
FFmpeg's `--disable-asm` also disables architecture C intrinsics. Effective
`ARCH_WASM=1` and `HAVE_SIMD128=1` are required, including cached builds. The cache
signature includes the backend identity; old work directories fail explicitly
and require a fresh `ANALYSIS_BUILD_ROOT`. O3, no LTO and single threading stay
unchanged by this configuration fix.

The analysis-only PR check uses Emscripten 6.0.9 and Node 24, builds the exact PR
head and base, and compares every output header/block/reference byte for synthetic
H264/HEVC test patterns, including drain, reopen at a known closed encoder boundary (same extradata),
and damaged-prefix rejection.
The workflow records FFmpeg's generator version, encoder parameters and input
hashes; it uploads evidence and core artifacts, never media. `testsrc2` is generated
locally with no user or third-party audiovisual input. VVC is not claimed as CI
coverage here: the separate local release check compares 49 VVC frames alongside
180 HEVC and 30 H264 frames, without redistributing those local fixtures.
Performance percentages belong to fixed-machine paired experiments, not CI
thresholds or end-to-end application guarantees.

Block sorting keeps the same `(y,x)` comparator and 12-byte records. A strictly
increasing sequence returns immediately. Otherwise an in-place typed heapsort
is used only after proving coordinate uniqueness with a bounded 4-pixel-grid
bitset in the existing allocation's unused tail. At 4096x2304 the bitset needs
73728 bytes. It never grows the allocation or changes record-byte accounting.
Insufficient spare capacity, unsupported coordinates/geometry, or duplicate keys
fall back to the same libc `qsort` on the untouched original record sequence.
The old `qsort` has no stable-tie contract: equal coordinates with different
width/height/QP/mode/depth must not be silently assigned a new ordering. There is
no new promise of stable sorting. Unique keys have a uniquely determined output;
duplicates retain the old implementation's exact bytes rather than relying on
the Web overlap validator to hide a changed result. Sorting and any scratch-tail
writes finish before publication, in the existing single-threaded collector.

`node scripts/test-analysis-sort.mjs BUILD_ROOT BASE_SOURCE REPORT_JSON` verifies
88398 generated record cases against actual native and Emscripten `qsort`, using
ASan/UBSan for the native run. It includes exhaustive small tied inputs, arbitrary
non-key fields, exact/insufficient scratch capacity, maximum counts/geometry,
integer/index boundaries, actual collector sealing and resource failures, and
hard worker cancellation after maximum-size heap construction. A fresh worker
then reruns the record checks. Test-only notification hooks/exports are absent
from production builds. Compiled test executables stay outside uploaded evidence;
only test logs and small numeric reports accompany the CI production artifact.
The generated-media check compares the
complete production ABI output and malformed-prefix behavior against the fixed
PR base, including HEVC Main10; real H264/HEVC/VVC and full Chromium comparisons remain separate local
evidence without uploading media or record payloads.

## HEVC execution extension v1

The optional `_vpa_execution_abi()`/`_vpa_execution_mode(0|1)` extension requests
full (default) or eligible Main8 no-pixels execution before `_vpa_open`.
`_vpa_execution_path()` reports the selected path; `_vpa_execution_reason()`
reports full/retry provenance. `_vpa_execution_retry_code()` is the fixed
FFERRTAG(V,P,R,F), not a host errno. Block ABI1/reference ABI1, record layout,
limits, Main10 QP semantics and the semantic analysis recipe are unchanged.
Older clients stay full; clients without the extension stay full. Artifacts
expose executionAbi=1 and hevcNoPixels, with exact source/WASM build identity.

Only independent analysis compiles VP_ANALYSIS_ONLY=1. Source admission and
late retry boundaries are documented in the pinned FFmpeg
`doc/voidplayer-analysis.md`. The client must discard unpublished attempt results
and rebuild a fresh full decoder from the proven index anchor, at most once per
request. It must not resume reconstruction in the pruned DPB or retry generic
resource/corrupt/cancellation errors. Web supplies that transaction.

`ANALYSIS_HEVC_NO_PIXELS=0` builds a full-path control using the same source and
execution ABI for serial paired measurements; select a fresh build root because
the feature is part of the cache signature. Playback build flags/pins are unchanged.
CI additionally checks synthetic late parameter/hash/grain retry, malformed PS
classification, fresh full replay bytes, and Main10/lossless initial full selection.

This branch is stacked on unmerged sorter PR9 (a6bb9b13). The new FFmpeg pin
must be published/reviewed before this Core branch can build from its public URL;
local validation uses an explicit ANALYSIS_FFMPEG_SOURCE checkout. Web pins must
only move after the upstream sources/artifact are available.
