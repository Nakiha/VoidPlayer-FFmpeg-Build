# Frame ABI v2

`vp_frame_info` returns 160 bytes, little endian. `vp_pixels` returns an owned
buffer valid until the next output, reset or destroy. Consumers must copy it
before continuing decoding. All byte ranges must be validated against the current
heap; a string ccall can grow and detach a previously obtained heap view.

| Offset | Fields |
| --- | --- |
| 0, 8 | signed i64 PTS and duration |
| 16, 20 | u32 version (2), descriptor size (160) |
| 24–36 | i32 coded width, height, RGBA stride (0 for YUV), buffer bytes |
| 40–56 | i32 source pixel format, primaries, transfer, matrix, range (FFmpeg enums) |
| 60, 64, 68 | i32 SAR numerator/denominator, u32 metadata revision |
| 72–92 | i32 layout (0 RGBA, 1 planar, 2 UV semiplanar), bit depth, right shift, log2 chroma width/height subsampling, chroma location |
| 96–140 | three planes, each i32 byte offset, byte stride, sample width, sample height; third is zero for semiplanar |
| 144–156 | i32 crop left/top/right/bottom |

YUV is tightly packed row by row, including odd-size chroma using ceiling
subsampling. Negative AVFrame linesize is traversed with signed pointer
arithmetic. Padding is not exported. 16-bit samples remain little endian and
retain their original shift (for example P010 shift 6); no 8-bit quantization.
Supported layouts are non-alpha planar YUV and UV semiplanar forms whose
AVPixFmtDescriptor component depth, shift, offsets and steps match the contract.
Unsupported layouts and colorimetry, including PQ/HLG until a high precision HDR
renderer exists, use explicit swscale RGBA. Consumers must label this fallback,
never count it as original-precision YUV. SDR matrices 601/709/2020 NCL and
primaries 601/709/2020 with ordinary SDR transfers are eligible for raw planes.

Both packet receive and container extract use this ABI. v1 consumers must not
interpret v2 YUV bytes as RGBA. Frontend pin updates require an immutable pushed
core commit and single/mt validation.

## FFmpeg index ABI v2 and stream ABI v1

`vp_index_abi_version() == 2` describes the unchanged 40-byte record layout above. The separate `vp_index_stream_abi_version() == 1` enables incremental record transfer:

- A producer opts in with `vp_index_scan_stream_begin()`. The compatibility `vp_index_scan_begin()` and `vp_index_build()` keep their demux-only full-index behavior.
- The producer checks `vp_index_scan_stable_count()` and `vp_index_scan_stable_ticks()`, then exports only new records with `vp_index_export_range(start, count, ...)`.
- Streaming publication is currently enabled only for MPEG-TS. It drains the FFmpeg decoder and advances from timestamps actually emitted in presentation order. A timestamp regression, missing output timestamp, decoder failure, or export failure disables partial publication; the ordinary full scan continues.
- A client starts with `vp_index_import_begin()`, appends contiguous batches with `vp_index_import_batch(context, records, count, seq, safeTicks, final)`, and signals finality with the last batch (which may be empty). Sequence, sorted PTS, monotonic safe ticks, and record flags are validated before appending.
- Imported partial records are visible to `vp_index_count/ticks/is_key/duration` and can be used by `vp_extract()`. Scan-side unpublished records remain hidden. The caller must wait until its requested target is at or before the received safe tick.

The producer's safe tick is the PTS of the last record in the stable sorted prefix, not a byte or packet progress estimate. When streaming is disabled, the caller must discard its partial import context and use the completed full export. Do not mix records from separate builds.
