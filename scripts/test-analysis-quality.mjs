import assert from 'node:assert/strict';
import {readFileSync} from 'node:fs';
import {pathToFileURL} from 'node:url';
import path from 'node:path';
const directory=path.resolve(process.argv[2]??'dist/voidplayer-analysis');
const create=(await import(pathToFileURL(path.join(directory,'voidplayer-analysis.js')).href)).default;
const core=await create({wasmBinary:readFileSync(path.join(directory,'voidplayer-analysis.wasm'))});
assert.equal(core._vpa_abi(),1);assert.equal(core._vpa_quality_abi(),1);
assert.equal(core._vpa_quality_input_abi(),1);
const width=320,height=64,bytes=width*height*3/2;
const source=new Uint8Array(bytes).fill(128);
for(let y=0;y<height;y++)for(let x=0;x<width;x++)source[y*width+x]=16+Math.floor(x/8)*5;
const ref=core._malloc(bytes),dist=core._malloc(bytes);core.HEAPU8.set(source,ref);core.HEAPU8.set(source,dist);
try{
 assert.notEqual(core._vpa_quality_open(3,0,63,64,8,8),0);
 for(let run=0;run<4;run++){
  assert.equal(core._vpa_quality_open(3,0,width,height,8,128),0);
  assert.notEqual(core._vpa_quality_open(3,0,width,height,8,128),0,'busy cannot replace active clip');
  assert.notEqual(core._vpa_quality_feed(ref,dist,bytes,1),0,'caller owns order');
  for(let index=0;index<128;index++){
   assert.equal(core._vpa_quality_feed(ref,dist,bytes,index),0);
   assert.equal(core._vpa_quality_heat_frame(),index);
   for(let scale=0;scale<5;scale++){
    assert.equal(core._vpa_quality_heat_width(scale),Math.ceil(width/2**scale));
    assert.equal(core._vpa_quality_heat_height(scale),Math.ceil(height/2**scale));
    assert.ok(core._vpa_quality_heat_data(scale)>0);
   }
  }
  assert.equal(core._vpa_quality_flush(),0);assert.equal(core._vpa_quality_flush(),0);
  for(const metric of [1,2])for(const pool of [1,2,3,4])assert.ok(Number.isFinite(core._vpa_quality_pool(metric,pool,0,127)));
  assert.notEqual(core._vpa_quality_feed(ref,dist,bytes,128),0,'flushed clip cannot accept pictures');
  assert.equal(core._vpa_quality_close(),0);assert.equal(core._vpa_quality_close(),0);
  assert.equal(core._vpa_quality_heat_data(0),0);
 }
 assert.equal(core.HEAPU8.byteLength,67108864,'Repeated clip pixels/heatmaps reclaimed');
}finally{core._vpa_quality_close();core._free(ref);core._free(dist);}
try {
 for(const same of [0,1]) {
  assert.equal(core._vpa_quality_open(3,0,width,height,8,2),0);
  assert.notEqual(core._vpa_quality_prepare(1,same),0,'Order remains host-owned');
  for(let index=0;index<2;index++) {
   assert.equal(core._vpa_quality_prepare(index,same),0);
   assert.notEqual(core._vpa_quality_prepare(index,same),0,'Only one pending input');
   assert.notEqual(core._vpa_quality_flush(),0,'Pending pictures cannot be flushed');
   for(let role=0;role<(same?1:2);role++)for(let plane=0,offset=0;plane<3;plane++) {
    const row=width/(plane?2:1),rows=height/(plane?2:1),pointer=core._vpa_quality_input_data(role,plane),stride=core._vpa_quality_input_stride(role,plane);
    assert.ok(pointer>0 && stride>=row);
    if(same)assert.equal(pointer,core._vpa_quality_input_data(1,plane),'Identical inputs share storage');
    for(let y=0;y<rows;y++)core.HEAPU8.set(source.subarray(offset+y*row,offset+(y+1)*row),pointer+y*stride);
    offset+=row*rows;
   }
   assert.equal(core._vpa_quality_commit(index),0);
   assert.equal(core._vpa_quality_input_data(0,0),0,'Commit invalidates borrowed input pointers');
   assert.notEqual(core._vpa_quality_commit(index),0,'No duplicate submission');
  }
  assert.equal(core._vpa_quality_flush(),0);
  assert.equal(core._vpa_quality_close(),0);
 }
 assert.equal(core._vpa_quality_open(2,0,width,height,8,1),0);
 assert.equal(core._vpa_quality_prepare(0,1),0);
 assert.equal(core._vpa_quality_close(),0,'Closing a pending shared picture releases both references');
 assert.equal(core._vpa_quality_input_data(0,0),0);
} finally {core._vpa_quality_close();}
console.log('Analysis pixel scoring ABI: lifecycle, temporal stream, both metrics and memory heatmaps passed');
