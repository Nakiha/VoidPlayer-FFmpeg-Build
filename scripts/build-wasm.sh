#!/usr/bin/env bash
set -euo pipefail

# ---------------------------------------------------------------------------
# VoidPlayer FFmpeg WASM build script
# Produces a trimmed single-threaded decoder core (Emscripten) for the web
# prototype's fallback path: container support mediabunny lacks and codecs
# WebCodecs lacks (FFV1, MPEG-1/2, MPEG-4 ASP, MJPEG, ProRes, H.266/VVC).
# ---------------------------------------------------------------------------

FFMPEG_REF="n9.0.1"
FFMPEG_REMOTE="https://github.com/Nakiha/FFmpeg.git"
BUILD_ROOT=""
DIST_ROOT=""
CLEAN=false
SKIP_PACKAGE=false
MT=false

while [[ $# -gt 0 ]]; do
    case "$1" in
        --ffmpeg-ref)    FFMPEG_REF="$2"; shift 2 ;;
        --ffmpeg-remote) FFMPEG_REMOTE="$2"; shift 2 ;;
        --build-root)    BUILD_ROOT="$2"; shift 2 ;;
        --dist-root)     DIST_ROOT="$2";  shift 2 ;;
        --clean)         CLEAN=true;      shift ;;
        --mt)            MT=true;         shift ;;
        --skip-package)  SKIP_PACKAGE=true; shift ;;
        -h|--help)
            echo "Usage: $0 [options]"
            echo "  --ffmpeg-ref REF     FFmpeg tag/branch (default: n9.0.1)"
            echo "  --ffmpeg-remote URL  FFmpeg source remote (default: Nakiha/FFmpeg fork)"
            echo "  --build-root PATH    Build directory    (default: .build)"
            echo "  --dist-root PATH     Output directory   (default: dist)"
            echo "  --clean              Wipe build directory before starting"
            echo "  --mt                 Multi-threaded core (pthreads + SharedArrayBuffer;"
            echo "                       requires COOP/COEP isolation on the host page)"
            echo "  --skip-package       Skip zip creation"
            exit 0 ;;
        *) echo "Unknown option: $1" >&2; exit 1 ;;
    esac
done

REPO_ROOT="$(cd "$(dirname "$0")/.." && pwd)"
BUILD_ROOT="${BUILD_ROOT:-$REPO_ROOT/.build}"
DIST_ROOT="${DIST_ROOT:-$REPO_ROOT/dist}"
EMSDK_ROOT="${EMSDK:-$REPO_ROOT/.toolchains/emsdk}"

SUFFIX=""
if $MT; then SUFFIX="-mt"; fi
PACKAGE_NAME="voidplayer-ffmpeg-wasm${SUFFIX}-${FFMPEG_REF}"
FFMPEG_SOURCE="$BUILD_ROOT/sources/ffmpeg-wasm"
FFMPEG_BUILD="$BUILD_ROOT/work/ffmpeg-wasm${SUFFIX}"
FFMPEG_INSTALL="$BUILD_ROOT/work/ffmpeg-wasm${SUFFIX}-install"
PACKAGE_ROOT="$DIST_ROOT/$PACKAGE_NAME"

die() {
    echo "ERROR: $*" >&2
    exit 1
}

step() {
    echo "==> $*"
    "$@"
}

# ---------------------------------------------------------------------------
# Prerequisites: Emscripten SDK (already on PATH, or local .toolchains / $EMSDK)
# ---------------------------------------------------------------------------

if ! command -v emcc >/dev/null 2>&1; then
    [ -f "$EMSDK_ROOT/emsdk_env.sh" ] || die "emcc not on PATH and emsdk not found at $EMSDK_ROOT. Clone https://github.com/emscripten-core/emsdk there and run: emsdk install latest && emsdk activate latest"
    # shellcheck disable=SC1091
    source "$EMSDK_ROOT/emsdk_env.sh" >/dev/null
fi
command -v emcc >/dev/null 2>&1 || die "emcc not found"

