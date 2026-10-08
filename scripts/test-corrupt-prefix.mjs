// Every retained frame must match the undamaged decode oracle, including the
// final B pictures, backwards seeks and an exported/imported server index.
import assert from 'node:assert/strict';
import { readFileSync } from 'node:fs';
import { createHash } from 'node:crypto';
import { pathToFileURL } from 'node:url';
const glue = process.argv[2], sample = readFileSync(process.argv[3]);
const create = (await import(pathToFileURL(glue))).default;
const core = await create({ wasmBinary: readFileSync(glue.replace(/\.js$/, '.wasm')) });
const call = (name, types = [], args = [], type = 'number') => core.ccall(name, type, types, args);
assert.equal(call('vp_index_recovery_abi_version'), 1);
const contexts = [];
function open(name, bytes) {
  core.FS.writeFile(name, bytes);
  const ctx = call('vp_create'); contexts.push(ctx);
  assert.equal(call('vp_open', ['number', 'string'], [ctx, name]), 0);
  return ctx;
}
function ticks(ctx, i) { return call('vp_index_ticks', ['number', 'number'], [ctx, i], 'i64'); }
function hash(ctx, target) {
  assert.equal(call('vp_extract', ['number', 'i64'], [ctx, target]), 1, `extract ${target}`);
  const info = call('vp_frame_info', ['number'], [ctx]);
  const size = new DataView(core.HEAPU8.buffer).getInt32(info + 36, true);
  const pixels = call('vp_pixels', ['number'], [ctx]);
  return createHash('sha256').update(core.HEAPU8.slice(pixels, pixels + size)).digest('hex');
}
try {
  const baseline = open('/original.ts', sample);
  const total = call('vp_index_build', ['number'], [baseline]);
  assert.ok(total > 30);
  assert.equal(call('vp_index_integrity', ['number'], [baseline]), 0);
  const oracle = new Map();
  for (let i = 0; i < total; i++) oracle.set(String(ticks(baseline, i)), hash(baseline, ticks(baseline, i)));
  const split = Math.floor(sample.length * 0.85 / 188) * 188;
  for (const [name, bytes] of [
    ['tail', Buffer.concat([sample, Buffer.alloc(190 * 1024, 0xa5)])],
    ['gap', Buffer.concat([sample.subarray(0, split), Buffer.alloc(190 * 1024, 0xa5), sample.subarray(split)])],
  ]) {
    const ctx = open('/' + name + '.ts', bytes);
    const count = call('vp_index_build', ['number'], [ctx]);
    assert.ok(count > 0 && count < total, `${name}: keep a conservative GOP prefix`);
    assert.equal(call('vp_index_scan_complete', ['number'], [ctx]), 1);
    assert.equal(call('vp_index_scan_failed', ['number'], [ctx]), 0);
    assert.equal(call('vp_index_integrity', ['number'], [ctx]), 1);
    const at = call('vp_index_truncated_at', ['number'], [ctx], 'i64');
    const end = call('vp_index_end_dts', ['number'], [ctx], 'i64');
    assert.ok(at >= 0n && at < BigInt(bytes.length));
    for (let i = 0; i < count; i++) assert.equal(hash(ctx, ticks(ctx, i)), oracle.get(String(ticks(ctx, i))));
    const ptr = core._malloc(count * 48);
    try {
      assert.equal(call('vp_index_export', ['number', 'number', 'number'], [ctx, ptr, count * 48]), count);
      const imported = open('/' + name + '.ts', bytes);
      assert.equal(call('vp_index_import', ['number', 'number', 'number'], [imported, ptr, count]), count);
      assert.equal(call('vp_index_apply_prefix', ['number', 'i64', 'i64'], [imported, at, end - 1n]), -1);
      assert.equal(call('vp_index_apply_prefix', ['number', 'i64', 'i64'], [imported, at, end]), 1);
      for (const i of [count - 1, 0, Math.floor(count / 2), count - 2])
        assert.equal(hash(imported, ticks(ctx, i)), oracle.get(String(ticks(ctx, i))));
    } finally { core._free(ptr); }
    console.log(`${name}: ${count}/${total} retained frames and imported seeks match the original pixels`);
  }
  // IO failure is never converted to an apparently usable, cacheable prefix.
  const ctx = call('vp_create'); contexts.push(ctx);
  let fail = false;
  core.vpBlobs = new Map([[ctx, { blob: { size: sample.length, slice(start, end) {
    return fail && start > sample.length * 0.75 ? { failure: true } : sample.slice(start, end);
  } }, reader: { readAsArrayBuffer(value) { return value.failure ? new ArrayBuffer(0) : value.buffer.slice(value.byteOffset, value.byteOffset + value.byteLength); } } }]]);
  assert.equal(call('vp_open_blob', ['number', 'number', 'i64'], [ctx, ctx, BigInt(sample.length)]), 0);
  fail = true;
  assert.ok(call('vp_index_build', ['number'], [ctx]) < 0, 'premature IO EOF is fatal');
  assert.equal(call('vp_index_integrity', ['number'], [ctx]), 0);
} finally { for (const ctx of contexts) call('vp_destroy', ['number'], [ctx], null); }
