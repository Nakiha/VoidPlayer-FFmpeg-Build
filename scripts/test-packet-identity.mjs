// Synthetic committed fixtures use a fixed native oracle. Run once per ST/MT
// core to bound pthread lifetime. --fault requires a separate test-only module.
import assert from 'node:assert/strict';
import {readFileSync} from 'node:fs';
import path from 'node:path';import {pathToFileURL} from 'node:url';
import {createHash} from 'node:crypto';
const [glue,mode]=process.argv.slice(2);
const samples=JSON.parse(readFileSync(new URL('../fixtures/packet-identity/samples.json',import.meta.url))).samples;
const factory=(await import(pathToFileURL(path.resolve(glue)))).default;
const core=await factory({wasmBinary:readFileSync(glue.replace(/\.js$/,'.wasm'))});
const call=(n,t=[],a=[],r='number')=>core.ccall(n,r,t,a),heap=()=>core.HEAPU8;
const hash=b=>{let n=14695981039346656037n;for(const x of b)n=BigInt.asUintN(64,(n^BigInt(x))*1099511628211n);return n;};
assert.equal(call('vp_packet_identity_abi'),1);assert.equal(call('vp_packet_config_bytes'),40);assert.equal(call('vp_picture_abi_version'),1);
const open=(s,legacy=false)=>{const c=call('vp_create'),b=Buffer.from(s.configuration,'base64'),p=core._malloc(b.length);heap().set(b,p);assert.equal(call(legacy?'vp_packet_open':'vp_packet_open_identity',['number','string','number','number'],[c,s.codec,p,b.length]),0);core._free(p);return c;};
const send=(ctx,p,token,legacy=false)=>{const b=Buffer.from(p.bytes,'base64'),ptr=call('vp_packet_alloc',['number','number'],[ctx,b.length]);assert.ok(ptr);heap().set(b,ptr);return call(legacy?'vp_packet_send':'vp_packet_send_identity',['number','i64','i64','number','number',...(legacy?[]:['i64'])],[ctx,BigInt(p.pts),BigInt(p.dts),+p.key,0,...(legacy?[]:[token])]);};
const drain=(ctx,legacy=false)=>call(legacy?'vp_packet_send':'vp_packet_send_identity',['number','i64','i64','number','number',...(legacy?[]:['i64'])],[ctx,0n,0n,0,1,...(legacy?[]:[0n])]);
let frames=0,drained=0,negative=0;
if(mode==='--fault'){
 assert.equal(typeof core._vp_packet_probe_fault,'function');
 for(const s of samples.filter(s=>s.packets.length===32))for(let fault=1;fault<=9;fault++){
  const ctx=open(s);call('vp_packet_probe_fault',['number'],[fault],null);let rejected=false;
  for(let i=0;i<s.packets.length&&!rejected;i++){
   assert.equal(send(ctx,s.packets[i],BigInt(i+1)),0);const status=call('vp_packet_receive',['number','i64'],[ctx,-9223372036854775808n]);
   if(status<0){rejected=true;assert.equal(call('vp_packet_identity_info',['number'],[ctx]),0);assert.equal(call('vp_packet_receive',['number','i64'],[ctx,-9223372036854775808n]),-1,'failed receive remains fenced');}else assert.equal(status,0);
  }
  assert.ok(rejected,`${s.codec} fault${fault}`);negative++;call('vp_destroy',['number'],[ctx],null);
 }
 console.log(`PASS packet identity: ${negative} injected missing/unknown/stale/tampered/duplicate/error/interlaced outputs refused`);process.exit(0);
}
assert.equal(typeof core._vp_packet_probe_fault,'undefined','production module must not expose test faults');
for(const s of samples){
 const native=Buffer.from(s.native,'base64'),frameBytes=s.width*s.height*3/2*(s.depth===8?1:2);
 assert.equal(createHash('sha256').update(native).digest('hex'),s.nativeSha256);assert.equal(native.length,frameBytes*s.packets.length);
 const tokens=s.packets.map((_,i)=>(1n<<60n)+BigInt(37+i*7919));const byToken=new Map(tokens.map((t,i)=>[String(t),i]));
 for(const legacy of [false,true]){
  const ctx=open(s,legacy),order=[];let sent=0,drainCount=0,generation;
  const receive=phase=>{for(;;){const status=call('vp_packet_receive',['number','i64'],[ctx,-9223372036854775808n]);assert.ok(status>=0,`${s.codec} receive`);if(!status){assert.equal(call('vp_packet_identity_info',['number'],[ctx]),0);return;}
   let au=s.displayAuOrder[order.length];
   if(!legacy){const id=call('vp_packet_identity_info',['number'],[ctx]);assert.ok(id);const d=new DataView(heap().buffer,id,64);assert.equal(d.getUint32(0,true),1);assert.equal(d.getUint32(4,true),64);
    au=byToken.get(String(d.getBigUint64(16,true)));assert.ok(Number.isInteger(au)&&au<sent&&!order.includes(au));
    const p=s.packets[au],b=Buffer.from(p.bytes,'base64');assert.equal(d.getBigInt64(24,true),BigInt(p.pts));assert.equal(d.getBigInt64(32,true),BigInt(p.dts));assert.equal(d.getBigUint64(40,true),hash(b));assert.equal(d.getInt32(48,true),b.length);assert.equal(d.getInt32(52,true),0);assert.equal(d.getUint32(60,true),1);
    generation??=d.getBigUint64(8,true);assert.equal(d.getBigUint64(8,true),generation);
   }else assert.equal(call('vp_packet_identity_info',['number'],[ctx]),0);
   const descriptor=call('vp_frame_info',['number'],[ctx]),d=new DataView(heap().buffer,descriptor,160);assert.equal(d.getUint32(16,true),2);assert.equal(d.getUint32(20,true),160);assert.equal(d.getInt32(36,true),frameBytes);assert.equal(d.getBigInt64(0,true),BigInt(s.packets[au].pts));
   const ptr=call('vp_pixels',['number'],[ctx]);assert.deepEqual(Buffer.from(heap().subarray(ptr,ptr+frameBytes)),native.subarray(order.length*frameBytes,(order.length+1)*frameBytes),`${s.codec} token-bound AU${au} native planes`);
   order.push(au);if(phase==='drain')drainCount++;
  }};
  for(let i=0;i<s.packets.length;i++){assert.equal(send(ctx,s.packets[i],tokens[i],legacy),0);sent++;receive('immediate');}
  if(!legacy&&s.packets.length===32){assert.equal(send(ctx,s.packets.at(-1),999n),-1);negative++;}
  assert.equal(drain(ctx,legacy),0);receive('drain');assert.deepEqual(order,s.displayAuOrder);assert.equal(drainCount,s.drainOutputs);
  if(!legacy){frames+=order.length;drained+=drainCount;assert.equal(drain(ctx),-1);negative++;assert.equal(call('vp_packet_send',['number','i64','i64','number','number'],[ctx,0n,0n,0,0]),-1);negative++;}
  call('vp_destroy',['number'],[ctx],null);
 }
 // Invalid input leaves no decoder side effect; reset invalidates membership.
 const ctx=open(s);assert.equal(send(ctx,s.packets[0],0n),-1);negative++;assert.equal(send(ctx,s.packets[0],1n),0);assert.equal(send(ctx,s.packets[1],1n),-1);negative++;
 assert.equal(call('vp_packet_receive',['number','i64'],[ctx,0n]),-1);negative++;call('vp_packet_reset',['number'],[ctx],null);assert.equal(call('vp_packet_identity_info',['number'],[ctx]),0);assert.equal(send(ctx,s.packets[0],1n),0);negative++;call('vp_destroy',['number'],[ctx],null);
}
console.log(`PASS packet identity: ${samples.length} fixtures, ${frames} native-byte-equal token-bound outputs, ${drained} drain outputs, ${negative} protocol checks; legacy pixel/ABI paths retained`);
process.exit(0);
