// Explicit regeneration, never run implicitly by tests. Requires native tools
// compiled from the fixture's immutable FFmpeg revision, not the system encoder.
import assert from 'node:assert/strict';import {readFileSync,writeFileSync,mkdtempSync,rmSync} from 'node:fs';
import {execFileSync} from 'node:child_process';import {tmpdir} from 'node:os';import path from 'node:path';import {createHash} from 'node:crypto';
const [ffmpeg,ffprobe]=process.argv.slice(2);assert.ok(ffmpeg&&ffprobe,'pass fixed native ffmpeg and ffprobe');
const file=new URL('../fixtures/packet-identity/samples.json',import.meta.url),fixture=JSON.parse(readFileSync(file));
for(const tool of [ffmpeg,ffprobe])assert.ok(execFileSync(tool,['-version'],{encoding:'utf8'}).split('\n')[0].includes(fixture.oracleRevision.slice(0,10)),'oracle source revision must match');
const root=mkdtempSync(path.join(tmpdir(),'vp-packet-oracle-'));const sha=b=>createHash('sha256').update(b).digest('hex');
try{for(const s of fixture.samples){
 const input=Buffer.from(s.sourceMp4,'base64');assert.equal(sha(input),s.sourceSha256);const source=path.join(root,'source.mp4');writeFileSync(source,input);
 for(const p of s.packets)assert.deepEqual(input.subarray(p.sourcePos,p.sourcePos+Buffer.from(p.bytes,'base64').length),Buffer.from(p.bytes,'base64'));
 const frames=JSON.parse(execFileSync(ffprobe,['-v','error','-select_streams','v:0','-show_frames','-show_entries','frame=pkt_pos','-of','json',source],{encoding:'utf8'})).frames;
 s.displayAuOrder=frames.map(f=>{const au=s.packets.findIndex(p=>p.sourcePos===Number(f.pkt_pos));assert.ok(au>=0);return au;});assert.equal(new Set(s.displayAuOrder).size,s.packets.length);
 const raw=execFileSync(ffmpeg,['-v','error','-i',source,'-map','0:v:0','-pix_fmt',s.depth===8?'yuv420p':'yuv420p10le','-fps_mode','passthrough','-f','rawvideo','-'],{maxBuffer:32*1024*1024});
 assert.equal(raw.length,s.width*s.height*3/2*(s.depth===8?1:2)*s.packets.length);s.native=raw.toString('base64');s.nativeSha256=sha(raw);
}writeFileSync(file,JSON.stringify(fixture)+'\n');console.log('Refreshed fixed native raw planes and demux-byte-position identity oracle');}
finally{rmSync(root,{recursive:true,force:true});}
