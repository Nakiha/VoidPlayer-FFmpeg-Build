import assert from 'node:assert/strict';
import {readFile} from 'node:fs/promises';
import {pathToFileURL} from 'node:url';
import path from 'node:path';
export async function createAnalysisTestInstance(moduleFile,settings={}) {
 const file=path.resolve(moduleFile),factory=(await import(pathToFileURL(file))).default;
 const threads=Number(settings.threads??process.env.ANALYSIS_TEST_THREADS??1),contexts=Number(settings.contexts??process.env.ANALYSIS_TEST_CONTEXTS??threads);
 assert.ok(Number.isInteger(threads)&&threads>=1&&threads<=4&&contexts>=1&&contexts<=threads);
 const wasmBinary=await readFile(file.replace(/\.js$/,'.wasm')),compiled=await WebAssembly.compile(wasmBinary);
 let memory;
 const core=await factory({wasmBinary,analysisThreads:threads>1?threads:0,instantiateWasm(imports,receive){
  memory=Object.values(imports).flatMap(value=>Object.values(value)).find(value=>value instanceof WebAssembly.Memory);
  const instance=new WebAssembly.Instance(compiled,imports);receive(instance,compiled);return instance.exports;
 }});
 if(memory){let heap=core.HEAPU8;Object.defineProperty(core,'HEAPU8',{get(){if(heap.buffer!==memory.buffer||heap.byteLength!==memory.buffer.byteLength)heap=new Uint8Array(memory.buffer);return heap;},set(value){heap=value;}});}
 if(core._vpa_configure_threads)assert.equal(core._vpa_configure_threads(threads,contexts),0);
 else assert.equal(threads,1,'requested concurrency requires the threaded artifact');
 return core;
}
export async function closeAnalysisTestInstance(core) {
 core._vpa_close();
 if(!core.PThread)return;
 const workers=[...new Set([...(core.PThread.unusedWorkers??[]),...Object.values(core.PThread.pthreads??{}).map(value=>value.worker)])];
 const stopped=Promise.all(workers.map(worker=>new Promise(resolve=>worker.threadId===-1?resolve():worker.once('exit',resolve))));
 core.PThread.terminateAllThreads();await stopped;
}