# ---------------------------------------------------------------------------
# Directory setup and source clone
# ---------------------------------------------------------------------------

if $CLEAN && [ -d "$BUILD_ROOT" ]; then
    step rm -rf "$BUILD_ROOT"
fi

mkdir -p "$BUILD_ROOT/sources" "$FFMPEG_BUILD" "$DIST_ROOT"

if [ ! -d "$FFMPEG_SOURCE" ]; then
    step git clone --depth 1 --branch "$FFMPEG_REF" "$FFMPEG_REMOTE" "$FFMPEG_SOURCE"
fi

# ---------------------------------------------------------------------------
# Configure FFmpeg (trimmed to the fallback decode path)
# ---------------------------------------------------------------------------

DEMUXERS="mov matroska mpegts mpegps avi"
DECODERS="ffv1 h264 hevc mpeg1video mpeg2video mpeg4 mjpeg prores vvc vp8 vp9"
PARSERS="ffv1 h264 hevc mpegvideo mpeg4video mjpeg vvc vp8 vp9"

# wasm SIMD128 (all modern browsers, Safari >= 16.4) measurably speeds up the
# software decoders; disable with WASM_SIMD=0 for exotic targets.
WASM_SIMD="${WASM_SIMD:-1}"
SIMD_ARGS=()
SIMD_CFLAGS=""
if [ "$WASM_SIMD" = "1" ]; then
    SIMD_ARGS=("--extra-cflags=-msimd128" "--extra-ldflags=-msimd128")
    SIMD_CFLAGS="-msimd128"
fi
if $MT; then
    SIMD_ARGS=("--extra-cflags=-msimd128 -pthread -DVP_MT" "--extra-ldflags=-msimd128 -pthread")
    SIMD_CFLAGS="-msimd128 -pthread -DVP_MT"
fi

FFMPEG_ARGS=(
    "--prefix=$FFMPEG_INSTALL"
    "--cc=emcc"
    "--cxx=em++"
    "--ar=emar"
    "--ranlib=emranlib"
    "--nm=emnm"
    "--strip=emstrip"
    "--target-os=none"
    "--arch=x86_32"
    "--cpu=generic"
    "--enable-cross-compile"
    "--disable-asm"
    "--disable-stripping"
    "--disable-programs"
    "--disable-doc"
    "--disable-debug"
    "--disable-avdevice"
    "--disable-avfilter"
    "--disable-swresample"
    "--disable-network"
    "--disable-everything"
    "--disable-autodetect"
    "--enable-avcodec"
    "--enable-avformat"
    "--enable-avutil"
    "--enable-swscale"
    "--enable-protocol=file"
    "${SIMD_ARGS[@]}"
)
if $MT; then
    FFMPEG_ARGS+=("--enable-pthreads")
fi
for demuxer in $DEMUXERS; do FFMPEG_ARGS+=("--enable-demuxer=$demuxer"); done
for decoder in $DECODERS; do FFMPEG_ARGS+=("--enable-decoder=$decoder"); done
for parser in $PARSERS; do FFMPEG_ARGS+=("--enable-parser=$parser"); done

cd "$FFMPEG_BUILD"
step emconfigure "$FFMPEG_SOURCE/configure" "${FFMPEG_ARGS[@]}"

NPROC="$(getconf _NPROCESSORS_ONLN 2>/dev/null || sysctl -n hw.ncpu)"
step emmake make -j"$NPROC"
step emmake make install
cd - > /dev/null

# ---------------------------------------------------------------------------
# Link the decoder core (direct libav API glue, no ffmpeg CLI)
# ---------------------------------------------------------------------------

mkdir -p "$PACKAGE_ROOT"

MT_LDFLAGS=()
if $MT; then
    # Pool sized for decode workers; spawned inside our hosting Web Worker.
    MT_LDFLAGS=(-pthread -sPTHREAD_POOL_SIZE=4)
fi

