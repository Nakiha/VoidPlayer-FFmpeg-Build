// Node smoke/integration harness for the WASM decoder core.
// Usage: node scripts/test-wasm-node.cjs <core-dir> [sample-file ...]
// Without sample files it only checks that the module loads and rejects
// garbage input. Sample files (absolute paths) get a full index + extract
// verification.

const path = require('path');
const fs = require('fs');

async function main() {
  const coreDir = process.argv[2];
  const samples = process.argv.slice(3);
  if (!coreDir) {
    console.error('usage: node scripts/test-wasm-node.cjs <core-dir> [sample-file ...]');
    process.exit(1);
  }
  const { pathToFileURL } = require('url');
  const glue = path.join(coreDir, 'voidplayer-core.js');
  const wasm = fs.readFileSync(path.join(coreDir, 'voidplayer-core.wasm'));
  const create = (await import(pathToFileURL(glue).href)).default;
  const core = await create({ wasmBinary: wasm });

  const ctx = core.ccall('vp_create', 'number', [], []);
  if (!ctx) throw new Error('vp_create failed');

  // Garbage input must fail cleanly, not crash.
  core.FS.writeFile('/garbage.bin', new Uint8Array([1, 2, 3, 4]));
  const garbageResult = core.ccall('vp_open', 'number', ['number', 'string'], [ctx, '/garbage.bin']);
  console.log('garbage open result:', garbageResult, '(expected non-zero)');
  if (garbageResult === 0) throw new Error('garbage input unexpectedly opened');

  for (const sample of samples) {
    const name = path.basename(sample);
    const vpath = `/vp-${name}`;
    const ctx = core.ccall('vp_create', 'number', [], []);
    core.FS.writeFile(vpath, fs.readFileSync(sample));
    const opened = core.ccall('vp_open', 'number', ['number', 'string'], [ctx, vpath]);
    if (opened !== 0) {
      console.log(`${name}: vp_open failed (${opened})`);
      core.FS.unlink(vpath);
      core.ccall('vp_destroy', null, ['number'], [ctx]);
      continue;
    }
    const width = core.ccall('vp_width', 'number', ['number'], [ctx]);
    const height = core.ccall('vp_height', 'number', ['number'], [ctx]);
    const tbNum = core.ccall('vp_tb_num', 'number', ['number'], [ctx]);
    const tbDen = core.ccall('vp_tb_den', 'number', ['number'], [ctx]);
    const codec = core.ccall('vp_codec_name', 'string', ['number'], [ctx]);
    const count = core.ccall('vp_index_build', 'number', ['number'], [ctx]);
    console.log(`${name}: codec=${codec} ${width}x${height} tb=${tbNum}/${tbDen} frames=${count}`);
    if (count <= 0) throw new Error(`${name}: empty index`);
    const first = Number(core.ccall('vp_index_ticks', 'i64', ['number', 'number'], [ctx, 0]));
    const last = Number(core.ccall('vp_index_ticks', 'i64', ['number', 'number'], [ctx, count - 1]));
    console.log(`${name}: first=${first} last=${last}`);
    // Extract first, second and last frame; verify exact pts and pixel bytes.
    for (const idx of [...new Set([0, 1, count - 1])]) {
      const target = Number(core.ccall('vp_index_ticks', 'i64', ['number', 'number'], [ctx, idx]));
      const result = core.ccall('vp_extract', 'number', ['number', 'i64'], [ctx, BigInt(target)]);
      const actual = Number(core.ccall('vp_last_ticks', 'i64', ['number'], [ctx]));
      if (result !== 1 || actual !== target) {
        throw new Error(`${name}: extract frame ${idx} -> result=${result} ticks=${actual} (expected ${target})`);
      }
      const pixels = core.ccall('vp_pixels', 'number', ['number'], [ctx]);
      const bytes = core.HEAPU8.slice(pixels, pixels + width * height * 4);
      if (bytes.length !== width * height * 4) throw new Error(`${name}: short pixel buffer`);
      const distinct = new Set(bytes.slice(0, 4096)).size;
      console.log(`${name}: frame ${idx} ticks=${actual} bytes=${bytes.length} distinctSampleBytes=${distinct}`);
      if (distinct < 2) throw new Error(`${name}: frame ${idx} looks blank`);
    }
    core.FS.unlink(vpath);
    core.ccall('vp_destroy', null, ['number'], [ctx]);
  }
  console.log('OK');
}

main().catch(error => {
  console.error('FAIL:', error.message);
  process.exit(1);
});
