// Real B-frame/open-GOP TS regression. Compare random seeks with a full
// sequential decode oracle; bound decoded frames, not machine-dependent time.
// Usage: node scripts/test-indexed-seek.mjs <core.js> [existing-70s-25fps.ts]
import assert from 'node:assert/strict';
import { readFileSync, mkdtempSync, rmSync } from 'node:fs';
import { execFileSync } from 'node:child_process';
import { createHash } from 'node:crypto';
import { tmpdir } from 'node:os';
import path from 'node:path';
import { pathToFileURL } from 'node:url';

const glue = path.resolve(process.argv[2]);
const temporary = mkdtempSync(path.join(tmpdir(), 'vp-seek-'));
const file = process.argv[3] ?? path.join(temporary, 'mpeg2.ts');
let core, ctx;
const call = (name, types = [], args = [], type = 'number') => core.ccall(name, type, types, args);
try {
  if (!process.argv[3]) execFileSync('ffmpeg', ['-v', 'error', '-y', '-f', 'lavfi', '-i',
    'testsrc2=size=128x96:rate=25', '-t', '70', '-an', '-c:v', 'mpeg2video', '-threads', '1',
    '-g', '12', '-bf', '2', '-muxrate', '2000000', '-f', 'mpegts', file], { timeout: 30000 });
  const create = (await import(pathToFileURL(glue))).default;
  core = await create({ wasmBinary: readFileSync(glue.replace(/\.js$/, '.wasm')) });
  call('vp_set_threads', ['number'], [2], null);
  core.FS.writeFile('/sample.ts', readFileSync(file));
  ctx = call('vp_create');
  assert.equal(call('vp_open', ['number', 'string'], [ctx, '/sample.ts']), 0);
  assert.equal(call('vp_index_scan_begin', ['number'], [ctx]), 1);
  assert.equal(call('vp_index_scan_complete', ['number'], [ctx]), 0);
  assert.equal(call('vp_index_count', ['number'], [ctx]), 0, 'partial index must stay private');

  let steps = 0, previousPackets = 0;
  while (!call('vp_index_scan_complete', ['number'], [ctx])) {
    const read = call('vp_index_scan_step', ['number', 'number'], [ctx, 17]);
    assert.ok(read >= 0 && read <= 17, `step read ${read} packets with a budget of 17`);
    const packets = call('vp_index_scan_packets', ['number'], [ctx]);
    assert.equal(packets, previousPackets + read, 'packet progress must be monotonic and exact');
    previousPackets = packets;
    assert.equal(call('vp_index_count', ['number'], [ctx]), 0, 'partial index must not escape through the public API');
    assert.ok(++steps < 10000, 'scan should terminate');
  }
  assert.equal(call('vp_index_scan_failed', ['number'], [ctx]), 0);
  assert.ok(steps > 1, 'the test must exercise multiple scan steps');
  const count = call('vp_index_count', ['number'], [ctx]);
  assert.equal(count, 1750);
  assert.ok(call('vp_index_seek_anchors', ['number'], [ctx]) >= 140);
  const targets = [1600, 250, 900, 1749, 0, 11, 12, 13, 23, 24, 25, 250];
  const oracle = new Map();
  const ticks = i => call('vp_index_ticks', ['number', 'number'], [ctx, i], 'i64');
  function extract(i) {
    const target = ticks(i);
    assert.equal(call('vp_extract', ['number', 'i64'], [ctx, target]), 1, `frame ${i}`);
    assert.equal(call('vp_last_ticks', ['number'], [ctx], 'i64'), target);
    const descriptor = call('vp_frame_info', ['number'], [ctx]);
    const view = new DataView(core.HEAPU8.buffer, descriptor, 160);
    assert.equal(view.getUint32(16, true), 2);
    const pixels = call('vp_pixels', ['number'], [ctx]);
    return createHash('sha256').update(core.HEAPU8.subarray(pixels, pixels + view.getInt32(36, true))).digest('hex');
  }
  for (let i = 0; i < count; i++) { const hash = extract(i); if (targets.includes(i)) oracle.set(i, hash); }
  for (const i of targets) {
    const started = performance.now(), hash = extract(i);
    const decoded = call('vp_extract_frames', ['number'], [ctx]);
    const restarts = call('vp_extract_restarts', ['number'], [ctx]);
    assert.equal(hash, oracle.get(i), `seek ${i} must match sequential pixels`);
    assert.equal(restarts, 0, `seek ${i} must not restart at file beginning`);
    assert.ok(decoded <= 30, `seek ${i} decoded ${decoded} frames; expected at most two GOPs + reorder`);
    console.log(JSON.stringify({ frame: i, ms: Math.round(performance.now() - started), decoded, restarts }));
  }
  call('vp_destroy', ['number'], [ctx], null); ctx = undefined;
  console.log('PASS TS: indexed random seek, backward seek, GOP edges, exact PTS and full-frame pixels');
} catch (error) { console.error(error); process.exitCode = 1; }
finally {
  if (ctx) call('vp_destroy', ['number'], [ctx], null);
  rmSync(temporary, { recursive: true, force: true });
}
// Emscripten pthread workers remain parked after module use.
process.exit(process.exitCode ?? 0);
