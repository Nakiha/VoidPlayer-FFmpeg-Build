// Node smoke/integration harness for the WASM decoder core.
// Usage: node scripts/test-wasm-node.cjs <core-dir> [sample-file ...]
// Without sample files it only checks that the module loads and rejects
// garbage input. Sample files (absolute paths) get a full index + extract
// verification.

const path = require('path');
const fs = require('fs');
const crypto = require('crypto');

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
  const indexAbi = core.ccall('vp_index_abi_version', 'number', [], []);
  const indexRecordBytes = core.ccall('vp_index_record_bytes', 'number', [], []);
  if (indexAbi !== 2 || indexRecordBytes !== 40) throw new Error('unexpected index ABI v2: ' + indexAbi + '/' + indexRecordBytes);
  if (typeof core._vp_index_export !== 'function' || typeof core._vp_index_import !== 'function' || typeof core._vp_core_build_id !== 'function') throw new Error('index ABI v2 exports are missing');

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

    // Round-trip the complete v2 index through a fresh decoder context and
    // compare an interior random-access frame, including MPEG-TS seek anchors.
    const anchorCount = core.ccall('vp_index_seek_anchors', 'number', ['number'], [ctx]);
    if (/\.(?:ts|m2ts)$/i.test(name) && anchorCount <= 0) throw new Error(`${name}: MPEG-TS index has no demux seek anchors`);
    const indexBytes = core.ccall('vp_index_export_bytes', 'number', ['number'], [ctx]);
    if (indexBytes !== count * 40) throw new Error(`${name}: wrong index byte length ${indexBytes}`);
    const indexBuffer = core._malloc(indexBytes);
    if (!indexBuffer) throw new Error(`${name}: index buffer allocation failed`);
    const importCtx = core.ccall('vp_create', 'number', [], []);
    try {
      const exported = core.ccall('vp_index_export', 'number', ['number', 'number', 'number'], [ctx, indexBuffer, indexBytes]);
      if (exported !== count) throw new Error(`${name}: export returned ${exported}`);
      const snapshot = core.HEAPU8.slice(indexBuffer, indexBuffer + indexBytes);
      const badCtx = core.ccall('vp_create', 'number', [], []);
      if (core.ccall('vp_open', 'number', ['number', 'string'], [badCtx, vpath]) !== 0) throw new Error(`${name}: malformed-import context failed to open`);
      try {
        const view = new DataView(core.HEAPU8.buffer);
        view.setUint32(indexBuffer + 36, 4, true); // unknown flags must be rejected
        if (core.ccall('vp_index_import', 'number', ['number', 'number', 'number'], [badCtx, indexBuffer, count]) >= 0) throw new Error(`${name}: malformed import unexpectedly succeeded`);
        if (core.ccall('vp_index_count', 'number', ['number'], [badCtx]) !== 0) throw new Error(`${name}: rejected import changed the index`);
        core.HEAPU8.set(snapshot, indexBuffer);
      } finally { core.ccall('vp_destroy', null, ['number'], [badCtx]); }
      if (core.ccall('vp_open', 'number', ['number', 'string'], [importCtx, vpath]) !== 0) throw new Error(`${name}: import context failed to open`);
      const imported = core.ccall('vp_index_import', 'number', ['number', 'number', 'number'], [importCtx, indexBuffer, count]);
      if (imported !== count) throw new Error(`${name}: valid import returned ${imported}`);
      if (core.ccall('vp_index_seek_anchors', 'number', ['number'], [importCtx]) !== anchorCount) throw new Error(`${name}: seek anchor count changed on import`);
      const target = Number(core.ccall('vp_index_ticks', 'i64', ['number', 'number'], [ctx, Math.floor(count * 0.73)]));
      function hashFrame(context) {
        const result = core.ccall('vp_extract', 'number', ['number', 'i64'], [context, BigInt(target)]);
        if (result !== 1 || Number(core.ccall('vp_last_ticks', 'i64', ['number'], [context])) !== target) throw new Error(`${name}: random seek failed after index transfer`);
        const info = core.ccall('vp_frame_info', 'number', ['number'], [context]);
        const bytes = new DataView(core.HEAPU8.buffer).getInt32(info + 36, true);
        const pixels = core.ccall('vp_pixels', 'number', ['number'], [context]);
        if (bytes <= 0 || !pixels) throw new Error(`${name}: invalid decoded frame buffer`);
        return crypto.createHash('sha256').update(core.HEAPU8.slice(pixels, pixels + bytes)).digest('hex');
      }
      const expectedHash = hashFrame(ctx);
      const importedHash = hashFrame(importCtx);
      if (expectedHash !== importedHash) throw new Error(`${name}: imported random-seek pixel hash differs`);
      console.log(`${name}: v2 import/export anchors=${anchorCount} random-seek hash=${importedHash.slice(0, 12)}`);
    } finally {
      core._free(indexBuffer);
      core.ccall('vp_destroy', null, ['number'], [importCtx]);
    }
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
      const info = core.ccall('vp_frame_info', 'number', ['number'], [ctx]);
      const byteLength = info ? new DataView(core.HEAPU8.buffer).getInt32(info + 36, true) : 0;
      const pixels = core.ccall('vp_pixels', 'number', ['number'], [ctx]);
      const bytes = core.HEAPU8.slice(pixels, pixels + byteLength);
      if (byteLength <= 0 || bytes.length !== byteLength) throw new Error(`${name}: short pixel buffer`);
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
