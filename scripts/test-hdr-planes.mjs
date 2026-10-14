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
    ffmpeg(['-f', 'lavfi', '-i', 'testsrc2=size=64x48:rate=24', '-frames:v', '4', '-an', '-vf', 'format=yuv420p10le', '-c:v', 'libx265', '-x265-params', `pools=1:frame-threads=1:log-level=error:colorprim=9:transfer=${tag}:colormatrix=9:range=limited`, '-color_primaries', 'bt2020', '-color_trc', transfer, '-colorspace', 'bt2020nc', file]);
    ffmpeg(['-i', file, '-frames:v', '1', '-pix_fmt', 'yuv420p10le', '-f', 'rawvideo', raw]);
    const expected = readFileSync(raw), ctx = call('vp_create');
    const metadata=JSON.parse(execFileSync('ffprobe',['-v','error','-select_streams','v:0','-show_data','-show_entries','stream=extradata:packet=pts_time,dts_time,pos,size,flags','-show_packets','-of','json',file],{encoding:'utf8'}));
    const hex=metadata.streams[0].extradata.split('\n').filter(line=>line.includes(': ')).map(line=>line.split(': ')[1].split('  ')[0].replaceAll(' ','')).join('');
    const config=Buffer.from(hex,'hex'),source=readFileSync(file),extra=core._malloc(config.length);core.HEAPU8.set(config,extra);
    try {
      assert.equal(call('vp_packet_open',['number','string','number','number'],[ctx,'hevc',extra,config.length]),0);
      for (const index of [0, 3, 0]) {
        call('vp_packet_reset',['number'],[ctx],null);
        const target=BigInt(Math.round(Number(metadata.packets.slice().sort((a,b)=>Number(a.pts_time)-Number(b.pts_time))[index].pts_time)*1e6));
        let found=false;
        const receive=()=>{let status;while((status=call('vp_packet_receive',['number','i64'],[ctx,-9007199254740991n]))>0){
          if(call('vp_last_ticks',['number'],[ctx],'i64')===target){found=true;return true;}
        }assert.equal(status,0);return false;};
        for(const packet of metadata.packets){const data=source.subarray(Number(packet.pos),Number(packet.pos)+Number(packet.size)),ptr=call('vp_packet_alloc',['number','number'],[ctx,data.length]);assert.ok(ptr);core.HEAPU8.set(data,ptr);
          assert.equal(call('vp_packet_send',['number','i64','i64','number','number'],[ctx,BigInt(Math.round(Number(packet.pts_time)*1e6)),BigInt(Math.round(Number(packet.dts_time)*1e6)),+packet.flags.includes('K'),0]),0);
          if(receive())break;
        }
        if(!found){assert.equal(call('vp_packet_send',['number','i64','i64','number','number'],[ctx,0n,0n,0,1]),0);receive();}
        assert.ok(found,'packet-fed HDR output at actual source clock');
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
    } finally { core._free(extra);call('vp_destroy', ['number'], [ctx], null); }
  }
  console.log('PASS PQ/HLG: ABI v2 raw 10-bit samples match independent FFmpeg; tags and random seek preserved');
  rmSync(root, { recursive: true, force: true }); process.exit(0);
} catch (error) { console.error(error); rmSync(root, { recursive: true, force: true }); process.exit(1); }
