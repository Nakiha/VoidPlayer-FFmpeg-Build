// Generated records only; production modules gain no test exports or hooks.
import assert from 'node:assert/strict';
import {execFileSync} from 'node:child_process';
import {readFile,mkdir,writeFile} from 'node:fs/promises';
import {createHash} from 'node:crypto';
import {pathToFileURL} from 'node:url';
import path from 'node:path';
import {Worker} from 'node:worker_threads';

const root=path.resolve(import.meta.dirname,'..');
const [buildArg,baseArg,reportArg]=process.argv.slice(2);
assert.ok(buildArg&&baseArg&&reportArg,'Pass analysis build root, fixed PR-base source and report.json');
const build=path.resolve(buildArg),baseline=path.resolve(baseArg),reportFile=path.resolve(reportArg);
// Compiled test executables are temporary, outside the uploaded evidence tree.
const output=path.join(path.dirname(path.dirname(reportFile)),'analysis-sort-test-build');
const logsDirectory=path.join(path.dirname(reportFile),'sort-test-logs');
await mkdir(output,{recursive:true});await mkdir(logsDirectory,{recursive:true});
const run=(command,args)=>execFileSync(command,args,{encoding:'utf8',maxBuffer:8*1024*1024,timeout:120000});
const hash=async file=>createHash('sha256').update(await readFile(file)).digest('hex');
const report={node:process.version,baselineRevision:run('git',['-C',baseline,'rev-parse','HEAD']).trim(),candidateRevision:run('git',['-C',root,'rev-parse','HEAD']).trim(),checks:[]};
const source=path.join(root,'analysis/test-block-sort.c'),native=path.join(output,'sort-native');
run(process.env.CC??'cc',['-std=c11','-O1','-g','-fsanitize=address,undefined','-fno-omit-frame-pointer',source,'-o',native]);
const nativeLog=run(native,[]);await writeFile(path.join(logsDirectory,'native.log'),nativeLog);
assert.match(nativeLog,/PASS block-sort: 88398 byte-exact cases/);report.checks.push({name:'native-ASan-UBSan',cases:88398});
const flags=['-O2','-sMODULARIZE=1','-sEXPORT_ES6=1','-sENVIRONMENT=node,worker','-sALLOW_MEMORY_GROWTH=1','-sSTACK_SIZE=4194304','-sINITIAL_MEMORY=67108864','-sMAXIMUM_MEMORY=536870912','-sFILESYSTEM=0'];
const sortFile=path.join(output,'sort-tests.mjs');
run('emcc',[source,...flags,'-sEXPORTED_FUNCTIONS=["_main","_vpa_sort_cancel_work"]','-o',sortFile]);
async function main(file,label){
 const logs=[],create=(await import(pathToFileURL(file))).default;
 const core=await create({noInitialRun:true,print:line=>logs.push(line),printErr:line=>logs.push(line)});
 assert.equal(core._main(),0);const log=logs.join('\n')+'\n';await writeFile(path.join(logsDirectory,label+'.log'),log);
 assert.match(log,/PASS /);return log.trim();
}
assert.match(await main(sortFile,'wasm-sort'),/88398 byte-exact cases/);
report.checks.push({name:'Emscripten-libc-byte-exact-sort',cases:88398,wasmSha256:await hash(path.join(output,'sort-tests.wasm'))});
const includes=['-I',path.join(build,'source'),'-I',path.join(build,'work')];
const archives=[path.join(build,'work/libavcodec/libavcodec.a'),path.join(build,'work/libavutil/libavutil.a')];
for(const [label,collector] of [['baseline',path.join(baseline,'analysis/stream.c')],['candidate',path.join(root,'analysis/stream.c')]]){
 const file=path.join(output,'collector-'+label+'.mjs');
 run('emcc',[path.join(root,'analysis/test-collector-limits.c'),`-DVPA_TEST_COLLECTOR=${JSON.stringify(collector)}`,...includes,...archives,...flags,'-Wl,--wrap=avcodec_receive_frame','-sEXPORTED_FUNCTIONS=["_main"]','-o',file]);
 report.checks.push({name:'actual-collector-'+label,result:await main(file,'collector-'+label)});
}
assert.equal(report.checks[2].result,report.checks[3].result,'identical collector budget/retention behavior');
// Kill an owned worker while it is exercising maximum-size eligible heaps.
// C remains synchronous; production cancellation still destroys the worker.
const code=`import {parentPort} from 'node:worker_threads';
 import create from ${JSON.stringify(pathToFileURL(sortFile).href)};
 globalThis.__sortCancelStarted=()=>parentPort.postMessage({event:'sort-started'});
 const core=await create({noInitialRun:true});core._vpa_sort_cancel_work();
 parentPort.postMessage({event:'completed'});`;
const worker=new Worker(new URL('data:text/javascript;base64,'+Buffer.from(code).toString('base64'))),messages=[];
let timer;
try{
 await new Promise((resolve,reject)=>{timer=setTimeout(()=>reject(Error('sort worker did not start')),5000);worker.on('error',reject);worker.on('message',message=>{messages.push(message);if(message.event==='sort-started')resolve();});});
 clearTimeout(timer);const exit=await worker.terminate();assert.notEqual(exit,0);
 assert.deepEqual(messages,[{event:'sort-started'}],'cancelled sort publishes no completion');
 report.checks.push({name:'maximum-sort-worker-hard-cancel',exit,completionPublished:false});
}finally{clearTimeout(timer);await worker.terminate();}
const freshCode=`import {parentPort} from 'node:worker_threads';import create from ${JSON.stringify(pathToFileURL(sortFile).href)};const c=await create({noInitialRun:true,print:()=>{}});parentPort.postMessage({result:c._main()});`;
const fresh=new Worker(new URL('data:text/javascript;base64,'+Buffer.from(freshCode).toString('base64')));
try{
 const result=await new Promise((resolve,reject)=>{timer=setTimeout(()=>reject(Error('fresh-worker unit timeout')),30000);fresh.once('error',reject);fresh.once('message',resolve);});
 assert.deepEqual(result,{result:0});report.checks.push({name:'fresh-worker-after-cancellation',cases:88398});
}finally{clearTimeout(timer);await fresh.terminate();}
report.sortHeaderSha256=await hash(path.join(root,'analysis/voidplayer_block_sort.h'));
await writeFile(reportFile,JSON.stringify(report,null,2)+'\n');
console.log('PASS analysis sort, collector budgets, unchanged tie bytes and worker cancellation');
