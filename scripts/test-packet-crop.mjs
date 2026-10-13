// Requires ffmpeg/libx264; invoke once per ST/MT glue to bound pthread lifetime.
import assert from 'node:assert/strict';
import { mkdtempSync, readFileSync, rmSync } from 'node:fs';
import { tmpdir } from 'node:os';
import path from 'node:path';
import { pathToFileURL } from 'node:url';
import { execFileSync } from 'node:child_process';
const [glue] = process.argv.slice(2);
const directory = mkdtempSync(path.join(tmpdir(), 'vp-packet-crop-'));
try {
  const create = (await import(pathToFileURL(path.resolve(glue)))).default;
  const core = await create({ wasmBinary: readFileSync(glue.replace(/\.js$/, '.wasm')) });
  const call = (name, types = [], args = [], result = 'number') => core.ccall(name, result, types, args);
  for (const rgb of [false, true]) {
    const file = path.join(directory, 'crop.h264');
    execFileSync('ffmpeg', ['-y', '-hide_banner', '-loglevel', 'error', '-f', 'lavfi', '-i',
      'color=black:size=64x64:rate=5', '-frames:v', '1', '-vf',
      `drawbox=x=16:y=0:w=16:h=64:color=white:t=fill,setsar=2${rgb ? ',format=rgb24' : ''}`,
      '-c:v', rgb ? 'libx264rgb' : 'libx264', '-x264-params', 'qp=17:aq-mode=0:mbtree=0:bframes=0',
      '-bsf:v', 'h264_metadata=crop_left=8:crop_right=8:crop_top=4:crop_bottom=12', file]);
    const packet = readFileSync(file), ctx = call('vp_create');
    try {
      assert.equal(call('vp_packet_open', ['number','string','number','number'], [ctx,'h264',0,0]), 0);
      const ptr = call('vp_packet_alloc', ['number','number'], [ctx,packet.length]);
      core.HEAPU8.set(packet,ptr);
      assert.equal(call('vp_packet_send',['number','i64','i64','number','number'],[ctx,0n,0n,1,0]),0);
      assert.equal(call('vp_packet_send',['number','i64','i64','number','number'],[ctx,0n,0n,0,1]),0);
      assert.equal(call('vp_packet_receive',['number','i64'],[ctx,0n]),1);
      const d = new DataView(core.HEAPU8.buffer,call('vp_frame_info',['number'],[ctx]),160);
      assert.equal(d.getUint32(16,true),2);
      assert.deepEqual([24,28,60,64,144,148,152,156].map(n=>d.getInt32(n,true)),[64,64,2,1,8,4,8,12]);
      assert.equal(d.getInt32(72,true),rgb?0:1);
      const pixels=call('vp_pixels',['number'],[ctx]);
      const stride=d.getInt32(rgb?32:100,true),step=rgb?4:1;
      assert.ok(core.HEAPU8[pixels+28*stride+24*step]>220,'coded white stripe preserves its origin');
      assert.ok(core.HEAPU8[pixels+28*stride+40*step]<25,'coded black stripe preserves its origin');
    } finally {call('vp_destroy',['number'],[ctx],null);}
  }
  console.log('PASS packet crop: coded planes/RGBA, nonzero origins, SAR and known pixels');
  rmSync(directory,{recursive:true,force:true});process.exit(0);
} catch(error) {console.error(error);rmSync(directory,{recursive:true,force:true});process.exit(1);}
