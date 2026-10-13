import { test } from 'node:test';
import assert from 'node:assert/strict';
import { mkdtempSync, mkdirSync, writeFileSync, rmSync } from 'node:fs';
import { tmpdir } from 'node:os';
import path from 'node:path';
import { execFileSync, spawnSync } from 'node:child_process';

test('shallow pinned source accepts an annotated tag but rejects wrong commits and dirty sources', () => {
  const root = mkdtempSync(path.join(tmpdir(), 'vp-build-source-'));
  const git = (cwd, ...args) => execFileSync('git', ['-C', cwd, ...args], { encoding: 'utf8', stdio: ['ignore', 'pipe', 'pipe'] }).trim();
  try {
    const upstream = path.join(root, 'upstream'), build = path.join(root, 'build'), bins = path.join(root, 'bin');
    mkdirSync(upstream); mkdirSync(bins); mkdirSync(path.join(build, 'sources'), { recursive: true });
    git(upstream, 'init', '-q', '--initial-branch=main');
    git(upstream, 'config', 'user.name', 'Fixture'); git(upstream, 'config', 'user.email', 'fixture@example.invalid');
    writeFileSync(path.join(upstream, 'source'), 'first'); git(upstream, 'add', '.'); git(upstream, 'commit', '-qm', 'first');
    const revision = git(upstream, 'rev-parse', 'HEAD');
    git(upstream, 'tag', '-a', 'fixture', '-m', 'Annotated release tag');
    const source = path.join(build, 'sources/ffmpeg-wasm');
    git(root, 'clone', '--depth=1', '--no-tags', `file://${upstream}`, source);
    assert.notEqual(git(upstream, 'rev-parse', 'fixture'), revision, 'tag object differs from commit');
    writeFileSync(path.join(bins, 'emcc'), '#!/bin/sh\necho fixture-emcc\n', { mode: 0o755 });
    writeFileSync(path.join(bins, 'meson'), '#!/bin/sh\necho SOURCE_VALIDATED\nexit 73\n', { mode: 0o755 });
    mkdirSync(path.join(build, 'sources/dav1d-wasm'));
    const run = () => spawnSync('bash', [new URL('build-wasm.sh', import.meta.url).pathname,
      '--ffmpeg-ref', 'fixture', '--build-root', build, '--dist-root', path.join(root, 'dist'), '--skip-package'],
      { env: { ...process.env, PATH: `${bins}${path.delimiter}${process.env.PATH}` }, encoding: 'utf8' });
    const success = run();
    assert.equal(success.status, 73, success.stdout + success.stderr);
    assert.match(success.stdout, /SOURCE_VALIDATED/);
    assert.equal(git(source, 'rev-parse', 'FETCH_HEAD^{commit}'), revision);
    git(source, 'config', 'user.name', 'Fixture'); git(source, 'config', 'user.email', 'fixture@example.invalid');
    writeFileSync(path.join(source, 'source'), 'second'); git(source, 'commit', '-qam', 'different');
    const mismatch = run(); assert.equal(mismatch.status, 1); assert.match(mismatch.stderr, /Cached FFmpeg source differs/);
    git(source, 'checkout', '--detach', revision);
    writeFileSync(path.join(source, 'source'), 'dirty');
    const dirty = run(); assert.equal(dirty.status, 1); assert.match(dirty.stderr, /tracked modifications/);
  } finally { rmSync(root, { recursive: true, force: true }); }
});
