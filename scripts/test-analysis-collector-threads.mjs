import assert from 'node:assert/strict';
import {execFileSync} from 'node:child_process';
import {mkdir,readFile,writeFile} from 'node:fs/promises';
import path from 'node:path';
import {pathToFileURL} from 'node:url';
const root=path.resolve(import.meta.dirname,'..'),build=path.resolve(process.argv[2]),reportFile=path.resolve(process.argv[3]);
const out=path.join(path.dirname(reportFile),'collector-thread-build');await mkdir(out,{recursive:true});
const moduleFile=path.join(out,'collector.mjs');
execFileSync('emcc',[path.join(root,'analysis/test-collector-threads.c'),'-O3','-pthread','-msimd128',
 '-DVP_ANALYSIS_ONLY=1','-DVP_HEVC_NO_PIXELS=1','-DVP_VVC_NO_PIXELS=1',
 '-I'+path.join(build,'source'),'-I'+path.join(build,'work'),
 path.join(build,'work/libavcodec/libavcodec.a'),path.join(build,'work/libavutil/libavutil.a'),
 '-sMODULARIZE=1','-sEXPORT_ES6=1','-sENVIRONMENT=node','-sALLOW_MEMORY_GROWTH=1','-sPTHREAD_POOL_SIZE=4',
 '-sSTACK_SIZE=4194304','-sINITIAL_MEMORY=67108864','-sMAXIMUM_MEMORY=1073741824','-sFILESYSTEM=0',
 '-sEXPORTED_RUNTIME_METHODS=["PThread"]','-sEXPORTED_FUNCTIONS=["_main"]','-o',moduleFile],{stdio:'inherit',timeout:120000});
const create=(await import(pathToFileURL(moduleFile).href)).default,logs=[];
const core=await create({noInitialRun:true,print:line=>logs.push(line)});
try{assert.equal(core._main(),0);}finally{
 const pool=[...new Set([...(core.PThread.unusedWorkers??[]),...Object.values(core.PThread.pthreads??{}).map(entry=>entry.worker)])];
 assert.equal(pool.length,4,'all owned pthread workers are observed');
 const exited=Promise.all(pool.map(worker=>new Promise(resolve=>worker.threadId===-1?resolve():worker.once('exit',resolve))));
 core.PThread.terminateAllThreads();await exited;
}
const log=logs.join('\n');assert.match(log,/PASS threaded collector:/);console.log(log);
await writeFile(reportFile,JSON.stringify({node:process.version,result:log,workerExitObserved:true},null,2)+'\n');
