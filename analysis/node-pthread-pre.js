import { workerData } from 'node:worker_threads';

// Use Emscripten's parentPort bridge for Node-compatible workers. Bun also
// supplies a Web Worker postMessage, whose channel differs from parentPort.
if (process.versions.bun && workerData === 'em-pthread') globalThis.postMessage = undefined;
