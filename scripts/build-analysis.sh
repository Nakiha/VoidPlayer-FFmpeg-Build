#!/usr/bin/env bash
set -euo pipefail
ROOT="$(cd "$(dirname "$0")/.." && pwd)"
BUILD="${ANALYSIS_BUILD_ROOT:-$ROOT/.build/analysis}"
OUT="${ANALYSIS_DIST_ROOT:-$ROOT/dist/voidplayer-analysis}"
REV=b726b4e422075b1670244cb5953df1dcf400f421
VP_REQUESTED_EM_CACHE="${EM_CACHE:-}"
if ! command -v emcc >/dev/null; then source "${EMSDK:-$ROOT/.toolchains/emsdk}/emsdk_env.sh" >/dev/null; fi
if [ -n "$VP_REQUESTED_EM_CACHE" ]; then export EM_CACHE="$VP_REQUESTED_EM_CACHE"; fi
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
cp "$ROOT/analysis/voidplayer_block_sort.h" "$BUILD/source/libavcodec/voidplayer_block_sort.h"
mkdir -p "$BUILD/work"
cd "$BUILD/work"
OPT="${ANALYSIS_OPT_LEVEL:-3}"
LTO="${ANALYSIS_LTO:-0}"
[[ "$OPT" =~ ^(2|3|s|z)$ ]] || { echo 'Invalid ANALYSIS_OPT_LEVEL'; exit 1; }
[[ "$LTO" =~ ^(0|1)$ ]] || { echo 'Invalid ANALYSIS_LTO'; exit 1; }
HEVC_NO_PIXELS="${ANALYSIS_HEVC_NO_PIXELS:-1}"
[[ "$HEVC_NO_PIXELS" =~ ^(0|1)$ ]] || { echo 'Invalid ANALYSIS_HEVC_NO_PIXELS'; exit 1; }
H264_SKIP_LOOP_FILTER="${ANALYSIS_H264_SKIP_LOOP_FILTER:-1}"
[[ "$H264_SKIP_LOOP_FILTER" =~ ^(0|1)$ ]] || { echo 'Invalid ANALYSIS_H264_SKIP_LOOP_FILTER'; exit 1; }
VVC_NO_PIXELS="${ANALYSIS_VVC_NO_PIXELS:-1}"
[[ "$VVC_NO_PIXELS" =~ ^(0|1)$ ]] || { echo 'Invalid ANALYSIS_VVC_NO_PIXELS'; exit 1; }
CFLAGS="-msimd128 -DVP_ANALYSIS_ONLY=1 -DVP_HEVC_NO_PIXELS=$HEVC_NO_PIXELS -DVP_VVC_NO_PIXELS=$VVC_NO_PIXELS"
if [[ "$LTO" == 1 ]]; then CFLAGS="$CFLAGS -flto"; fi
SIGNATURE="$REV:arch=wasm32:ffmpeg-simd128=1:execution=1:hevc-no-pixels=$HEVC_NO_PIXELS:vvc-no-pixels=$VVC_NO_PIXELS:demux=none:decoders=h264,hevc,vvc:parsers=h264,hevc,vvc:$OPT:$LTO:$(emcc --version | head -1)"
if [[ -f config.h && "$(cat .analysis-build-signature 2>/dev/null || true)" != "$SIGNATURE" ]]; then
  echo 'Analysis build inputs changed; select a fresh ANALYSIS_BUILD_ROOT.' >&2; exit 1
fi
if [[ ! -f config.h ]]; then
emconfigure "$BUILD/source/configure" --cc=emcc --cxx=em++ --ar=emar --ranlib=emranlib --nm=emnm \
 --target-os=none --arch=wasm32 --cpu=generic --enable-cross-compile --disable-stripping \
 --disable-programs --disable-doc --disable-debug --disable-avdevice --disable-avfilter --disable-swresample \
 --disable-swscale --disable-avformat --disable-network --disable-everything --disable-autodetect \
 --disable-pthreads --disable-w32threads --disable-os2threads --enable-avcodec --enable-avutil \
 --enable-decoder=h264,hevc,vvc --enable-parser=h264,hevc,vvc --optflags="-O$OPT" --extra-cflags="$CFLAGS"
printf '%s\n' "$SIGNATURE" > .analysis-build-signature
fi
# FFmpeg's --disable-asm also disables the WASM C-intrinsics backend.
# Check the effective config, including when an existing work directory is used.
for VP_ANALYSIS_FEATURE in ARCH_WASM HAVE_SIMD128; do
  if ! grep -Eq "^#define ${VP_ANALYSIS_FEATURE} 1$" config.h; then
    echo "Analysis backend missing ${VP_ANALYSIS_FEATURE}; select a fresh ANALYSIS_BUILD_ROOT." >&2
    exit 1
  fi
