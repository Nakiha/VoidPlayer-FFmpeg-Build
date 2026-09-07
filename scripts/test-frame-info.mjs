// Runs in a child process per core variant so pthreads have a bounded lifetime.
import assert from 'node:assert/strict';
import {readFileSync} from 'node:fs';
import {pathToFileURL} from 'node:url';
import path from 'node:path';
const [glue,samples]=process.argv.slice(2);
const create=(await import(pathToFileURL(path.resolve(glue)))).default;
const core=await create({wasmBinary:readFileSync(glue.replace(/\.js$/,'.wasm'))});
const call=(name,types,args,type='number')=>core.ccall(name,type,types,args);
try {
 for(const [file,expected] of [
  ['h264--extradata-reload-multi-stsd.mov',[[256,128],[256,128],[128,128],[128,128]]],
  ['hevc--extradata-reload-multi-stsd.mov',[[128,128],[128,128],[128,256],[128,256]]],
 ]){
  const ctx=call('vp_create',[],[]);core.FS.writeFile('/sample.mov',readFileSync(path.join(samples,file)));
  try{
   assert.equal(call('vp_frame_info',['number'],[ctx]),0);
   assert.equal(call('vp_open',['number','string'],[ctx,'/sample.mov']),0);
   assert.equal(call('vp_index_build',['number'],[ctx]),4);
   let revision=0;
   for(const i of [0,1,2,3,0,3,1]){
    const pts=call('vp_index_ticks',['number','number'],[ctx,i],'i64');
    assert.equal(call('vp_extract',['number','i64'],[ctx,pts]),1);
    const ptr=call('vp_frame_info',['number'],[ctx]);
    const d=new DataView(core.HEAPU8.buffer,ptr,72);
    assert.equal(d.getUint32(16,true),1);assert.equal(d.getUint32(20,true),72);
    assert.equal(d.getBigInt64(0,true),pts);
    const [w,h]=expected[i];assert.equal(d.getInt32(24,true),w);assert.equal(d.getInt32(28,true),h);
    assert.equal(d.getInt32(32,true),w*4);assert.equal(d.getInt32(36,true),w*h*4);
    assert.ok(d.getUint32(68,true)>=revision);revision=d.getUint32(68,true);
    assert.ok(call('vp_pixels',['number'],[ctx])+w*h*4<=core.HEAPU8.byteLength);
   }
   call('vp_packet_reset',['number'],[ctx],null);assert.equal(call('vp_frame_info',['number'],[ctx]),0);
  }finally{call('vp_destroy',['number'],[ctx],null);}
 }
 console.log('PASS frame ABI: actual output geometry, timestamps, length, revisions, seeking, invalidation');process.exit(0);
}catch(error){console.error(error);process.exit(1);}
