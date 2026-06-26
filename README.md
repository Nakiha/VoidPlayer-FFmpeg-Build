# VoidPlayer FFmpeg Build

Playback-focused FFmpeg dependency builds for VoidPlayer.

This repository builds runtime/dev packages for VoidPlayer without carrying
full official FFmpeg distributions, command-line tools, encoders, muxers,
filters, or device support.

## Scope

- FFmpeg source: `n8.1` by default.
- AV1 software decode helper: dav1d `1.5.3` by default, linked into
  `avcodec`.
- Windows SFTP helper: libssh from vcpkg, statically linked into `avformat`.
- Target layout: `include/`, `lib/`, plus platform runtime libraries, license,
  and manifest files.
- Windows x64: MSVC import libraries, DLLs, D3D11VA/D3D12VA/DXVA2 hardware accel.
- macOS arm64: shared libraries (`.dylib`), VideoToolbox hardware accel.

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

The profiles keep FFmpeg's broad default playback surface and disable the parts
VoidPlayer does not ship:

- default FFmpeg demuxers, decoders, parsers, and bitstream filters are enabled
- representative coverage includes MP4/MOV, Matroska/WebM, AVI, FLV,
  MPEG-TS/PS, Ogg, ASF, WAV, raw H.264/HEVC/VVC, H.264, HEVC, H.266/VVC, AV1,
  VP8/VP9, MPEG-1/2/4, AAC, AC3/EAC3, DTS/DCA, TrueHD, MP1/MP2/MP3, FLAC, ALAC,
  APE, Opus, Vorbis, and PCM
- encoders, muxers, command-line programs, avdevice/devices, avfilter/filters,
  and swscale are disabled
- local and HTTP/HTTPS input protocols are enabled; Windows also enables SFTP
- platform hardware acceleration entry points are enabled
- `libdav1d` is enabled for AV1 software decode

| Platform | Acceleration | Extra Protocols |
| --- | --- | --- |
| Windows x64 | D3D11VA / D3D12VA / DXVA2 | SFTP via static libssh |
| macOS arm64 | VideoToolbox | none |

## Build Locally -- Windows

Run from a Visual Studio Developer PowerShell on Windows:

```powershell
python -m pip install meson ninja
powershell -ExecutionPolicy Bypass -File scripts/build-windows-msvc.ps1
```

MSYS2 must be installed and provide `bash` and `make`. NASM
should be installed as a normal Windows executable, for example under
`C:\Program Files\NASM`. vcpkg is used to install static `libssh` and
`pkgconf` for SFTP support. If `vcpkg.exe` is not found through `-VcpkgRoot`,
`VCPKG_ROOT`, `PATH`, or `C:\vcpkg`, the script clones and bootstraps vcpkg
under the requested `-VcpkgRoot` path, or `.build/vcpkg` when none is provided.
The GitHub Actions workflow installs the other tools automatically.

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

To produce an emergency package without SFTP support:

```powershell
powershell -ExecutionPolicy Bypass -File scripts/build-windows-msvc.ps1 `
  -DisableSftp
```

## Build Locally -- macOS

Prerequisites: Xcode command line tools, NASM, pkg-config, and a Python virtual
environment with meson/ninja.

```bash
xcode-select --install
brew install nasm pkg-config
python3 -m venv .venv
. .venv/bin/activate
python -m pip install meson ninja
bash scripts/build-macos.sh
```

The build uses Apple Clang natively. VideoToolbox and Secure Transport are
provided by macOS and require no external runtime libraries. The script rewrites
FFmpeg dylib install names to `@rpath` for app-bundle redistribution, then
checks with `otool -L` that Homebrew/MacPorts library paths did not leak into
the package.

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

The unpacked directory can be copied to `VoidPlayer/windows/libs/ffmpeg`.

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

On macOS, shared libraries live in `lib/` rather than `bin/`.

Before swapping it into VoidPlayer, verify the package surface:

```powershell
Get-Content dist\voidplayer-ffmpeg-windows-x64-n8.1\voidplayer-ffmpeg-manifest.json
dumpbin /dependents dist\voidplayer-ffmpeg-windows-x64-n8.1\bin\avformat-*.dll
```

The manifest should include `file`, `http`, `https`, and `sftp`; hardware
acceleration entries should include `d3d11va` and `d3d12va`; and `dumpbin` should not show
MSYS2/MinGW runtime DLLs or dynamic `libssh`/OpenSSL/zlib DLLs.

For macOS packages:

```bash
cat dist/voidplayer-ffmpeg-macos-arm64-n8.1/voidplayer-ffmpeg-manifest.json
otool -L dist/voidplayer-ffmpeg-macos-arm64-n8.1/lib/libavformat*.dylib
```

The package intentionally does not include FFmpeg CLI binaries.