done
make -j "${ANALYSIS_BUILD_JOBS:-4}"
bash "$ROOT/scripts/build-quality-library.sh"
CORE_REV="$(git -C "$ROOT" rev-parse HEAD)"
printf '#define VP_CORE_BUILD_ID "%s"\n' "$CORE_REV" > "$BUILD/vp_core_build_id.h"
# This control changes the always-relinked collector, not cached FFmpeg objects.
emcc "-O$OPT" $CFLAGS -DVP_ANALYSIS_H264_SKIP_LOOP_FILTER="$H264_SKIP_LOOP_FILTER" -include "$BUILD/vp_core_build_id.h" "$ROOT/analysis/stream.c" "$ROOT/analysis/quality.c" "$ROOT/analysis/xpsnr.c" "$ROOT/shared/vp_capabilities.c" -DVP_NO_CONTAINER_DEMUX -I "$BUILD/source" -I "$BUILD/work" \
 -I "$BUILD/quality/source/libvmaf/include" -I "$BUILD/quality/work/include" \
 libavcodec/libavcodec.a libavutil/libavutil.a "$BUILD/quality/work/src/libvmaf.a" -sDEFAULT_TO_CXX=1 \
 -s MODULARIZE=1 -s EXPORT_ES6=1 -s ENVIRONMENT=web,worker,node -s ALLOW_MEMORY_GROWTH=1 \
 -s EXPORTED_RUNTIME_METHODS='["HEAPU8"]' -s STACK_SIZE=4194304 -s INITIAL_MEMORY=67108864 -s MAXIMUM_MEMORY=536870912 -s FILESYSTEM=0 \
 -s EXPORTED_FUNCTIONS='["_malloc","_free","_vpa_xpsnr_abi","_vpa_xpsnr_open","_vpa_xpsnr_feed","_vpa_xpsnr_score","_vpa_xpsnr_pool","_vpa_xpsnr_flush","_vpa_xpsnr_close","_vpa_abi","_vpa_quality_abi","_vpa_quality_error","_vpa_quality_open","_vpa_quality_feed","_vpa_quality_flush","_vpa_quality_score","_vpa_quality_pool","_vpa_quality_heat_frame","_vpa_quality_heat_data","_vpa_quality_heat_width","_vpa_quality_heat_height","_vpa_quality_close","_vpa_execution_abi","_vpa_execution_mode","_vpa_execution_path","_vpa_execution_reason","_vpa_execution_retry_code","_vpa_open","_vpa_feed","_vpa_drain","_vpa_step","_vpa_count","_vpa_blocks","_vpa_ordinal","_vpa_width","_vpa_height","_vpa_invalid","_vpa_depth","_vpa_pts","_vpa_take","_vpa_record_bytes","_vpa_close","_vpa_reference_abi","_vpa_ref_count","_vpa_refs","_vpa_refs_invalid","_vpa_picture_type","_vpa_poc","_vpa_feed_known","_vp_capability_abi","_vp_build_id","_vp_demuxer_count","_vp_demuxer_name","_vp_decoder_count","_vp_decoder_name"]' \
 -o "$OUT/voidplayer-analysis.js"
node "$ROOT/scripts/write-capabilities.mjs" "$OUT" analysis voidplayer-analysis "" "h264 hevc vvc"
cp "$BUILD/source/COPYING.LGPLv2.1" "$OUT/LICENSE"
cp "$BUILD/quality/source/LICENSE" "$OUT/LICENSE.libvmaf"
cp "$BUILD/quality/source/libvmaf/src/svm.h" "$BUILD/quality/svm-license-source.txt"
# LIBSVM attribution is at the start of its source, retain the complete notice.
sed -n '1,/^#ifndef/{ /^#ifndef/!p; }' "$BUILD/quality/svm-license-source.txt" > "$OUT/LICENSE.libsvm"
python3 - "$ROOT" "$REV" "$OUT" "$OPT" "$LTO" "$HEVC_NO_PIXELS" "$VVC_NO_PIXELS" "$H264_SKIP_LOOP_FILTER" "$BUILD/quality/source" <<'PY'
import json,subprocess,sys,hashlib
root,rev,out,opt,lto,hevc_no_pixels,vvc_no_pixels,h264_skip_loop_filter,quality_source=sys.argv[1:]
json.dump({'schema':1,'h264SkipLoopFilter':h264_skip_loop_filter=='1','executionAbi':1,'hevcNoPixels':hevc_no_pixels=='1','vvcNoPixels':vvc_no_pixels=='1','optimization':{'compile':opt,'link':opt,'lto':lto=='1','simd':True},'buildId':subprocess.check_output(['git','-C',root,'rev-parse','HEAD'],text=True).strip()+':refs-'+hashlib.sha256(open(out+'/voidplayer-analysis.wasm','rb').read()).hexdigest()[:16],'referenceAbi':1,'dirty':bool(subprocess.check_output(['git','-C',root,'status','--porcelain'],text=True).strip()),'abi':1,'semanticVersion':'0.1.0','demuxAbi':0,'demuxers':[],'libraries':['avcodec','avutil','vmaf'],'qualityAbi':1,'quality':{'revision':'17a67b238ce0539bdeafdc95961abac64fa16ea8','version':'3.0.0','models':{name:hashlib.sha256(open(quality_source+'/model/'+file,'rb').read()).hexdigest() for name,file in [('vmaf_v0_6_1','vmaf_v0.6.1.json'),('vmaf_v0_6_1neg','vmaf_v0.6.1neg.json')]},'cambiWindowSize':63,'heatmapAbi':1,'xpsnr':{'abi':1,'revision':'8998a1a670c81a04d144c36f9e2d3aa680c10720','filterSha256':'4b59b32eb4a501ac74f6d767a916ef1a29833e1895b150cfa96b8e4ffb91f3d7','adapterSha256':hashlib.sha256(open(root+'/analysis/xpsnr.c','rb').read()).hexdigest(),'temporalInput':'caller-ordered-reference-first','pooling':'square-mean-root-with-low-distortion-log-fallback'},'avfilter':False},'capabilityManifest':'capabilities.json','repository':'Nakiha/VoidPlayer-FFmpeg-Build','revision':subprocess.check_output(['git','-C',root,'rev-parse','HEAD'],text=True).strip(),'ffmpegRepository':'Nakiha/FFmpeg','ffmpegRevision':rev,'emscripten':subprocess.check_output(['emcc','--version'],text=True).splitlines()[0],'threading':'single','heapMaxBytes':536870912,'recordMaxBytes':33554432,'pictureMaxBlocks':131072,'license':'LGPL-2.1-or-later'},open(out+'/manifest.json','w'),indent=2)
PY
