# VoidPlayer FFmpeg Build

Minimal FFmpeg dependency builds for VoidPlayer.

This repository builds small runtime/dev packages for **Windows** and **macOS**
without carrying the full official distributions.

## Scope

- FFmpeg source: `n8.1` by default.
- AV1 software decode helper: dav1d `1.5.3` by default, linked into
  `avcodec`.
- Target layout: `include/`, `lib/`, plus license and manifest files.
- **Windows x64**: MSVC import libraries, DLLs, D3D11VA/DXVA2 hardware accel.
- **macOS arm64**: Shared libraries (.dylib), VideoToolbox hardware accel.

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

Both platforms share the same playback-oriented feature set. FFmpeg's broad
auto-detection is disabled and only useful pieces are enabled:

- common file demuxers: MP4/MOV, Matroska/WebM, AVI, FLV, MPEG-TS/PS, raw H.264,
  raw HEVC, IVF, Ogg, MP3, AAC, WAV, FLAC, ASF
- common video decoders: H.264, HEVC, AV1, VP8, VP9, MPEG-1/2/4, MJPEG, ProRes,
  FFV1
- common audio decoders: AAC, MP3, FLAC, Opus, Vorbis, PCM variants
- selected bitstream filters and parsers
- `libdav1d` for AV1 software decode

Hardware acceleration differs per platform:

| Platform | Acceleration |
|----------|-------------|
| Windows  | D3D11VA / DXVA2 |
| macOS    | VideoToolbox |

## Build Locally — Windows

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

## Build Locally — macOS

Prerequisites: Xcode command line tools, NASM, pkg-config, Python with meson/ninja.

```bash
xcode-select --install
brew install nasm pkg-config
python3 -m pip install meson ninja
bash scripts/build-macos.sh
```

The build uses Apple Clang natively — no cross-compilation toolchain or MSYS2
needed. VideoToolbox is enabled automatically via `--enable-videotoolbox` and
requires no external dependencies (it ships with macOS).

The script verifies that no Homebrew or MacPorts library paths leak into the
produced dylibs using `otool -L`.

To choose another FFmpeg release or architecture:

```bash
bash scripts/build-macos.sh --ffmpeg-ref n8.0.1 --dav1d-ref 1.5.3
bash scripts/build-macos.sh --arch x86_64
```

## Artifacts

Successful Windows builds create:

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

Successful macOS builds create:

```text
dist/
  voidplayer-ffmpeg-macos-arm64-n8.1/
    include/
    lib/
    LICENSES/
    README.txt
    voidplayer-ffmpeg-manifest.json
  voidplayer-ffmpeg-macos-arm64-n8.1.zip
```

On macOS, shared libraries (.dylib) live in `lib/` rather than `bin/`.
