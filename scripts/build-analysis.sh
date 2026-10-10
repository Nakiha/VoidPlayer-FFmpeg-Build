#!/usr/bin/env bash
set -euo pipefail
ROOT="$(cd "$(dirname "$0")/.." && pwd)"
BUILD="${ANALYSIS_BUILD_ROOT:-$ROOT/.build/analysis}"
OUT="${ANALYSIS_DIST_ROOT:-$ROOT/dist/voidplayer-analysis}"
REV=59196ee81d09cd5312acf7d3b3896ad530d1248b
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
cp "$ROOT/analysis/stream.c" "$BUILD/source/libavcodec/voidplayer_vachunk.c"
# The old VVC hook used a range-local decode_order. Bind to the source packet's
# COPY_OPAQUE metadata instead; fail build if the pinned hunk no longer matches.
python3 - "$BUILD/source/libavcodec/vvc/ctu.c" <<'PY'
import sys
p=sys.argv[1]
s=open(p).read()
old='info->frame_identity = (uintptr_t)(fc->decode_order + 1);'
new='info->frame_identity = (uintptr_t)fc->frame->opaque;'
assert old in s or new in s
s=s.replace(old,new)
old_mode='cu->intra_pred_mode_y,'
new_mode='cu->pred_mode == MODE_PLT ? 255 : cu->intra_pred_mode_y,'
assert old_mode in s or new_mode in s
if new_mode not in s:s=s.replace(old_mode,new_mode)
open(p,'w').write(s)
PY
python3 - "$BUILD/source" <<'PYCODE'
import sys
root=sys.argv[1]
changes={
 'libavcodec/h264_mb.c': [('h->avctx->width > 0 ? h->avctx->width : h->width','h->mb_width * 16'),('h->avctx->height > 0 ? h->avctx->height : h->height','h->mb_height * 16')],
 'libavcodec/hevc/hevcdec.c': [('s->avctx->width > 0 ? s->avctx->width : sps->width','sps->width'),('s->avctx->height > 0 ? s->avctx->height : sps->height','sps->height')]
}
for file,pairs in changes.items():
 p=root+'/'+file;s=open(p).read()
 for old,new in pairs:
  assert old in s or new in s
  s=s.replace(old,new)
 open(p,'w').write(s)
PYCODE
mkdir -p "$BUILD/work"
cd "$BUILD/work"
if [[ ! -f "$BUILD/work/config.h" ]]; then
emconfigure "$BUILD/source/configure" --cc=emcc --cxx=em++ --ar=emar --ranlib=emranlib --nm=emnm \
 --target-os=none --arch=x86_32 --cpu=generic --enable-cross-compile --disable-asm --disable-stripping \
 --disable-programs --disable-doc --disable-debug --disable-avdevice --disable-avfilter --disable-swresample \
 --disable-swscale --disable-avformat --disable-network --disable-everything --disable-autodetect \
 --disable-pthreads --disable-w32threads --disable-os2threads --enable-avcodec --enable-avutil \
 --enable-decoder=h264,hevc,vvc --enable-parser=h264,hevc,vvc --extra-cflags=-msimd128
fi
make -j "${ANALYSIS_BUILD_JOBS:-4}"
emcc -O3 -msimd128 "$ROOT/analysis/stream.c" -I "$BUILD/source" -I "$BUILD/work" \
 libavcodec/libavcodec.a libavutil/libavutil.a \
 -s MODULARIZE=1 -s EXPORT_ES6=1 -s ENVIRONMENT=web,worker,node -s ALLOW_MEMORY_GROWTH=1 \
 -s EXPORTED_RUNTIME_METHODS='["HEAPU8"]' -s STACK_SIZE=4194304 -s INITIAL_MEMORY=67108864 -s MAXIMUM_MEMORY=536870912 -s FILESYSTEM=0 \
 -s EXPORTED_FUNCTIONS='["_malloc","_free","_vpa_abi","_vpa_open","_vpa_feed","_vpa_drain","_vpa_step","_vpa_count","_vpa_blocks","_vpa_ordinal","_vpa_width","_vpa_height","_vpa_invalid","_vpa_depth","_vpa_pts","_vpa_take","_vpa_record_bytes","_vpa_close"]' \
 -o "$OUT/voidplayer-analysis.js"
cp "$BUILD/source/COPYING.LGPLv2.1" "$OUT/LICENSE"
python3 - "$ROOT" "$REV" "$OUT" <<'PY'
import json,subprocess,sys
root,rev,out=sys.argv[1:]
json.dump({'schema':1,'abi':1,'semanticVersion':'0.1.0','repository':'Nakiha/VoidPlayer-FFmpeg-Build','revision':subprocess.check_output(['git','-C',root,'rev-parse','HEAD'],text=True).strip(),'ffmpegRepository':'Nakiha/FFmpeg','ffmpegRevision':rev,'emscripten':subprocess.check_output(['emcc','--version'],text=True).splitlines()[0],'threading':'single','heapMaxBytes':536870912,'recordMaxBytes':33554432,'pictureMaxBlocks':131072,'license':'LGPL-2.1-or-later'},open(out+'/manifest.json','w'),indent=2)
PY
