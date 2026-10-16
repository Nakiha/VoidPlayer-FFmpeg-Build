// Run against an owned, successfully built ST or MT cache. All mutations are
// restored, including when an assertion fails. No compiler may run in a reject.
import assert from 'node:assert/strict';
import {existsSync, mkdtempSync, readFileSync, rmSync, writeFileSync} from 'node:fs';
import {tmpdir} from 'node:os';
import path from 'node:path';
import {spawnSync} from 'node:child_process';
import {fileURLToPath} from 'node:url';

const build = path.resolve(process.argv[2]);
const mt = process.argv.includes('--mt');
const simd = process.env.WASM_SIMD ?? '1';
assert.match(simd, /^[01]$/);
const work = path.join(build, 'work', `ffmpeg-wasm${mt ? '-mt' : ''}`);
const config = path.join(work, 'config.h');
const signature = path.join(work, '.build-signature');
const originalConfig = readFileSync(config, 'utf8');
const originalSignature = readFileSync(signature, 'utf8');
const backend = /:arch=wasm32:asm=1:ffmpeg-simd128=[01]:mt=(?:true|false)/;
assert.match(originalSignature, backend);
const temporary = mkdtempSync(path.join(tmpdir(), 'vp-playback-cache-'));
const marker = path.join(temporary, 'dependency-ran');
for (const command of ['meson', 'make']) {
  writeFileSync(path.join(temporary, command), '#!/bin/sh\ntouch "$VP_CACHE_MARKER"\nexit 73\n', {mode: 0o755});
}
let checks = 0;
function rejects(message, extra = {}) {
  const result = spawnSync('bash', [
    fileURLToPath(new URL('build-wasm.sh', import.meta.url)),
    '--build-root', build, '--dist-root', path.join(temporary, 'dist'),
    '--skip-package', ...(mt ? ['--mt'] : []),
  ], {
    env: {...process.env, WASM_SIMD: simd, ...extra,
      VP_CACHE_MARKER: marker, PATH: temporary + path.delimiter + process.env.PATH},
    encoding: 'utf8', timeout: 30000,
  });
  assert.equal(result.status, 1, result.stdout + result.stderr);
  assert.match(result.stdout + result.stderr, message);
  assert.equal(existsSync(marker), false, 'reject before meson or make');
  checks++;
}
try {
  rejects(/Invalid WASM_SIMD/, {WASM_SIMD: '2'});
  writeFileSync(signature, originalSignature.replace(backend, ''));
  rejects(/Build inputs changed/);
  writeFileSync(signature, originalSignature);
  rejects(/Build inputs changed/, {WASM_SIMD: simd === '1' ? '0' : '1'});
  for (const [macro, expected] of [['ARCH_WASM', '1'], ['HAVE_SIMD128', simd]]) {
    const line = `#define ${macro} ${expected}`;
    assert.ok(originalConfig.split('\n').includes(line));
    writeFileSync(config, originalConfig.replace(line, `#define ${macro} ${expected === '1' ? '0' : '1'}`));
    rejects(new RegExp(`Playback backend requires ${macro}=${expected}`));
    writeFileSync(config, originalConfig);
  }
  console.log(`PASS ${mt ? 'MT' : 'ST'} SIMD=${simd}: ${checks} invalid inputs rejected before dependency compilation`);
} finally {
  writeFileSync(config, originalConfig);
  writeFileSync(signature, originalSignature);
  rmSync(temporary, {recursive: true, force: true});
}
