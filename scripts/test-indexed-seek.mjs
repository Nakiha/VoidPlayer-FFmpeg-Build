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
let core, ctx, importCtx;
const call = (name, types = [], args = [], type = 'number') => core.ccall(name, type, types, args);
try {
  if (!process.argv[3]) execFileSync('ffmpeg', ['-v', 'error', '-y', '-f', 'lavfi', '-i',
    'testsrc2=size=128x96:rate=25', '-t', '70', '-an', '-c:v', 'mpeg2video', '-threads', '1',
    '-g', '12', '-bf', '2', '-muxrate', '2000000', '-f', 'mpegts', file], { timeout: 30000 });
  const create = (await import(pathToFileURL(glue))).default;
  core = await create({ wasmBinary: readFileSync(glue.replace(/\.js$/, '.wasm')) });
  call('vp_set_threads', ['number'], [2], null);
  const sampleBytes = readFileSync(file);
  core.FS.writeFile('/sample.ts', sampleBytes);
  ctx = call('vp_create');
  assert.equal(call('vp_open', ['number', 'string'], [ctx, '/sample.ts']), 0);
  assert.equal(call('vp_index_count', ['number'], [ctx]), 0, 'first presentation must not require an index');
  assert.equal(call('vp_prime_first_presentable', ['number'], [ctx]), 1, 'first presentable frame must decode before indexing');
  const primedTicks = call('vp_last_ticks', ['number'], [ctx], 'i64');
  assert.ok(BigInt(primedTicks) >= 0n, 'the first presentable frame must skip negative preroll');
  const primedDescriptor = call('vp_frame_info', ['number'], [ctx]);
  const primedView = new DataView(core.HEAPU8.buffer, primedDescriptor, 160);
  assert.equal(primedView.getBigInt64(0, true), BigInt(primedTicks));
  const primedPixels = call('vp_pixels', ['number'], [ctx]);
  const primedHash = createHash('sha256').update(core.HEAPU8.subarray(primedPixels, primedPixels + primedView.getInt32(36, true))).digest('hex');
  assert.equal(call('vp_index_scan_stream_begin', ['number'], [ctx]), 1);
  assert.equal(call('vp_index_scan_complete', ['number'], [ctx]), 0);
  assert.equal(call('vp_index_count', ['number'], [ctx]), 0, 'legacy full-index view stays private while scanning');
  assert.equal(call('vp_index_stream_abi_version'), 1, 'record-batch ABI must be present');

  importCtx = call('vp_create');
  assert.equal(call('vp_open', ['number', 'string'], [importCtx, '/sample.ts']), 0);
  assert.equal(call('vp_prime_first_presentable', ['number'], [importCtx]), 1);
  assert.equal(call('vp_index_import_begin', ['number'], [importCtx]), 1);
  const streamedBatches = [];
  let streamedCount = 0, streamSeq = 0, partialSeek = null;
  function streamStableRanges(stableCount, safeTicks) {
    assert.ok(stableCount >= streamedCount, 'published record count must be monotonic');
    while (streamedCount < stableCount) {
      const count = Math.min(128, stableCount - streamedCount);
      const bytes = count * 40;
      const ptr = core._malloc(bytes);
      assert.ok(ptr > 0);
      try {
        assert.equal(call('vp_index_export_range', ['number', 'number', 'number', 'number', 'number'],
          [ctx, streamedCount, count, ptr, bytes]), count);
        const records = core.HEAPU8.slice(ptr, ptr + bytes);
        const batchSafeTicks = new DataView(core.HEAPU8.buffer).getBigInt64(ptr + (count - 1) * 40, true);
        assert.ok(batchSafeTicks <= safeTicks, 'a batch frontier cannot exceed the decoder frontier');
        if (streamSeq === 0 && count > 1) {
          const view = new DataView(core.HEAPU8.buffer);
          const firstPts = view.getBigInt64(ptr, true);
          const secondPts = view.getBigInt64(ptr + 40, true);
          view.setBigInt64(ptr + 40, firstPts - 1n, true);
          assert.equal(call('vp_index_import_batch',
            ['number', 'number', 'number', 'number', 'i64', 'number'],
            [importCtx, ptr, count, streamSeq, batchSafeTicks, 0]), -1,
            'an out-of-order record must be rejected before append');
          view.setBigInt64(ptr + 40, secondPts, true);
        }
        if (streamSeq > 0) {
          const view = new DataView(core.HEAPU8.buffer);
          const originalPts = view.getBigInt64(ptr, true);
          const previousPts = call('vp_index_ticks', ['number', 'number'],
            [importCtx, streamedCount - 1], 'i64');
          view.setBigInt64(ptr, previousPts - 1n, true);
          assert.equal(call('vp_index_import_batch',
            ['number', 'number', 'number', 'number', 'i64', 'number'],
            [importCtx, ptr, count, streamSeq, batchSafeTicks, 0]), -1,
            'a batch that inserts before the published prefix must be rejected');
          view.setBigInt64(ptr, originalPts, true);
        }
                assert.equal(call('vp_index_import_batch',
          ['number', 'number', 'number', 'number', 'i64', 'number'],
          [importCtx, ptr, count, streamSeq, batchSafeTicks, 0]), count);
        streamedBatches.push({ records, count, safeTicks: batchSafeTicks });
      } finally { core._free(ptr); }
      streamedCount += count;
      streamSeq++;
      if (streamSeq === 1) {
        assert.equal(call('vp_index_import_batch',
          ['number', 'number', 'number', 'number', 'i64', 'number'],
          [importCtx, 0, 0, streamSeq + 1, safeTicks, 0]), -1,
          'a skipped batch sequence must be rejected');
        assert.equal(call('vp_index_import_batch',
          ['number', 'number', 'number', 'number', 'i64', 'number'],
          [importCtx, 0, 0, streamSeq, streamedBatches[streamedBatches.length - 1].safeTicks - 1n, 0]), -1,
          'a regressing safe frontier must be rejected');
      }
      assert.equal(call('vp_index_count', ['number'], [importCtx]), streamedCount,
        'the importing decoder exposes only accepted batches');
      if (!partialSeek && streamedCount > 100) {
        const targetIndex = 80;
        const target = call('vp_index_ticks', ['number', 'number'], [importCtx, targetIndex], 'i64');
        assert.equal(call('vp_extract', ['number', 'i64'], [importCtx, target]), 1,
          'a frame behind the imported stable frontier must be seekable before finality');
        const descriptor = call('vp_frame_info', ['number'], [importCtx]);
        const view = new DataView(core.HEAPU8.buffer, descriptor, 160);
        const pixels = call('vp_pixels', ['number'], [importCtx]);
        partialSeek = { index: targetIndex, hash: createHash('sha256')
          .update(core.HEAPU8.subarray(pixels, pixels + view.getInt32(36, true))).digest('hex') };
      }
    }
  }

  let steps = 0, previousPackets = 0, previousBytes = 0, sawStableBeforeEof = false;
  while (!call('vp_index_scan_complete', ['number'], [ctx])) {
    const read = call('vp_index_scan_step', ['number', 'number'], [ctx, 17]);
    assert.ok(read >= 0 && read <= 17, `step read ${read} packets with a budget of 17`);
    const packets = call('vp_index_scan_packets', ['number'], [ctx]);
    assert.equal(packets, previousPackets + read, 'packet progress must be monotonic and exact');
    previousPackets = packets;
    const scannedBytes = Number(call('vp_index_scan_bytes', ['number'], [ctx], 'i64'));
    assert.ok(scannedBytes >= previousBytes, 'scan byte progress must be monotonic');
    assert.ok(scannedBytes <= sampleBytes.length, 'scan byte progress must stay within the media');
    previousBytes = scannedBytes;
    const complete = call('vp_index_scan_complete', ['number'], [ctx]);
    if (!complete) {
      const visibleCount = call('vp_index_count', ['number'], [ctx]);
      assert.equal(visibleCount, 0, `legacy count must stay private (read=${read}, packets=${packets}, failed=${call('vp_index_scan_failed', ['number'], [ctx])}, visibleCount=${visibleCount})`);
      const stable = call('vp_index_scan_stable_count', ['number'], [ctx]);
      assert.ok(stable >= 0, 'well-formed MPEG-TS must support the decoder-retired prefix');
      const safeTicks = call('vp_index_scan_stable_ticks', ['number'], [ctx], 'i64');
      streamStableRanges(stable, safeTicks);
      if (stable > 0) sawStableBeforeEof = true;
    }
    assert.ok(++steps < 10000, 'scan should terminate');
  }
  assert.equal(call('vp_index_scan_failed', ['number'], [ctx]), 0);
  assert.equal(call('vp_index_scan_progressive_supported', ['number'], [ctx]), 1);
  assert.ok(sawStableBeforeEof, 'the scan must publish seekable records before EOF');
  streamStableRanges(call('vp_index_scan_stable_count', ['number'], [ctx]),
    call('vp_index_scan_stable_ticks', ['number'], [ctx], 'i64'));
  assert.equal(streamedCount, call('vp_index_count', ['number'], [ctx]));
  assert.ok(partialSeek, 'a seek must complete using an imported partial index before EOF');
  const count = call('vp_index_count', ['number'], [ctx]);
  const fullBytes = call('vp_index_export_bytes', ['number'], [ctx]);
  assert.equal(fullBytes, count * 40);
  const fullPtr = core._malloc(fullBytes);
  assert.ok(fullPtr > 0);
  let expectedBytes;
  try {
    assert.equal(call('vp_index_export', ['number', 'number', 'number'], [ctx, fullPtr, fullBytes]), count);
    expectedBytes = core.HEAPU8.slice(fullPtr, fullPtr + fullBytes);
  } finally { core._free(fullPtr); }
  const receivedBytes = new Uint8Array(fullBytes);
  let byteOffset = 0;
  for (const batch of streamedBatches) {
    receivedBytes.set(batch.records, byteOffset);
    byteOffset += batch.records.length;
  }
  assert.equal(byteOffset, fullBytes);
  assert.deepEqual(receivedBytes, expectedBytes,
    'concatenated immutable batches must equal the final complete export byte-for-byte');
  assert.equal(call('vp_index_import_batch',
    ['number', 'number', 'number', 'number', 'i64', 'number'],
    [importCtx, 0, 0, streamSeq, call('vp_index_scan_stable_ticks', ['number'], [ctx], 'i64'), 1]), 0,
    'an empty terminal event must finalize a stream');
  assert.equal(call('vp_index_scan_complete', ['number'], [importCtx]), 1);
  assert.ok(steps > 1, 'the test must exercise multiple scan steps');
  assert.equal(count, 1750);
  assert.equal(call('vp_index_count', ['number'], [importCtx]), count);
  assert.ok(call('vp_index_seek_anchors', ['number'], [ctx]) >= 140);
  assert.ok(call('vp_index_seek_anchors', ['number'], [importCtx]) >= 140);
  const targets = [1600, 250, 900, 1749, 80, 0, 11, 12, 13, 23, 24, 25, 250];
  const oracle = new Map();
  const ticks = i => call('vp_index_ticks', ['number', 'number'], [ctx, i], 'i64');
  assert.equal(ticks(0), BigInt(primedTicks), 'the stable origin is the first displayable indexed frame');
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
  assert.equal(primedHash, oracle.get(0), 'the pre-index first frame must equal the full-index oracle');
  assert.equal(partialSeek.hash, oracle.get(partialSeek.index),
    'a pre-EOF partial-index seek must match the full sequential pixel oracle');
  assert.equal(call('vp_extract', ['number', 'i64'], [importCtx, ticks(1600)]), 1,
    'final imported batches must support distant random seek anchors');
  {
    const descriptor = call('vp_frame_info', ['number'], [importCtx]);
    const view = new DataView(core.HEAPU8.buffer, descriptor, 160);
    const pixels = call('vp_pixels', ['number'], [importCtx]);
    const hash = createHash('sha256').update(core.HEAPU8.subarray(pixels, pixels + view.getInt32(36, true))).digest('hex');
    assert.equal(hash, oracle.get(1600), 'server-streamed seek anchors must preserve full-index output');
  }
  for (const i of targets) {
    const started = performance.now(), hash = extract(i);
    const decoded = call('vp_extract_frames', ['number'], [ctx]);
    const restarts = call('vp_extract_restarts', ['number'], [ctx]);
    assert.equal(hash, oracle.get(i), `seek ${i} must match sequential pixels`);
    assert.equal(restarts, 0, `seek ${i} must not restart at file beginning`);
    assert.ok(decoded <= 30, `seek ${i} decoded ${decoded} frames; expected at most two GOPs + reorder`);
    console.log(JSON.stringify({ frame: i, ms: Math.round(performance.now() - started), decoded, restarts }));
  }
  call('vp_destroy', ['number'], [importCtx], null); importCtx = undefined;
  call('vp_destroy', ['number'], [ctx], null); ctx = undefined;
  console.log('PASS TS: stable record streaming, partial-index seek, full-index random seek, GOP edges, exact PTS and full-frame pixels');
} catch (error) { console.error(error); process.exitCode = 1; }
finally {
  if (importCtx) call('vp_destroy', ['number'], [importCtx], null);
  if (ctx) call('vp_destroy', ['number'], [ctx], null);
  rmSync(temporary, { recursive: true, force: true });
}
// Emscripten pthread workers remain parked after module use.
process.exit(process.exitCode ?? 0);
