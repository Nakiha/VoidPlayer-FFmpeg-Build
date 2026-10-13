import assert from 'node:assert/strict';
import fs from 'node:fs';
import path from 'node:path';
import {pathToFileURL} from 'node:url';
const [glue='dist/voidplayer-ffmpeg-wasm-n9.0.2/voidplayer-core.js',analysisGlue='dist/voidplayer-analysis/voidplayer-analysis.js',samples='../VoidPlayer-Web/fixtures/ts-analysis']=process.argv.slice(2);
async function load(file){return (await import(pathToFileURL(path.resolve(file)).href)).default({wasmBinary:fs.readFileSync(file.replace(/\.js$/,'.wasm'))});}
const playback=await load(glue),analysis=await load(analysisGlue),hrdReports=[],admission=[];
for(const c of [playback,analysis])for(const name of ['hrd.ts','hrd-missing.ts']) {
 const bytes=fs.readFileSync(path.join(samples,name));c.vpDemuxRead=(_h,o,n)=>bytes.subarray(o,o+n);const ctx=c._vpd_open(-1,BigInt(bytes.length));assert.ok(ctx);
 const rows=[];let count=0;
 try {for(;;) {
  const r=c._vpd_read(ctx,128);if(!r)break;assert.ok(r>0);if(r===2)continue;
  const p=c._vpd_packet(ctx),v=new DataView(c.HEAPU8.buffer,p,72),au=Number(v.getBigUint64(32,true));assert.equal(au,count++);
  const flags=v.getUint32(44,true),pts=v.getBigInt64(0,true),dts=v.getBigInt64(8,true);
  if(name==='hrd-missing.ts' && [10,11,40,41].includes(au)) {assert.equal(flags&3,0);assert.equal(pts,-9223372036854775808n);assert.equal(dts,-9223372036854775808n);rows.push({au,flags,pts:String(pts),dts:String(dts)});}
  else assert.equal(flags&3,3,'present PES timing remains known');
  c._vpd_release(ctx);
 }}finally{c._vpd_close(ctx);}
 assert.equal(count,100);hrdReports.push({variant:c===playback?'playback':'analysis',name,count,rows});
}
for(const [name,count,expected] of [['progressive-single.ts',25,25],['progressive-two-slices.ts',25,0],['mbaff.ts',50,0],['mbaff-frame-sei.ts',50,0]]) {
 const c=playback,bytes=fs.readFileSync(path.join(samples,name));c.FS.writeFile('/admission.ts',bytes);const ctx=c._vp_create();let admitted=0,presentationFrames=0;
 try {
  assert.equal(c.ccall('vp_open','number',['number','string'],[ctx,'/admission.ts']),0);assert.equal(c._vp_index_build(ctx),count);
  assert.equal(!!c._vp_identity_config_bytes(ctx),expected>0 || name==='progressive-two-slices.ts','coding SPS gates configuration');
  for(let i=0;i<count;i++) {
   assert.equal(c._vp_extract(ctx,c._vp_index_ticks(ctx,i)),1);admitted+=!!c._vp_picture_info(ctx);presentationFrames+=c._vp_frame_interlaced(ctx)===0;
  }
  assert.equal(admitted,expected,`${name}: exact core receive admission`);
  if(name==='mbaff-frame-sei.ts')assert.equal(presentationFrames,50,'contradictory FRAME presentation flags cannot override coding SPS');
 }finally{c._vp_destroy(ctx);c.FS.unlink('/admission.ts');}
 admission.push({name,count,admitted,presentationFrames});
}
console.log(JSON.stringify({hrdReports,admission},null,2));process.exit(0);
