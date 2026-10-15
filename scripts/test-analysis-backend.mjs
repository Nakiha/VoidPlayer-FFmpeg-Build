// Generated test patterns only: no user media or playback/container ABI.
import assert from 'node:assert/strict';
import {readFile,writeFile} from 'node:fs/promises';
import {execFileSync} from 'node:child_process';
import {pathToFileURL} from 'node:url';
import path from 'node:path';
import {createHash} from 'node:crypto';
const [baselineFile,candidateFile,reportFile,...files]=process.argv.slice(2);
assert.ok(baselineFile&&candidateFile&&reportFile&&files.length);
async function verifyArtifact(file,expectedRevision){
 const directory=path.dirname(path.resolve(file)),manifest=JSON.parse(await readFile(path.join(directory,'manifest.json'))),capabilities=JSON.parse(await readFile(path.join(directory,'capabilities.json'))),wasm=await readFile(path.join(directory,'voidplayer-analysis.wasm')),sha256=createHash('sha256').update(wasm).digest('hex');
 assert.equal(manifest.dirty,false,'publishable artifact must come from a clean source checkout');
 if(expectedRevision)assert.equal(manifest.revision,expectedRevision);
 assert.equal(manifest.buildId,manifest.revision+':refs-'+sha256.slice(0,16));assert.equal(capabilities.revision,manifest.revision);assert.equal(capabilities.wasmSha256,sha256);assert.equal(manifest.ffmpegRevision,'c160c1bb3f0faf3ac59b8fbf907a8e3fb604632e');assert.equal(manifest.threading,'single');assert.deepEqual(capabilities.registeredDemuxers,[]);assert.deepEqual(capabilities.registeredDecoders,['h264','hevc','vvc']);
 const create=(await import(pathToFileURL(path.resolve(file)).href)).default,core=await create();assert.ok(!Object.keys(core).some(k=>/^_vpa_profile|^_av_(?:force_cpu_flags|get_cpu_flags)$/.test(k)),'production exports only');const p=core._vp_build_id(),end=core.HEAPU8.indexOf(0,p);assert.ok(p>0&&end>p&&end-p<=128);assert.equal(new TextDecoder().decode(core.HEAPU8.subarray(p,end)),manifest.revision);core._vpa_close();
 return {revision:manifest.revision,buildId:manifest.buildId,wasmSha256:sha256,dirty:manifest.dirty};
}
async function input(file){
 const data=await readFile(file),info=JSON.parse(execFileSync('ffprobe',['-v','error','-select_streams','v:0','-show_streams','-show_packets','-show_data','-of','json',file],{maxBuffer:32*1024*1024}));
 const stream=info.streams[0],codec={h264:1,hevc:2}[stream.codec_name];assert.ok(codec,'only generated H264/HEVC fixtures');
 const hex=stream.extradata.split('\n').filter(line=>/^[0-9a-f]{8}: /.test(line)).map(line=>line.slice(10,49).replaceAll(' ','')).join('');assert.match(hex,/^(?:[0-9a-f]{2})+$/i);const extra=Buffer.from(hex,'hex');assert.equal(extra.length,stream.extradata_size);
 assert.ok(info.packets.length>1&&info.packets.length<=2000);const packets=info.packets.map((p,au)=>{const offset=Number(p.pos),size=Number(p.size),pts=Math.round(Number(p.pts_time)*1e6),dts=Math.round(Number(p.dts_time)*1e6);assert.ok(Number.isSafeInteger(offset)&&offset>=0&&size>0&&size<=8*1024*1024&&offset+size<=data.length&&Number.isSafeInteger(pts)&&Number.isSafeInteger(dts));return {au,pts,dts,key:p.flags.includes('K'),bytes:data.subarray(offset,offset+size)};});assert.ok(packets[0].key);
 return {file:path.basename(file),sha256:createHash('sha256').update(data).digest('hex'),codec,extra,packets};
}
async function run(moduleFile,source,{split=false,dropAnchor=false,corrupt=false}={}){
 const create=(await import(pathToFileURL(path.resolve(moduleFile)).href)).default,core=await create();
 assert.equal(core._vpa_abi(),1);assert.equal(core._vpa_reference_abi(),1);
 assert.ok(!Object.keys(core).some(k=>/^_vpa_profile|^_av_(?:force_cpu_flags|get_cpu_flags)$/.test(k)),'production export surface');
 let opens=0,error=null;const records=[];
 const finish=()=>{const r=core._vpa_drain();if(r<0)return {phase:'drain',code:r};return steps(true);};
 function steps(eof=false){for(let n=0;n<4000;n++){const r=core._vpa_step();if(r<0)return {phase:'step',code:r};if(r===1){const count=core._vpa_count(),refs=core._vpa_ref_count();assert.ok(count>0&&count<=131072&&refs>=0&&refs<=64);const header=['ordinal','width','height','depth','invalid','picture_type','poc','count','ref_count','refs_invalid','record_bytes'].map(k=>core['_vpa_'+k]());header.push(core._vpa_pts());const bp=core._vpa_blocks(),rp=core._vpa_refs();assert.ok(Number.isSafeInteger(bp)&&bp>0&&bp+count*12<=core.HEAPU8.length);assert.ok(Number.isSafeInteger(rp)&&rp>=0&&rp+refs*8<=core.HEAPU8.length);const blocks=Buffer.from(core.HEAPU8.slice(bp,bp+count*12)),edges=Buffer.from(core.HEAPU8.slice(rp,rp+refs*8));assert.equal(blocks.length,count*12);assert.equal(edges.length,refs*8);records.push({header,blocks,edges,configuration:opens-1});assert.ok(core._vpa_take(8*1024*1024)>0);continue;}assert.equal(r,eof?2:0);return null;}throw Error('core made no bounded progress');}
 function open(){const p=core._malloc(source.extra.length);core.HEAPU8.set(source.extra,p);const r=core._vpa_open(source.codec,p,source.extra.length);core._free(p);assert.equal(r,0);opens++;}
 try{open();for(const packet of source.packets){if((dropAnchor||corrupt)&&packet.au>0&&packet.key)break;if(dropAnchor&&packet.au===0)continue;if(split&&packet.au>0&&packet.key&&opens===1){error=finish();if(error)break;core._vpa_close();assert.equal(core._vpa_record_bytes(),0);open();}
  const bytes=corrupt&&packet.au===0?Buffer.alloc(packet.bytes.length):packet.bytes,p=core._malloc(bytes.length);core.HEAPU8.set(bytes,p);const r=core._vpa_feed_known(p,bytes.length,packet.pts,packet.dts,3,packet.au);core._free(p);if(r<0){error={phase:'feed',code:r,au:packet.au};break;}assert.notEqual(r,2,'every picture is taken before feeding again');error=steps();if(error)break;
 }if(!error)error=finish();}finally{core._vpa_close();}
 assert.equal(core._vpa_record_bytes(),0,'decoder records released even on failure');
 if(dropAnchor||corrupt){assert.ok(error||records.length===0||records.some(r=>r.header[4]||r.header[9]),'damaged prefix cannot yield exact output');assert.ok(records.every(r=>r.header[4]||r.header[9]),'no exact result from the damaged prefix');}
 else{assert.equal(error,null);assert.equal(records.length,source.packets.length);assert.equal(new Set(records.map(r=>r.header[0])).size,records.length);for(const r of records){assert.equal(r.header[4],0);assert.equal(r.header[9],0);assert.equal(r.header[11],source.packets[r.header[0]].pts);for(let i=0;i<r.edges.length;i+=8){assert.ok(r.edges.readUInt32LE(i)<r.header[0]);assert.ok(r.edges.readUInt32LE(i+4)<=1);}}if(split)assert.equal(opens,2);}
 return {opens,error,records};
}
const artifacts={baseline:await verifyArtifact(baselineFile,process.env.BASELINE_REVISION),candidate:await verifyArtifact(candidateFile,process.env.CANDIDATE_REVISION)};
const report={artifacts,fixtureTools:execFileSync('ffmpeg',['-version'],{encoding:'utf8'}).split('\n')[0],inputs:[],checks:[]};
for(const file of files){const source=await input(file);report.inputs.push({file:source.file,sha256:source.sha256,codec:source.codec,packets:source.packets.length});for(const [name,options] of [['whole',{}],['closed-boundary-reopen',{split:true}],['missing-anchor',{dropAnchor:true}],['corrupt-anchor',{corrupt:true}]]){
 const a=await run(baselineFile,source,options),b=await run(candidateFile,source,options);assert.deepEqual(b,a,`${source.file} ${name}: every header/block/ref byte and failure must match`);report.checks.push({fixture:source.file,name,opens:b.opens,pictures:b.records.length,error:b.error,retainedBytes:0});console.log(`PASS ${source.file} ${name}: ${b.records.length} outputs, ${b.opens} opens, ${JSON.stringify(b.error)}`);
 }}await writeFile(reportFile,JSON.stringify(report,null,2)+'\n');
