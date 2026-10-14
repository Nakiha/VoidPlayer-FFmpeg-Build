// Actual format registry and container-open checks; invoke per variant so this
// process owns and terminates any pthread pool it creates.
import assert from 'node:assert/strict';
import {readFileSync,mkdtempSync,rmSync} from 'node:fs';
import {tmpdir} from 'node:os';
import path from 'node:path';
import {pathToFileURL} from 'node:url';
import {execFileSync} from 'node:child_process';
const glue=path.resolve(process.argv[2]),directory=mkdtempSync(path.join(tmpdir(),'vp-removed-demux-'));
try{
 const core=await (await import(pathToFileURL(glue))).default({wasmBinary:readFileSync(glue.replace(/\.js$/,'.wasm'))});
 assert.equal(core._vp_capability_abi(),1);assert.deepEqual(Object.keys(core).filter(n=>n.startsWith('_vpd_')),[]);
 const names=Array.from({length:core._vp_demuxer_count()},(_,i)=>core.ccall('vp_demuxer_name','string',['number'],[i])).sort();
 assert.deepEqual(names,['avi','mpeg','mpegvideo']);
 for(const [file,codec,format,removed] of [['removed.ts','mpeg2video','mpegts',true],['removed.mp4','libx264','mp4',true],['removed.mkv','ffv1','matroska',true],['residual.mpg','mpeg2video','mpeg',false],['residual.avi','ffv1','avi',false],['residual.m2v','mpeg2video','mpeg2video',false]]){
  const target=path.join(directory,file);execFileSync('ffmpeg',['-y','-v','error','-f','lavfi','-i','testsrc2=size=160x96:rate=25','-frames:v','12','-an','-c:v',codec,'-f',format,target]);
  const ctx=core._vp_create();assert.ok(ctx);core.FS.writeFile('/'+file,readFileSync(target));
  try{const opened=core.ccall('vp_open','number',['number','string'],[ctx,'/'+file]);
   if(removed)assert.notEqual(opened,0,`${file}: removed container must not open through a residual demuxer`);
   else{assert.equal(opened,0,file);assert.ok(core._vp_index_build(ctx)>0,file);const count=core._vp_index_count(ctx);assert.equal(count,12,file);}
  }finally{core._vp_destroy(ctx);core.FS.unlink('/'+file);}
 }
 console.log('PASS actual registry, removed MOV/Matroska/TS rejection, residual PS/AVI/raw index');
 rmSync(directory,{recursive:true,force:true});process.exit(0);
}catch(error){console.error(error);rmSync(directory,{recursive:true,force:true});process.exit(1);}
