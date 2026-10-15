import assert from 'node:assert/strict';
import {readFileSync,writeFileSync,mkdtempSync,existsSync,rmSync} from 'node:fs';
import {tmpdir} from 'node:os';
import path from 'node:path';
import {spawnSync} from 'node:child_process';
const build=path.resolve(process.argv[2]),work=path.join(build,'work'),config=path.join(work,'config.h'),signature=path.join(work,'.analysis-build-signature');
const originalConfig=readFileSync(config,'utf8'),originalSignature=readFileSync(signature,'utf8'),temp=mkdtempSync(path.join(tmpdir(),'vp-analysis-guard-')),marker=path.join(temp,'make-ran');
writeFileSync(path.join(temp,'make'),`#!/bin/sh\ntouch '${marker}'\nexit 73\n`,{mode:0o755});
function rejects(message){const run=spawnSync('bash',[new URL('build-analysis.sh',import.meta.url).pathname],{env:{...process.env,ANALYSIS_BUILD_ROOT:build,ANALYSIS_DIST_ROOT:path.join(temp,'dist'),PATH:temp+path.delimiter+process.env.PATH},encoding:'utf8',timeout:30000});assert.equal(run.status,1,run.stdout+run.stderr);assert.match(run.stderr,message);assert.equal(existsSync(marker),false,'invalid cached inputs must fail before make');}
try{
 writeFileSync(signature,originalSignature.replace(':arch=wasm32:ffmpeg-simd128=1',''));rejects(/inputs changed/);
 writeFileSync(signature,originalSignature);
 for(const macro of ['ARCH_WASM','HAVE_SIMD128']){assert.ok(originalConfig.includes(`#define ${macro} 1`));writeFileSync(config,originalConfig.replace(`#define ${macro} 1`,`#define ${macro} 0`));rejects(new RegExp(`backend missing ${macro}`));writeFileSync(config,originalConfig);}
 console.log('PASS old cache signature and both missing backend macros fail before compilation');
}finally{writeFileSync(config,originalConfig);writeFileSync(signature,originalSignature);rmSync(temp,{recursive:true,force:true});}
