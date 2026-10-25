#!/usr/bin/env bash
# Link a non-packaged fault-injection module from an already built ST library.
set -euo pipefail
repo_dir="$(cd "$(dirname "$0")/.." && pwd)"
build_dir="$(cd "$1" && pwd)"
test_dir="$build_dir/packet-identity-test"
mkdir -p "$test_dir"
printf '#define VP_CORE_BUILD_ID "test-only-packet-identity"\n' > "$test_dir/build-id.h"
emcc -O2 -msimd128 -DVP_PACKET_IDENTITY_TEST_FAULTS \
  "$repo_dir/wasm/vp_decoder.c" "$repo_dir/shared/vp_demux.c" "$repo_dir/shared/vp_capabilities.c" "$repo_dir/shared/vp_parser_timing.c" \
  -Wl,--wrap=av_parser_parse2 -include "$test_dir/build-id.h" \
  -I"$build_dir/work/ffmpeg-wasm-install/include" -L"$build_dir/work/ffmpeg-wasm-install/lib" \
  -L"$build_dir/work/dav1d-wasm-install/lib" -lavformat -lavcodec -lswscale -lavutil -ldav1d \
  -sMODULARIZE=1 -sEXPORT_ES6=1 -sALLOW_MEMORY_GROWTH=1 -sMAXIMUM_MEMORY=268435456 -sSTACK_SIZE=4194304 \
  -sEXPORTED_FUNCTIONS=_malloc,_free,_vp_create,_vp_destroy,_vp_set_threads,_vp_packet_open,_vp_packet_open_identity,_vp_packet_send,_vp_packet_send_identity,_vp_packet_alloc,_vp_packet_receive,_vp_packet_reset,_vp_packet_identity_abi,_vp_packet_identity_info,_vp_packet_probe_fault,_vp_packet_config_bytes,_vp_picture_abi_version,_vp_frame_info,_vp_pixels \
  -sEXPORTED_RUNTIME_METHODS=ccall,HEAPU8 -o "$test_dir/core.mjs"
node "$repo_dir/scripts/test-packet-identity.mjs" "$test_dir/core.mjs" --fault
