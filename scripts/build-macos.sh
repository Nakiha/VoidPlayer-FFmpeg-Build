#!/usr/bin/env bash
set -euo pipefail

# ---------------------------------------------------------------------------
# VoidPlayer FFmpeg macOS arm64 build script
# Produces a minimal FFmpeg runtime/dev package with VideoToolbox hw accel.
# ---------------------------------------------------------------------------

FFMPEG_REF="n8.1"
DAV1D_REF="1.5.3"
ARCH="arm64"
BUILD_ROOT=""
DIST_ROOT=""
CLEAN=false
SKIP_PACKAGE=false

while [[ $# -gt 0 ]]; do
    case "$1" in
        --ffmpeg-ref)   FFMPEG_REF="$2"; shift 2 ;;
        --dav1d-ref)    DAV1D_REF="$2";  shift 2 ;;
        --arch)         ARCH="$2";       shift 2 ;;
        --build-root)   BUILD_ROOT="$2"; shift 2 ;;
        --dist-root)    DIST_ROOT="$2";  shift 2 ;;
        --clean)        CLEAN=true;      shift ;;
        --skip-package) SKIP_PACKAGE=true; shift ;;
        -h|--help)
            echo "Usage: $0 [options]"
            echo "  --ffmpeg-ref REF    FFmpeg tag/branch (default: n8.1)"
            echo "  --dav1d-ref REF     dav1d tag/branch  (default: 1.5.3)"
            echo "  --arch ARCH         arm64|x86_64      (default: arm64)"
            echo "  --build-root PATH   Build directory    (default: .build)"
            echo "  --dist-root PATH    Output directory   (default: dist)"
            echo "  --clean             Wipe build directory before starting"
            echo "  --skip-package      Skip zip creation"
            exit 0 ;;
        *) echo "Unknown option: $1" >&2; exit 1 ;;
    esac
done

REPO_ROOT="$(cd "$(dirname "$0")/.." && pwd)"
BUILD_ROOT="${BUILD_ROOT:-$REPO_ROOT/.build}"
DIST_ROOT="${DIST_ROOT:-$REPO_ROOT/dist}"

PACKAGE_NAME="voidplayer-ffmpeg-macos-${ARCH}-${FFMPEG_REF}"
SOURCE_ROOT="$BUILD_ROOT/sources"
WORK_ROOT="$BUILD_ROOT/work"
DAV1D_SOURCE="$SOURCE_ROOT/dav1d"
FFMPEG_SOURCE="$SOURCE_ROOT/ffmpeg"
DAV1D_BUILD="$WORK_ROOT/dav1d-${ARCH}"
DAV1D_INSTALL="$WORK_ROOT/dav1d-install"
FFMPEG_BUILD="$WORK_ROOT/ffmpeg-${ARCH}"
PACKAGE_ROOT="$DIST_ROOT/$PACKAGE_NAME"

# ---------------------------------------------------------------------------
# Helpers
# ---------------------------------------------------------------------------

die() {
    echo "ERROR: $*" >&2
    exit 1
}

step() {
    echo "==> $*"
    "$@"
}

need_cmd() {
    command -v "$1" >/dev/null 2>&1 || die "$1 not found. $2"
}

add_enable_list() {
    local kind="$1"; shift
    for name in "$@"; do
        FFMPEG_ARGS+=("--enable-${kind}=${name}")
    done
}

require_config_component() {
    local symbol="$1"
    local config="$FFMPEG_BUILD/ffbuild/config.mak"
    grep -q "^${symbol}=yes" "$config" ||
        die "Required FFmpeg component was not enabled: $symbol"
}

