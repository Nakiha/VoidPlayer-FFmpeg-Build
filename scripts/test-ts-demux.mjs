import assert from 'node:assert/strict';
import fs from 'node:fs';
import path from 'node:path';
import {pathToFileURL} from 'node:url';
const fixture=process.argv[2];if(!fixture)throw new Error('Usage: node scripts/test-ts-demux.mjs fixture.ts');
const bytes=fs.readFileSync(fixture), root=process.cwd();
async function load(file) {const create=(await import(pathToFileURL(path.resolve(file)).href)).default; return create({wasmBinary:fs.readFileSync(file.replace(/\.js$/,'.wasm'))});}
const play=await load(process.argv[3] ?? 'dist/voidplayer-ffmpeg-wasm-n9.0.2/voidplayer-core.js');
const analysis=await load('dist/voidplayer-analysis/voidplayer-analysis.js');
let io=0;
for(const c of [play,analysis]) {
 c.vpDemuxRead=(_h,offset,n)=>{io+=Math.min(n,bytes.length-offset);return bytes.subarray(offset,Math.min(bytes.length,offset+n));};
 c.vpBlobs=new Map([[1,{blob:{slice:(start,end)=>({start,end})},reader:{readAsArrayBuffer:({start,end})=>bytes.subarray(start,end)}}]]);
 assert.equal(c._vpd_abi(),1);
}
const ctx=play._vp_create();assert.equal(play._vp_open_blob(ctx,1,BigInt(bytes.length)),0);assert.ok(play._vp_index_build(ctx)>0);
const count=play._vp_index_count(ctx), len=play._vp_index_export_bytes(ctx), ptr=play._malloc(len);
assert.equal(play._vp_index_export(ctx,ptr,len),count);const records=play.HEAPU8.slice(ptr,ptr+len);play._free(ptr);
const rv=new DataView(records.buffer),ordered=[];
for(let i=0;i<count;i++)ordered[Number(rv.getBigUint64(i*48+40,true))]={pts:rv.getBigInt64(i*48,true),dts:rv.getBigInt64(i*48+8,true),pos:rv.getBigInt64(i*48+24,true),size:rv.getInt32(i*48+32,true),flags:rv.getUint32(i*48+36,true)};
const demuxes=[],configs=[];
function desc(c,d) {const p=c._vpd_packet(d),v=new DataView(c.HEAPU8.buffer,p,72);return {pts:v.getBigInt64(0,true),dts:v.getBigInt64(8,true),pos:v.getBigInt64(24,true),au:Number(v.getBigUint64(32,true)),stream:v.getInt32(40,true),flags:v.getUint32(44,true),payload:v.getUint32(48,true),size:v.getInt32(52,true),hash:v.getBigUint64(64,true).toString()};}
for(const c of [play,analysis]) {
 const d=c._vpd_open(-1,BigInt(bytes.length));assert.ok(d,c===play?"playback demux open":"analysis demux open");assert.equal(c._vpd_video_count(d),1);
 const configPtr=c._vpd_extra(d),configSize=c._vpd_extra_size(d);configs.push([...c.HEAPU8.slice(configPtr,configPtr+configSize)]);
 const packets=[];let status;
 while((status=c._vpd_read(d,128))!==0) {
  assert.ok(status>0,`demux error ${status}`);if(status===2)continue;
  const p=desc(c,d);assert.equal(p.au,packets.length);assert.equal(p.flags&7,7);
  assert.deepEqual({pts:p.pts,dts:p.dts,pos:p.pos,size:p.size},(({pts,dts,pos,size})=>({pts,dts,pos,size}))(ordered[p.au]));
  packets.push({...p,payload:undefined});c._vpd_release(d);
 }
 assert.equal(packets.length,count);
 demuxes.push(packets);c._vpd_close(d);
}
assert.deepEqual(demuxes[0],demuxes[1]);assert.deepEqual(configs[0],configs[1]);
const seekReports=[];
for(const c of [play,analysis]) {
 const d=c._vpd_open(-1,BigInt(bytes.length)),p=c._malloc(len);c.HEAPU8.set(records,p);assert.equal(c._vpd_bind_index(d,p,count),0);c._free(p);
 const keys=ordered.map((r,au)=>({...r,au})).filter(r=>r.flags&2);
 assert.ok(keys.length>=3,`need rear GOPs, found ${keys.length}`);
 for(const k of [keys.at(-2),keys.at(-1)]) {
  const before=io, packetsBefore=Number(c._vpd_packets(d));assert.equal(c._vpd_seek(d,k.dts),0);
  let previous=-1,read=0,found=false;
  for(let tries=0;tries<300;tries++) {const s=c._vpd_read(d,128);assert.ok(s>0);if(s===2)continue;const x=desc(c,d);assert.equal(x.flags&4,4);if(previous>=0)assert.equal(x.au,previous+1);previous=x.au;
   assert.equal(x.hash,demuxes[0][x.au].hash);c._vpd_release(d);read++;
   if(x.au===k.au+3){found=true;break;}
  }
  assert.ok(found);assert.ok(io-before<bytes.length/2);
  seekReports.push({au:k.au,readBytes:io-before,packets:Number(c._vpd_packets(d))-packetsBefore});
 }
 c._vpd_close(d);
}
const identity=[];
for(const au of [0,1,2,200,560,599,300,0]) {
 const r=ordered[au];assert.equal(play._vp_extract(ctx,r.pts),1);
 const p=play._vp_picture_info(ctx);assert.ok(p,`missing actual receive token AU ${au}`);
 const v=new DataView(play.HEAPU8.buffer,p,56);assert.equal(v.getUint32(44,true),1);assert.equal(Number(v.getBigUint64(24,true)),au);
 assert.equal(v.getBigInt64(0,true),r.pts);assert.equal(v.getBigUint64(48,true).toString(),demuxes[0][au].hash);
 identity.push(au);
}
play._vp_destroy(ctx);
const report={fixture:path.basename(fixture),packets:count,payloadHashAgreement:true,seekReports,actualReceiveTokens:identity};
console.log(JSON.stringify(report,null,2));

// This CLI owns the MT pool; bound its lifetime after all contexts close.
process.exit(0);
