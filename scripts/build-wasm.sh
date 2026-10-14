#!/usr/bin/env bash
set -euo pipefail

# ---------------------------------------------------------------------------
# VoidPlayer FFmpeg WASM build script
# Produces a trimmed single-threaded decoder core (Emscripten) for the web
# prototype's fallback path: container support mediabunny lacks and codecs
# WebCodecs lacks (FFV1, MPEG-1/2, MPEG-4 ASP, MJPEG, ProRes, H.266/VVC).
# ---------------------------------------------------------------------------

FFMPEG_REF="n9.0.2"
FFMPEG_REMOTE="https://github.com/FFmpeg/FFmpeg.git"
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
            echo "  --ffmpeg-ref REF     FFmpeg tag/branch (default: n9.0.2)"
            echo "  --ffmpeg-remote URL  FFmpeg source remote (default: FFmpeg/FFmpeg upstream)"
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

VP_REQUESTED_EM_CACHE="${EM_CACHE:-}"
if ! command -v emcc >/dev/null 2>&1; then
    [ -f "$EMSDK_ROOT/emsdk_env.sh" ] || die "emcc not on PATH and emsdk not found at $EMSDK_ROOT. Clone https://github.com/emscripten-core/emsdk there and run: emsdk install latest && emsdk activate latest"
    # shellcheck disable=SC1091
    source "$EMSDK_ROOT/emsdk_env.sh" >/dev/null
fi
command -v emcc >/dev/null 2>&1 || die "emcc not found"
# emsdk_env clears EM_CACHE; preserve an explicitly isolated build cache.
if [ -n "$VP_REQUESTED_EM_CACHE" ]; then export EM_CACHE="$VP_REQUESTED_EM_CACHE"; fi

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

# Never label a cached source checkout as a different requested revision.
if ! EXPECTED_REV="$(git -C "$FFMPEG_SOURCE" rev-parse --verify "$FFMPEG_REF^{commit}" 2>/dev/null)"; then
    step git -C "$FFMPEG_SOURCE" fetch --depth=1 origin "$FFMPEG_REF"
    EXPECTED_REV="$(git -C "$FFMPEG_SOURCE" rev-parse "FETCH_HEAD^{commit}")"
fi
[ "$(git -C "$FFMPEG_SOURCE" rev-parse HEAD)" = "$EXPECTED_REV" ] || die "Cached FFmpeg source differs from $FFMPEG_REF; use a fresh --build-root"
[ -z "$(git -C "$FFMPEG_SOURCE" status --porcelain --untracked-files=no)" ] || die "FFmpeg source has tracked modifications"

# ---------------------------------------------------------------------------
# Configure FFmpeg (trimmed to the fallback decode path)
# ---------------------------------------------------------------------------

DEMUXERS="mpegps mpegvideo avi"
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
    SIMD_CFLAGS="$SIMD_CFLAGS -pthread -DVP_MT"
fi

WASM_OPT_LEVEL="${WASM_OPT_LEVEL:-3}"
WASM_LINK_OPT_LEVEL="${WASM_LINK_OPT_LEVEL:-2}"
WASM_LTO="${WASM_LTO:-0}"
[[ "$WASM_OPT_LEVEL" =~ ^(2|3|s|z)$ && "$WASM_LINK_OPT_LEVEL" =~ ^(2|3|s|z)$ ]] || die "Invalid optimization level"
[[ "$WASM_LTO" =~ ^(0|1)$ ]] || die "Invalid WASM_LTO"
if [ "$WASM_LTO" = 1 ]; then SIMD_CFLAGS="$SIMD_CFLAGS -flto"; fi
SIMD_ARGS=("--extra-cflags=$SIMD_CFLAGS" "--extra-ldflags=$SIMD_CFLAGS" "--optflags=-O$WASM_OPT_LEVEL")
SIGNATURE="$EXPECTED_REV:demux=$DEMUXERS:decoders=$DECODERS,libdav1d:parsers=$PARSERS:$WASM_OPT_LEVEL:$WASM_LINK_OPT_LEVEL:$WASM_LTO:$SIMD_CFLAGS:$(emcc --version | head -1)"
if [ -f "$FFMPEG_BUILD/config.h" ] && [ "$(cat "$FFMPEG_BUILD/.build-signature" 2>/dev/null || true)" != "$SIGNATURE" ]; then
    die "Build inputs changed; use a fresh --build-root"
fi

