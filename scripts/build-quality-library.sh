#!/usr/bin/env bash
# Pinned pixel-only dependency linked into the existing analysis artifact.
set -euo pipefail
ROOT="$(cd "$(dirname "$0")/.." && pwd)"
BUILD="${ANALYSIS_BUILD_ROOT:-$ROOT/.build/analysis}/quality"
REV=17a67b238ce0539bdeafdc95961abac64fa16ea8
mkdir -p "$BUILD"
if [[ ! -d "$BUILD/source/.git" ]]; then
  git init "$BUILD/source"
  git -C "$BUILD/source" fetch --depth=1 "${ANALYSIS_VMAF_SOURCE:-https://github.com/Netflix/vmaf.git}" "$REV"
  git -C "$BUILD/source" checkout --detach FETCH_HEAD
fi
[[ "$(git -C "$BUILD/source" rev-parse HEAD)" == "$REV" ]] || { echo 'Quality source revision mismatch' >&2; exit 1; }
# Refuse unrelated local edits. The only accepted change is our exact memory sink.
if ! git -C "$BUILD/source" diff --quiet; then
  git -C "$BUILD/source" apply --reverse --check "$ROOT/analysis/cambi-memory.patch"
  git -C "$BUILD/source" apply --reverse "$ROOT/analysis/cambi-memory.patch"
  git -C "$BUILD/source" diff --exit-code
fi
git -C "$BUILD/source" apply "$ROOT/analysis/cambi-memory.patch"
python3 - "$BUILD" <<'PY'
import pathlib, shutil, sys
build=pathlib.Path(sys.argv[1])
lines=['[binaries]']
for name,tool in [('c','emcc'),('cpp','em++'),('ar','emar'),('ranlib','emranlib'),('strip','emstrip')]:
    lines.append(f'{name} = {str(shutil.which(tool))!r}')
lines.extend(["[host_machine]", "system = 'emscripten'", "cpu_family = 'wasm32'", "cpu = 'wasm32'", "endian = 'little'", "[properties]", "needs_exe_wrapper = true", "[built-in options]", "c_args = ['-O3', '-DVP_CAMBI_MEMORY=1']", "cpp_args = ['-O3']"])
(build/'cross.ini').write_text('\n'.join(lines)+'\n')
PY
SIGNATURE="$(emcc --version | head -1):$(shasum -a 256 "$ROOT/analysis/cambi-memory.patch" | cut -d' ' -f1):$(shasum -a 256 "$BUILD/cross.ini" | cut -d' ' -f1)"
if [[ -d "$BUILD/work" && "$(cat "$BUILD/signature" 2>/dev/null || true)" != "$SIGNATURE" ]]; then
  echo 'Quality build inputs changed; select a fresh ANALYSIS_BUILD_ROOT.' >&2; exit 1
fi
if [[ ! -d "$BUILD/work" ]]; then
  meson setup "$BUILD/work" "$BUILD/source/libvmaf" --cross-file "$BUILD/cross.ini" \
    --buildtype=release --default-library=static -Denable_tests=false -Denable_docs=false \
    -Denable_asm=false -Denable_avx512=false -Dbuilt_in_models=true -Denable_float=false \
    -Denable_cuda=false -Denable_nvtx=false
  printf '%s\n' "$SIGNATURE" > "$BUILD/signature"
fi
ninja -C "$BUILD/work" src/libvmaf.a
