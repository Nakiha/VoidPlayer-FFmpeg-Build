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
