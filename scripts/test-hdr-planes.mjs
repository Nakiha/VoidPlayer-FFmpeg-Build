// Exact HDR samples/metadata through both core variants; each invocation exits
// to bound pthread lifetime. Requires ffmpeg with libx265 for tiny fixtures.
import assert from 'node:assert/strict';
import { mkdtempSync, readFileSync, rmSync } from 'node:fs';
import { tmpdir } from 'node:os';
import path from 'node:path';
import { pathToFileURL } from 'node:url';
import { execFileSync } from 'node:child_process';
const [glue] = process.argv.slice(2);
const root = mkdtempSync(path.join(tmpdir(), 'vp-hdr-planes-'));
const ffmpeg = args => execFileSync('ffmpeg', ['-hide_banner', '-loglevel', 'error', '-y', ...args], { stdio: ['ignore', 'pipe', 'pipe'] });
try {
  const create = (await import(pathToFileURL(path.resolve(glue)))).default;
  const core = await create({ wasmBinary: readFileSync(glue.replace(/\.js$/, '.wasm')) });
  const call = (name, types = [], args = [], result = 'number') => core.ccall(name, result, types, args);
  for (const [transfer, tag] of [['smpte2084', 16], ['arib-std-b67', 18]]) {
    const file = path.join(root, `${transfer}.mp4`), raw = path.join(root, 'reference.yuv');
    ffmpeg(['-f', 'lavfi', '-i', 'testsrc2=size=64x48:rate=24', '-frames:v', '4', '-an', '-vf', 'format=yuv420p10le', '-c:v', 'libx265', '-x265-params', 'pools=1:frame-threads=1:log-level=error', '-color_primaries', 'bt2020', '-color_trc', transfer, '-colorspace', 'bt2020nc', file]);
    ffmpeg(['-i', file, '-frames:v', '1', '-pix_fmt', 'yuv420p10le', '-f', 'rawvideo', raw]);
    const expected = readFileSync(raw), ctx = call('vp_create');
    core.FS.writeFile('/hdr.mp4', readFileSync(file));
    try {
      assert.equal(call('vp_open', ['number', 'string'], [ctx, '/hdr.mp4']), 0);
      assert.equal(call('vp_index_build', ['number'], [ctx]), 4);
      for (const index of [0, 3, 0]) {
        const pts = call('vp_index_ticks', ['number', 'number'], [ctx, index], 'i64');
        assert.equal(call('vp_extract', ['number', 'i64'], [ctx, pts]), 1);
        const ptr = call('vp_frame_info', ['number'], [ctx]);
        const d = new DataView(core.HEAPU8.buffer, ptr, 160);
        assert.equal(d.getUint32(16, true), 2); assert.equal(d.getUint32(20, true), 160);
        assert.equal(d.getInt32(44, true), 9); assert.equal(d.getInt32(48, true), tag); assert.equal(d.getInt32(52, true), 9);
        assert.equal(d.getInt32(72, true), 1, 'HDR must use original YUV planes, not swscale RGBA');
        assert.equal(d.getInt32(76, true), 10); assert.equal(d.getInt32(80, true), 0);
        assert.equal(d.getInt32(36, true), expected.length);
        if (index === 0) {
          const pixels = call('vp_pixels', ['number'], [ctx]);
          assert.deepEqual(Buffer.from(core.HEAPU8.subarray(pixels, pixels + expected.length)), expected);
        }
      }
    } finally { call('vp_destroy', ['number'], [ctx], null); }
  }
  console.log('PASS PQ/HLG: ABI v2 raw 10-bit samples match independent FFmpeg; tags and random seek preserved');
  rmSync(root, { recursive: true, force: true }); process.exit(0);
} catch (error) { console.error(error); rmSync(root, { recursive: true, force: true }); process.exit(1); }
