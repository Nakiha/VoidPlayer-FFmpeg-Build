import {readFileSync,writeFileSync,mkdtempSync,rmSync,openSync,writeSync,closeSync} from 'node:fs';
import {execFileSync} from 'node:child_process';import {tmpdir} from 'node:os';import path from 'node:path';import {createHash} from 'node:crypto';
import {xpsnrCases,xpsnrFrame} from './analysis-xpsnr-fixtures.mjs';
const [binaryArg,sourceArg]=process.argv.slice(2);if(!binaryArg||!sourceArg)throw Error('Pass pinned official FFmpeg binary and clean source directory');
const binary=path.resolve(binaryArg),source=path.resolve(sourceArg),revision='8998a1a670c81a04d144c36f9e2d3aa680c10720',filterSha256='4b59b32eb4a501ac74f6d767a916ef1a29833e1895b150cfa96b8e4ffb91f3d7',hash=b=>createHash('sha256').update(b).digest('hex');
if(execFileSync('git',['-C',source,'rev-parse','HEAD'],{encoding:'utf8'}).trim()!==revision||execFileSync('git',['-C',source,'diff','HEAD'],{encoding:'utf8'})||hash(readFileSync(path.join(source,'libavfilter/vf_xpsnr.c')))!==filterSha256)throw Error('Expected clean pinned official source');
const cases=[];
for(const spec of xpsnrCases){
 const dir=mkdtempSync(path.join(tmpdir(),'vp-xpsnr-oracle-'));try{
  const hashes=[];
  for(const [side,distorted]of [['reference',false],['distorted',true]]){
   const fd=openSync(path.join(dir,side+'.yuv'),'w'),digest=createHash('sha256');try{for(let i=0;i<spec.frames;i++){const b=xpsnrFrame(spec,i,distorted);writeSync(fd,b);digest.update(b);}}finally{closeSync(fd);}hashes.push(digest.digest('hex'));
  }
  const args=['-hide_banner','-nostdin','-loglevel','info','-cpuflags','0','-filter_complex_threads','1'];
  for(const side of ['reference','distorted'])args.push('-threads','1','-f','rawvideo','-pixel_format',spec.depth===8?'yuv420p':'yuv420p10le','-video_size',`${spec.width}x${spec.height}`,'-framerate',`${spec.rate.numerator}/${spec.rate.denominator}`,'-i',side+'.yuv');
  args.push('-filter_complex','[0:v][1:v]xpsnr=stats_file=score.txt:shortest=1:repeatlast=0[out]','-map','[out]','-threads','1','-f','null','-');
  execFileSync(binary,args,{cwd:dir,stdio:'pipe'});
  const text=readFileSync(path.join(dir,'score.txt'),'utf8'),number=s=>s==='inf'?'inf':Number(s),frames=[...text.matchAll(/n:\s+(\d+)\s+XPSNR y:\s+(\S+)\s+XPSNR u:\s+(\S+)\s+XPSNR v:\s+(\S+)/g)].map(m=>({index:Number(m[1])-1,planes:m.slice(2,5).map(number)}));
  const pool=/XPSNR average, (\d+) frames\s+y:\s+(\S+)\s+u:\s+(\S+)\s+v:\s+(\S+)/.exec(text);if(frames.length!==spec.frames||!pool||Number(pool[1])!==spec.frames)throw Error('Official frame count mismatch');
  cases.push({...spec,referenceSha256:hashes[0],distortedSha256:hashes[1],framesExpected:frames,pooled:pool.slice(2,5).map(number),statisticsSha256:hash(Buffer.from(text))});
 }finally{rmSync(dir,{recursive:true,force:true});}
}
writeFileSync(new URL('../fixtures/analysis/xpsnr-oracle.json',import.meta.url),JSON.stringify({revision,filterSha256,binarySha256:hash(readFileSync(binary)),cpuFlags:0,tolerance:0.000051,precision:'official CLI emits four decimal places',cases},null,2)+'\n');
console.log(`Pinned independent XPSNR oracle: ${cases.length} clips`);
