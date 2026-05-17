# VoidPlayer FFmpeg Build

Minimal FFmpeg dependency builds for VoidPlayer.

This repository builds a small runtime/dev package that can replace
`VoidPlayer/windows/libs/ffmpeg` without carrying the full official Windows
distribution.

## Scope

- FFmpeg source: `n8.1` by default.
- AV1 software decode helper: dav1d `1.5.3` by default, linked into
  `avcodec`.
- Target layout: `include/`, `lib/`, `bin/`, plus license and manifest files.
- Primary target: Windows x64, MSVC import libraries.

The FFmpeg analysis tooling under
`VoidPlayer/native/analysis/vendor/ffmpeg` is intentionally out of scope. That
instrumented submodule has its own branch and build path.

## Libraries

The package is shaped for VoidPlayer's current runner CMake:

- `avcodec`
- `avformat`
- `avutil`
- `swresample`

FFmpeg no longer ships the old `libavresample` in n8.x. VoidPlayer currently
links `swresample`, so this build keeps that ABI.

## Default Feature Profile

The Windows profile disables FFmpeg's broad auto-detection and enables only the
pieces useful for playback:

- common file demuxers: MP4/MOV, Matroska/WebM, AVI, FLV, MPEG-TS/PS, raw H.264,
  raw HEVC, IVF, Ogg, MP3, AAC, WAV, FLAC, ASF
- common video decoders: H.264, HEVC, AV1, VP8, VP9, MPEG-1/2/4, MJPEG, ProRes,
  FFV1
- common audio decoders: AAC, MP3, FLAC, Opus, Vorbis, PCM variants
- D3D11VA/DXVA2 hardware acceleration entry points
- selected bitstream filters and parsers
- `libdav1d` for AV1 software decode

## Build Locally

Run from a Visual Studio Developer PowerShell on Windows:

```powershell
python -m pip install meson ninja
powershell -ExecutionPolicy Bypass -File scripts/build-windows-msvc.ps1
```

MSYS2 must be installed and provide `bash`, `make`, and `pkg-config`. NASM
should be installed as a normal Windows executable, for example under
`C:\Program Files\NASM`. The GitHub Actions workflow installs those tools
automatically.

MSYS2 is only used as a build shell. The produced DLLs must be MSVC-linked and
must not depend on MSYS2 or MinGW runtime DLLs such as `msys-2.0.dll`,
`libwinpthread-1.dll`, `libgcc_s_seh-1.dll`, or `libstdc++-6.dll`. The build
script verifies this with `dumpbin /dependents` before packaging.

To choose another FFmpeg release:

```powershell
powershell -ExecutionPolicy Bypass -File scripts/build-windows-msvc.ps1 `
  -FFmpegRef n8.0.1 `
  -Dav1dRef 1.5.3
```

## Artifacts

Successful builds create:

```text
dist/
  voidplayer-ffmpeg-windows-x64-n8.1/
    bin/
    include/
    lib/
    LICENSES/
    README.txt
    voidplayer-ffmpeg-manifest.json
  voidplayer-ffmpeg-windows-x64-n8.1.zip
```

The unpacked directory can be copied to `VoidPlayer/windows/libs/ffmpeg`.