# dav1d supplies software AV1 for packet-fed FLV on browsers without AV1.
DAV1D_SOURCE="$BUILD_ROOT/sources/dav1d-wasm"
DAV1D_BUILD="$BUILD_ROOT/work/dav1d-wasm${SUFFIX}"
DAV1D_INSTALL="$BUILD_ROOT/work/dav1d-wasm${SUFFIX}-install"
if [ ! -d "$DAV1D_SOURCE" ]; then
    step git clone --depth 1 --branch 1.5.1 https://github.com/videolan/dav1d.git "$DAV1D_SOURCE"
fi
step meson setup "$DAV1D_BUILD" "$DAV1D_SOURCE" --reconfigure \
    --cross-file "$DAV1D_SOURCE/package/crossfiles/wasm32.meson" \
    --prefix "$DAV1D_INSTALL" --libdir lib --default-library static --buildtype release \
    -Denable_asm=false -Denable_tools=false -Denable_tests=false -Dc_args="$SIMD_CFLAGS"
step meson compile -C "$DAV1D_BUILD"
step meson install -C "$DAV1D_BUILD"
export EM_PKG_CONFIG_PATH="$DAV1D_INSTALL/lib/pkgconfig${PKG_CONFIG_PATH:+:$PKG_CONFIG_PATH}"
DECODERS="$DECODERS libdav1d"

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
    "--enable-libdav1d"
    "--pkg-config=pkg-config"
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
if [ ! -f config.h ]; then
    step emconfigure "$FFMPEG_SOURCE/configure" "${FFMPEG_ARGS[@]}"
    printf '%s\n' "$SIGNATURE" > .build-signature
fi

NPROC="$(getconf _NPROCESSORS_ONLN 2>/dev/null || sysctl -n hw.ncpu)"
step emmake make -j"$NPROC"
step emmake make install
cd - > /dev/null

# ---------------------------------------------------------------------------
# Link the decoder core (direct libav API glue, no ffmpeg CLI)
# ---------------------------------------------------------------------------

mkdir -p "$PACKAGE_ROOT"

# Core build identity is generated from the checked-out source commit by build-wasm.sh.
[[ "$(git -C "$FFMPEG_SOURCE" rev-parse HEAD:libavformat)" == 740392267b71cb9f0e6bd3b5b52173777b0d0939 ]] || die "Demux compatibility tree mismatch"
[[ "$(git -C "$FFMPEG_SOURCE" rev-parse HEAD:libavcodec/parser.c)" == cf5d65ee1699eb6a81f4da8df41edc6a503c5241 ]] || { echo 'Parser timing descriptor source mismatch' >&2; exit 1; }
CORE_BUILD_ID="$(git -C "$REPO_ROOT" rev-parse HEAD 2>/dev/null || printf 'unknown')"
CORE_BUILD_ID_HEADER="$BUILD_ROOT/vp_core_build_id.h"
printf '#define VP_CORE_BUILD_ID "%s"\n' "$CORE_BUILD_ID" > "$CORE_BUILD_ID_HEADER"

MT_LDFLAGS=()
if $MT; then
    # Pool sized for decode workers; spawned inside our hosting Web Worker.
    MT_LDFLAGS=(-pthread -sPTHREAD_POOL_SIZE=4)
fi

