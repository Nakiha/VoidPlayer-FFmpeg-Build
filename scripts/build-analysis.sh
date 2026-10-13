#!/usr/bin/env bash
set -euo pipefail
ROOT="$(cd "$(dirname "$0")/.." && pwd)"
BUILD="${ANALYSIS_BUILD_ROOT:-$ROOT/.build/analysis}"
OUT="${ANALYSIS_DIST_ROOT:-$ROOT/dist/voidplayer-analysis}"
REV=c160c1bb3f0faf3ac59b8fbf907a8e3fb604632e
if ! command -v emcc >/dev/null; then source "${EMSDK:-$ROOT/.toolchains/emsdk}/emsdk_env.sh" >/dev/null; fi
mkdir -p "$BUILD" "$OUT"
if [[ ! -d "$BUILD/source/.git" ]]; then
  INPUT_SOURCE="${ANALYSIS_FFMPEG_SOURCE:-https://github.com/Nakiha/FFmpeg.git}"
  if [[ -d "$INPUT_SOURCE/.git" || -f "$INPUT_SOURCE/.git" ]]; then
    git clone --no-checkout "$INPUT_SOURCE" "$BUILD/source"
  else
    git init "$BUILD/source"
    git -C "$BUILD/source" remote add origin "$INPUT_SOURCE"
    git -C "$BUILD/source" fetch --depth=1 origin "$REV"
  fi
fi
git -C "$BUILD/source" checkout --detach "$REV"
[[ "$(git -C "$BUILD/source" rev-parse HEAD:libavformat)" == 740392267b71cb9f0e6bd3b5b52173777b0d0939 ]] || { echo 'Demux compatibility tree mismatch' >&2; exit 1; }
[[ "$(git -C "$BUILD/source" rev-parse HEAD:libavcodec/parser.c)" == cf5d65ee1699eb6a81f4da8df41edc6a503c5241 ]] || { echo 'Parser timing descriptor source mismatch' >&2; exit 1; }
cp "$ROOT/analysis/stream.c" "$BUILD/source/libavcodec/voidplayer_vachunk.c"
mkdir -p "$BUILD/work"
cd "$BUILD/work"
OPT="${ANALYSIS_OPT_LEVEL:-3}"
LTO="${ANALYSIS_LTO:-0}"
[[ "$OPT" =~ ^(2|3|s|z)$ ]] || { echo 'Invalid ANALYSIS_OPT_LEVEL'; exit 1; }
[[ "$LTO" =~ ^(0|1)$ ]] || { echo 'Invalid ANALYSIS_LTO'; exit 1; }
CFLAGS="-msimd128"
if [[ "$LTO" == 1 ]]; then CFLAGS="$CFLAGS -flto"; fi
SIGNATURE="$REV:demux-ts-v2:$OPT:$LTO:$(emcc --version | head -1)"
if [[ -f config.h && "$(cat .analysis-build-signature 2>/dev/null || true)" != "$SIGNATURE" ]]; then
  echo 'Analysis build inputs changed; select a fresh ANALYSIS_BUILD_ROOT.' >&2; exit 1
fi
if [[ ! -f config.h ]]; then
emconfigure "$BUILD/source/configure" --cc=emcc --cxx=em++ --ar=emar --ranlib=emranlib --nm=emnm \
 --target-os=none --arch=x86_32 --cpu=generic --enable-cross-compile --disable-asm --disable-stripping \
 --disable-programs --disable-doc --disable-debug --disable-avdevice --disable-avfilter --disable-swresample \
 --disable-swscale --enable-avformat --disable-network --disable-everything --disable-autodetect \
 --disable-pthreads --disable-w32threads --disable-os2threads --enable-avcodec --enable-avutil --enable-demuxer=mpegts \
 --enable-decoder=h264,hevc,vvc --enable-parser=h264,hevc,vvc --optflags="-O$OPT" --extra-cflags="$CFLAGS"