step emcc -O2 ${SIMD_CFLAGS:+$SIMD_CFLAGS} "$REPO_ROOT/wasm/vp_decoder.c" \
    -I"$FFMPEG_INSTALL/include" \
    -L"$FFMPEG_INSTALL/lib" \
    -lavformat -lavcodec -lswscale -lavutil \
    -sMODULARIZE=1 \
    -sEXPORT_ES6=1 \
    -sEXPORT_NAME=createVoidPlayerCore \
    -sALLOW_MEMORY_GROWTH=1 \
    -sFORCE_FILESYSTEM=1 \
    -sSTACK_SIZE=4194304 \
    ${MT_LDFLAGS[@]+"${MT_LDFLAGS[@]}"} \
    -sEXPORTED_FUNCTIONS=_malloc,_free,_vp_create,_vp_destroy,_vp_set_threads,_vp_open,_vp_open_blob,_vp_close_input,_vp_width,_vp_height,_vp_tb_num,_vp_tb_den,_vp_codec_name,_vp_color_primaries,_vp_color_transfer,_vp_color_space,_vp_color_range,_vp_index_build,_vp_index_count,_vp_index_ticks,_vp_index_is_key,_vp_index_duration,_vp_extract,_vp_last_ticks,_vp_pixels \
    -sEXPORTED_RUNTIME_METHODS=FS,ccall,cwrap,HEAPU8 \
    -o "$PACKAGE_ROOT/voidplayer-core${SUFFIX}.js"

# ---------------------------------------------------------------------------
# Licenses, manifest, archive
# ---------------------------------------------------------------------------

LICENSE_ROOT="$PACKAGE_ROOT/LICENSES"
mkdir -p "$LICENSE_ROOT"
for f in "$FFMPEG_SOURCE"/COPYING*; do
    [ -f "$f" ] && cp "$f" "$LICENSE_ROOT/$(basename "$f")"
done
for name in LICENSE.md README.md; do
    [ -f "$FFMPEG_SOURCE/$name" ] && cp "$FFMPEG_SOURCE/$name" "$LICENSE_ROOT/FFmpeg-$name"
done

BUILT_AT="$(date -u +%Y-%m-%dT%H:%M:%SZ)"

cat > "$PACKAGE_ROOT/voidplayer-ffmpeg-manifest.json" <<EOF
{
  "package": "$PACKAGE_NAME",
  "target": "wasm32-emscripten",
  "ffmpegRef": "$FFMPEG_REF",
  "ffmpegRemote": "$FFMPEG_REMOTE",
  "builtAtUtc": "$BUILT_AT",
  "libraries": ["avcodec", "avformat", "avutil", "swscale"],
  "demuxers": "$(echo $DEMUXERS)",
  "decoders": "$(echo $DECODERS)",
  "audio": "not included",
  "threading": "single-threaded, no SharedArrayBuffer requirement"
}
EOF

cat > "$PACKAGE_ROOT/README.txt" <<EOF
VoidPlayer FFmpeg WASM decoder core

FFmpeg: $FFMPEG_REF (from $FFMPEG_REMOTE)
Target: wasm32 (Emscripten, single-threaded)

Trimmed decoder-only core for the VoidPlayer web prototype's fallback path:
demuxers ($DEMUXERS) and decoders ($DECODERS) for files the browser cannot
handle through mediabunny/WebCodecs. No encoders, filters, audio, network,
or ffmpeg CLI. Exposes the vp_* C API from wasm/vp_decoder.c plus Emscripten
FS/ccall/cwrap.
EOF

if ! $SKIP_PACKAGE; then
    ZIP_PATH="$DIST_ROOT/${PACKAGE_NAME}.zip"
    [ -f "$ZIP_PATH" ] && rm -f "$ZIP_PATH"
    cd "$DIST_ROOT"
    step zip -r --symlinks "$ZIP_PATH" "$PACKAGE_NAME"
    cd - > /dev/null
    echo "==> package: $ZIP_PATH"
fi

echo "==> done: $PACKAGE_ROOT"
ls -la "$PACKAGE_ROOT"