step emcc "-O$WASM_LINK_OPT_LEVEL" ${SIMD_CFLAGS:+$SIMD_CFLAGS} "$REPO_ROOT/wasm/vp_decoder.c" "$REPO_ROOT/shared/vp_demux.c" "$REPO_ROOT/shared/vp_capabilities.c" "$REPO_ROOT/shared/vp_parser_timing.c" -Wl,--wrap=av_parser_parse2 \
    -include "$CORE_BUILD_ID_HEADER" \
    -I"$FFMPEG_INSTALL/include" \
    -L"$FFMPEG_INSTALL/lib" \
    -L"$DAV1D_INSTALL/lib" -lavformat -lavcodec -lswscale -lavutil -ldav1d \
    -sMODULARIZE=1 \
    -sEXPORT_ES6=1 \
    -sEXPORT_NAME=createVoidPlayerCore \
    -sALLOW_MEMORY_GROWTH=1 \
    -sFORCE_FILESYSTEM=1 \
    -sSTACK_SIZE=4194304 \
    ${MT_LDFLAGS[@]+"${MT_LDFLAGS[@]}"} \
    -sEXPORTED_FUNCTIONS=_malloc,_free,_vp_create,_vp_destroy,_vp_set_threads,_vp_open,_vp_open_blob,_vp_close_input,_vp_width,_vp_height,_vp_tb_num,_vp_tb_den,_vp_codec_name,_vp_pixel_format,_vp_color_primaries,_vp_color_transfer,_vp_color_space,_vp_color_range,_vp_index_build,_vp_index_scan_begin,_vp_index_scan_stream_begin,_vp_index_scan_step,_vp_index_scan_complete,_vp_index_scan_failed,_vp_index_recovery_abi_version,_vp_index_integrity,_vp_index_truncated_at,_vp_index_end_dts,_vp_index_apply_prefix,_vp_index_scan_packets,_vp_index_scan_decoded_packets,_vp_index_scan_bytes,_vp_index_scan_progressive_supported,_vp_index_scan_stable_count,_vp_index_scan_stable_ticks,_vp_index_count,_vp_index_seek_anchors,_vp_extract_frames,_vp_extract_restarts,_vp_index_ticks,_vp_index_is_key,_vp_index_duration,_vp_index_abi_version,_vp_index_stream_abi_version,_vp_index_record_bytes,_vp_index_export_bytes,_vp_index_export_range,_vp_index_export,_vp_index_import_begin,_vp_index_import_batch,_vp_index_import,_vp_core_build_id,_vp_stream_index,_vp_prime_first_presentable,_vp_extract,_vp_last_ticks,_vp_frame_interlaced,_vp_pixels,_vp_frame_info,_vp_frame_format,_vp_packet_open,_vp_packet_open_config,_vp_packet_config_bytes,_vp_packet_alloc,_vp_packet_send,_vp_packet_receive,_vp_packet_reset,_vp_picture_abi_version,_vp_picture_info,_vp_identity_config_bytes,_vp_identity_config,_vp_capability_abi,_vp_build_id,_vp_demuxer_count,_vp_demuxer_name,_vp_decoder_count,_vp_decoder_name \
    -sEXPORTED_RUNTIME_METHODS=FS,ccall,cwrap,HEAPU8 \
    -o "$PACKAGE_ROOT/voidplayer-core${SUFFIX}.js"

# Emscripten 6 resolves the pthread entry by basename, dropping the content
# version query on import.meta.url. Reuse the exact importing glue URL so an
# older cached main Worker cannot accidentally launch newer pthread glue.
if $MT; then
    python3 - "$PACKAGE_ROOT/voidplayer-core-mt.js" <<'PYTHON'
import pathlib, sys
p = pathlib.Path(sys.argv[1])
s = p.read_text()
old = 'new URL("voidplayer-core-mt.js",import.meta.url)'
if s.count(old) != 1:
    raise SystemExit('Emscripten pthread URL changed; review version preservation')
p.write_text(s.replace(old, 'new URL(import.meta.url)'))
PYTHON
fi

# ---------------------------------------------------------------------------
# Licenses, manifest, archive
# ---------------------------------------------------------------------------

node "$REPO_ROOT/scripts/write-capabilities.mjs" "$PACKAGE_ROOT" playback "voidplayer-core${SUFFIX}" "$DEMUXERS" "$DECODERS"

LICENSE_ROOT="$PACKAGE_ROOT/LICENSES"
mkdir -p "$LICENSE_ROOT"
for f in "$FFMPEG_SOURCE"/COPYING*; do
    [ -f "$f" ] && cp "$f" "$LICENSE_ROOT/$(basename "$f")"
done
cp "$DAV1D_SOURCE/COPYING" "$LICENSE_ROOT/dav1d-COPYING"
for name in LICENSE.md README.md; do
    [ -f "$FFMPEG_SOURCE/$name" ] && cp "$FFMPEG_SOURCE/$name" "$LICENSE_ROOT/FFmpeg-$name"
done

BUILT_AT="$(date -u +%Y-%m-%dT%H:%M:%SZ)"

cat > "$PACKAGE_ROOT/voidplayer-ffmpeg-manifest.json" <<EOF
{
  "package": "$PACKAGE_NAME",
  "target": "wasm32-emscripten",
  "demuxAbi": 0,
  "pictureAbi": 1,
  "capabilityManifest": "capabilities${SUFFIX}.json",
  "ffmpegRef": "$FFMPEG_REF",
  "ffmpegRemote": "$FFMPEG_REMOTE",
  "ffmpegRevision": "$EXPECTED_REV",
  "optimization": {"compile": "$WASM_OPT_LEVEL", "link": "$WASM_LINK_OPT_LEVEL", "lto": "$WASM_LTO", "simd": "$WASM_SIMD"},
  "builtAtUtc": "$BUILT_AT",
  "libraries": ["avcodec", "avformat", "avutil", "swscale"],
  "demuxers": "$(echo $DEMUXERS)",
  "decoders": "$(echo $DECODERS)",
  "audio": "not included",
  "threading": "$(if $MT; then echo pthreads; else echo single; fi)"
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
