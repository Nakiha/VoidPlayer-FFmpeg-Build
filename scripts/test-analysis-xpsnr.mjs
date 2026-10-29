import assert from 'node:assert/strict';import {readFileSync} from 'node:fs';import {createHash} from 'node:crypto';import {pathToFileURL} from 'node:url';import path from 'node:path';
import {xpsnrFrame} from './analysis-xpsnr-fixtures.mjs';
const directory=path.resolve(process.argv[2]??'dist/voidplayer-analysis'),oracle=JSON.parse(readFileSync(new URL('../fixtures/analysis/xpsnr-oracle.json',import.meta.url)));
const create=(await import(pathToFileURL(path.join(directory,'voidplayer-analysis.js')).href)).default;
const core=await create({wasmBinary:readFileSync(path.join(directory,'voidplayer-analysis.wasm'))});assert.equal(core._vpa_xpsnr_abi(),1);
const compare=(actual,expected)=>expected==='inf'?assert.equal(actual,Infinity):assert.ok(Number.isFinite(actual)&&Math.abs(actual-expected)<=oracle.tolerance,`${actual} vs official ${expected}`);
for(const spec of oracle.cases){
 const bytes=spec.width*spec.height*3/2*(spec.depth===8?1:2),a=core._malloc(bytes),b=core._malloc(bytes),hashes=[createHash('sha256'),createHash('sha256')];
 try{
  assert.equal(core._vpa_xpsnr_open(spec.width,spec.height,spec.depth,spec.rate.numerator,spec.rate.denominator,spec.frames),0);
  assert.notEqual(core._vpa_xpsnr_open(spec.width,spec.height,spec.depth,24,1,spec.frames),0,'cannot replace an active clip');
  assert.notEqual(core._vpa_xpsnr_flush(),0,'cannot publish incomplete history');
  assert.ok(Number.isNaN(core._vpa_xpsnr_pool(0)));
  assert.notEqual(core._vpa_xpsnr_feed(a,b,bytes,1),0,'caller-owned strict frame order');
  for(let i=0;i<spec.frames;i++){
   for(const [side,pointer]of [[0,a],[1,b]]){const raw=xpsnrFrame(spec,i,!!side);hashes[side].update(raw);core.HEAPU8.set(raw,pointer);}
   assert.equal(core._vpa_xpsnr_feed(a,b,bytes,i),0);
   for(let p=0;p<3;p++)compare(core._vpa_xpsnr_score(p),spec.framesExpected[i].planes[p]);
  }
  assert.deepEqual(hashes.map(h=>h.digest('hex')),[spec.referenceSha256,spec.distortedSha256]);
  assert.equal(core._vpa_xpsnr_flush(),0);
  for(let p=0;p<3;p++)compare(core._vpa_xpsnr_pool(p),spec.pooled[p]);
  assert.notEqual(core._vpa_xpsnr_feed(a,b,bytes,spec.frames),0);
 }finally{core._vpa_xpsnr_close();core._free(a);core._free(b);}
}
const spec=oracle.cases[0],bytes=spec.width*spec.height*3/2,a=core._malloc(bytes),b=core._malloc(bytes),heaps=[];
try{for(let repeat=0;repeat<4;repeat++){
 assert.equal(core._vpa_xpsnr_open(spec.width,spec.height,8,24,1,4096),0);
 for(let i=0;i<4096;i++){
  core.HEAPU8.set(xpsnrFrame(spec,i,false),a);core.HEAPU8.set(xpsnrFrame(spec,i,true),b);
  assert.equal(core._vpa_xpsnr_feed(a,b,bytes,i),0);
 }
 assert.equal(core._vpa_xpsnr_flush(),0);assert.ok(Number.isFinite(core._vpa_xpsnr_pool(0)));core._vpa_xpsnr_close();heaps.push(core.HEAPU8.byteLength);
}assert.equal(new Set(heaps).size,1,'no per-clip or per-frame memory growth');}finally{core._vpa_xpsnr_close();core._free(a);core._free(b);}
console.log(`Pixel-only XPSNR: ${oracle.cases.length} official clips, 4×4096 frames, exact source hashes, lifecycle and stable heap passed`);