printf '%s\n' "$SIGNATURE" > .analysis-build-signature
fi
make -j "${ANALYSIS_BUILD_JOBS:-4}"
CORE_REV="$(git -C "$ROOT" rev-parse HEAD)"
printf '#define VP_CORE_BUILD_ID "%s"\n' "$CORE_REV" > "$BUILD/vp_core_build_id.h"
emcc "-O$OPT" $CFLAGS -include "$BUILD/vp_core_build_id.h" "$ROOT/analysis/stream.c" "$ROOT/shared/vp_demux.c" "$ROOT/shared/vp_packet_demux.c" "$ROOT/shared/vp_parser_timing.c" -Wl,--wrap=av_parser_parse2 -I "$BUILD/source" -I "$BUILD/work" \
 libavformat/libavformat.a libavcodec/libavcodec.a libavutil/libavutil.a \
 -s MODULARIZE=1 -s EXPORT_ES6=1 -s ENVIRONMENT=web,worker,node -s ALLOW_MEMORY_GROWTH=1 \
 -s EXPORTED_RUNTIME_METHODS='["HEAPU8"]' -s STACK_SIZE=4194304 -s INITIAL_MEMORY=67108864 -s MAXIMUM_MEMORY=536870912 -s FILESYSTEM=0 \
 -s EXPORTED_FUNCTIONS='["_malloc","_free","_vpa_abi","_vpa_open","_vpa_feed","_vpa_drain","_vpa_step","_vpa_count","_vpa_blocks","_vpa_ordinal","_vpa_width","_vpa_height","_vpa_invalid","_vpa_depth","_vpa_pts","_vpa_take","_vpa_record_bytes","_vpa_close","_vpa_reference_abi","_vpa_ref_count","_vpa_refs","_vpa_refs_invalid","_vpa_picture_type","_vpa_poc","_vpa_feed_known","_vpd_live_contexts","_vpd_build_id","_vpd_abi","_vpd_compat_id","_vpd_open","_vpd_close","_vpd_stream","_vpd_video_count","_vpd_codec","_vpd_format","_vpd_extra_size","_vpd_extra","_vpd_tb_num","_vpd_tb_den","_vpd_bytes","_vpd_packets","_vpd_release","_vpd_packet","_vpd_bind_index","_vpd_read","_vpd_seek"]' \
 -o "$OUT/voidplayer-analysis.js"
cp "$BUILD/source/COPYING.LGPLv2.1" "$OUT/LICENSE"
python3 - "$ROOT" "$REV" "$OUT" "$OPT" "$LTO" <<'PY'
import json,subprocess,sys,hashlib
root,rev,out,opt,lto=sys.argv[1:]
json.dump({'schema':1,'optimization':{'compile':opt,'link':opt,'lto':lto=='1','simd':True},'buildId':subprocess.check_output(['git','-C',root,'rev-parse','HEAD'],text=True).strip()+':refs-'+hashlib.sha256(open(out+'/voidplayer-analysis.wasm','rb').read()).hexdigest()[:16],'referenceAbi':1,'dirty':bool(subprocess.check_output(['git','-C',root,'status','--porcelain'],text=True).strip()),'abi':1,'semanticVersion':'0.1.0','demuxAbi':1,'demuxCompatibility':'ffmpeg-9.0.2-avformat-740392267b71-ts-raw-input-v2','repository':'Nakiha/VoidPlayer-FFmpeg-Build','revision':subprocess.check_output(['git','-C',root,'rev-parse','HEAD'],text=True).strip(),'ffmpegRepository':'Nakiha/FFmpeg','ffmpegRevision':rev,'emscripten':subprocess.check_output(['emcc','--version'],text=True).splitlines()[0],'threading':'single','heapMaxBytes':536870912,'recordMaxBytes':33554432,'pictureMaxBlocks':131072,'license':'LGPL-2.1-or-later'},open(out+'/manifest.json','w'),indent=2)
PY