assert_no_forbidden_deps() {
    local lib_dir="$1"
    [ -d "$lib_dir" ] || die "Library directory not produced: $lib_dir"

    local bad=()
    for dylib in "$lib_dir"/*.dylib; do
        [ -f "$dylib" ] || continue
        while IFS= read -r line; do
            if [[ "$line" =~ /usr/local/ ]] || [[ "$line" =~ /opt/homebrew/ ]] || [[ "$line" =~ /opt/local/ ]]; then
                bad+=("$(basename "$dylib"): $line")
            fi
        done < <(otool -L "$dylib" 2>/dev/null)
    done

    if [ ${#bad[@]} -gt 0 ]; then
        printf 'Forbidden Homebrew/MacPorts dependency detected:\n'
        printf '  %s\n' "${bad[@]}"
        exit 1
    fi
}

rewrite_install_names() {
    local lib_dir="$1"
    [ -d "$lib_dir" ] || die "Library directory not produced: $lib_dir"

    local dylibs=()
    while IFS= read -r dylib; do
        dylibs+=("$dylib")
    done < <(find "$lib_dir" -maxdepth 1 -type f -name '*.dylib' | sort)

    for dylib in "${dylibs[@]}"; do
        install_name_tool -id "@rpath/$(basename "$dylib")" "$dylib"
    done

    for dylib in "${dylibs[@]}"; do
        while IFS= read -r dep; do
            case "$dep" in
                "$lib_dir"/*.dylib|"$PACKAGE_ROOT/lib"/*.dylib)
                    install_name_tool -change "$dep" "@rpath/$(basename "$dep")" "$dylib"
                    ;;
            esac
        done < <(otool -L "$dylib" | awk 'NR > 1 { print $1 }')
    done
}

# ---------------------------------------------------------------------------
# Prerequisites
# ---------------------------------------------------------------------------

need_cmd cc          "Install Xcode command line tools: xcode-select --install"
need_cmd make        "Install Xcode command line tools"
need_cmd nasm        "Install NASM: brew install nasm"
need_cmd meson       "Install meson: pip install meson ninja"
need_cmd ninja       "Install ninja: pip install meson ninja"
need_cmd pkg-config  "Install pkg-config: brew install pkg-config"
need_cmd install_name_tool "Install Xcode command line tools"
need_cmd otool       "Install Xcode command line tools"

# ---------------------------------------------------------------------------
# Directory setup
# ---------------------------------------------------------------------------

if $CLEAN && [ -d "$BUILD_ROOT" ]; then
    step rm -rf "$BUILD_ROOT"
fi

mkdir -p "$SOURCE_ROOT" "$WORK_ROOT" "$DIST_ROOT"

# ---------------------------------------------------------------------------
# Clone sources
# ---------------------------------------------------------------------------

if [ ! -d "$DAV1D_SOURCE" ]; then
    step git clone --depth 1 --branch "$DAV1D_REF" \
        https://code.videolan.org/videolan/dav1d.git "$DAV1D_SOURCE"
fi

if [ ! -d "$FFMPEG_SOURCE" ]; then
    step git clone --depth 1 --branch "$FFMPEG_REF" \
        https://github.com/FFmpeg/FFmpeg.git "$FFMPEG_SOURCE"
fi

# ---------------------------------------------------------------------------
# Build dav1d (static library via meson)
# ---------------------------------------------------------------------------

step meson setup "$DAV1D_BUILD" "$DAV1D_SOURCE" \
    --prefix "$DAV1D_INSTALL" \
    --libdir lib \
    --buildtype release \
    --default-library static \
    -Denable_tools=false \
    -Denable_tests=false

step meson compile -C "$DAV1D_BUILD"
step meson install -C "$DAV1D_BUILD"

DAV1D_STATIC_LIB="$DAV1D_INSTALL/lib/libdav1d.a"
[ -f "$DAV1D_STATIC_LIB" ] || die "dav1d static library not produced: $DAV1D_STATIC_LIB"

# ---------------------------------------------------------------------------
# Build FFmpeg
# ---------------------------------------------------------------------------

export PKG_CONFIG_PATH="$DAV1D_INSTALL/lib/pkgconfig:${PKG_CONFIG_PATH:-}"

FFMPEG_ARGS=()
FFMPEG_ARGS+=(
    "--prefix=$PACKAGE_ROOT"
    "--target-os=darwin"
    "--arch=$ARCH"
    "--enable-shared"
    "--disable-static"
    "--disable-autodetect"
    "--disable-encoders"
    "--disable-muxers"
    "--disable-programs"
    "--disable-doc"
    "--disable-debug"
    "--disable-avdevice"
    "--disable-avfilter"
    "--disable-swscale"
    "--enable-runtime-cpudetect"
    "--enable-avcodec"
    "--enable-avformat"
    "--enable-avutil"
    "--enable-swresample"
    "--enable-libdav1d"
    "--enable-videotoolbox"
    "--enable-network"
    "--enable-securetransport"
    "--pkg-config-flags=--static"
)

add_enable_list protocol cache concat file http https pipe tcp tls

add_enable_list decoder libdav1d

add_enable_list hwaccel \
    h264_videotoolbox hevc_videotoolbox \
    vp9_videotoolbox av1_videotoolbox

mkdir -p "$FFMPEG_BUILD"

cd "$FFMPEG_BUILD"
step "$FFMPEG_SOURCE/configure" "${FFMPEG_ARGS[@]}"
require_config_component CONFIG_HTTP_PROTOCOL
require_config_component CONFIG_HTTPS_PROTOCOL
require_config_component CONFIG_MOV_DEMUXER
require_config_component CONFIG_MATROSKA_DEMUXER
require_config_component CONFIG_FLV_DEMUXER
require_config_component CONFIG_AVI_DEMUXER
require_config_component CONFIG_MPEGTS_DEMUXER
require_config_component CONFIG_AAC_DECODER
require_config_component CONFIG_AC3_DECODER
require_config_component CONFIG_EAC3_DECODER
require_config_component CONFIG_MP1_DECODER
require_config_component CONFIG_MP2_DECODER
require_config_component CONFIG_MP3_DECODER
require_config_component CONFIG_DCA_DECODER
require_config_component CONFIG_TRUEHD_DECODER
require_config_component CONFIG_APE_DECODER
require_config_component CONFIG_ALAC_DECODER
require_config_component CONFIG_H264_DECODER
require_config_component CONFIG_HEVC_DECODER
require_config_component CONFIG_VVC_DECODER
require_config_component CONFIG_AV1_DECODER
require_config_component CONFIG_LIBDAV1D_DECODER
require_config_component CONFIG_VVC_PARSER
require_config_component CONFIG_VVC_MP4TOANNEXB_BSF
require_config_component CONFIG_H264_VIDEOTOOLBOX_HWACCEL
require_config_component CONFIG_HEVC_VIDEOTOOLBOX_HWACCEL
require_config_component CONFIG_AV1_VIDEOTOOLBOX_HWACCEL
require_config_component CONFIG_VP9_VIDEOTOOLBOX_HWACCEL
cd - > /dev/null

NPROC="$(sysctl -n hw.ncpu)"
step make -C "$FFMPEG_BUILD" -j"$NPROC"
step make -C "$FFMPEG_BUILD" install

# ---------------------------------------------------------------------------
# Validate
# ---------------------------------------------------------------------------

[ -d "$PACKAGE_ROOT/lib" ] || die "Package lib directory not produced: $PACKAGE_ROOT/lib"
rewrite_install_names "$PACKAGE_ROOT/lib"
assert_no_forbidden_deps "$PACKAGE_ROOT/lib"

# ---------------------------------------------------------------------------
# Licenses
# ---------------------------------------------------------------------------

LICENSE_ROOT="$PACKAGE_ROOT/LICENSES"
mkdir -p "$LICENSE_ROOT"

for f in "$FFMPEG_SOURCE"/COPYING*; do
    [ -f "$f" ] && cp "$f" "$LICENSE_ROOT/$(basename "$f")"
done
for name in LICENSE.md README.md; do
    [ -f "$FFMPEG_SOURCE/$name" ] && cp "$FFMPEG_SOURCE/$name" "$LICENSE_ROOT/FFmpeg-$name"
done
[ -f "$DAV1D_SOURCE/COPYING" ] && cp "$DAV1D_SOURCE/COPYING" "$LICENSE_ROOT/dav1d-COPYING"

# ---------------------------------------------------------------------------
# Manifest
# ---------------------------------------------------------------------------

BUILT_AT="$(date -u +%Y-%m-%dT%H:%M:%SZ)"

cat > "$PACKAGE_ROOT/voidplayer-ffmpeg-manifest.json" <<EOF
{
  "package": "$PACKAGE_NAME",
  "target": "macos-${ARCH}",
  "ffmpegRef": "$FFMPEG_REF",
  "dav1dRef": "$DAV1D_REF",
  "builtAtUtc": "$BUILT_AT",
  "libraries": ["avcodec", "avformat", "avutil", "swresample"],
  "protocols": ["cache", "concat", "file", "http", "https", "pipe"],
  "componentPolicy": "FFmpeg default demuxers, decoders, parsers, bitstream filters, and built-in protocols are enabled; encoders, muxers, devices, filters, programs, swscale, and avdevice are disabled.",
  "representativeDemuxers": ["mov/mp4", "matroska/webm", "avi", "flv", "mpegts", "mpegps", "ogg", "asf", "wav", "raw h264/hevc/vvc"],
  "representativeDecoders": ["h264", "hevc", "vvc/h266", "av1", "vp8", "vp9", "mpeg1/2/4", "aac", "ac3", "eac3", "dca/dts", "truehd", "mp1", "mp2", "mp3", "flac", "alac", "ape", "opus", "vorbis", "pcm"],
  "av1SoftwareDecoder": "libdav1d",
  "hardwareAcceleration": "videotoolbox",
  "installNames": "@rpath",
  "analysisFfmpegSubmodule": "not included",
  "forbiddenRuntimeDependencyCheck": "otool -L"
}
EOF

cat > "$PACKAGE_ROOT/README.txt" <<EOF
VoidPlayer FFmpeg runtime/dev package

FFmpeg: $FFMPEG_REF
dav1d:  $DAV1D_REF
Target: macos-${ARCH}

This package contains avcodec, avformat, avutil, swresample shared
libraries (.dylib), headers, and license material for VoidPlayer.

VideoToolbox hardware acceleration is enabled for H.264, HEVC, VP9,
and AV1 decoding.

Local file and HTTP/HTTPS playback protocols are enabled. Dylib install names
are rewritten to @rpath for app-bundle redistribution.

FFmpeg's broad default demuxer/decoder/parser/bitstream-filter set is enabled
for playback compatibility, including H.266/VVC software decode. Encoders,
muxers, devices, filters, command-line programs, avdevice, and swscale are not
included.

The instrumented analysis FFmpeg submodule is not included.
EOF

# ---------------------------------------------------------------------------
# Archive
# ---------------------------------------------------------------------------

if ! $SKIP_PACKAGE; then
    ZIP_PATH="$DIST_ROOT/${PACKAGE_NAME}.zip"
    [ -f "$ZIP_PATH" ] && rm -f "$ZIP_PATH"
    cd "$DIST_ROOT"
    step zip -r --symlinks "$ZIP_PATH" "$PACKAGE_NAME"
    cd - > /dev/null
    echo "==> package: $ZIP_PATH"
fi

echo "==> done: $PACKAGE_ROOT"
