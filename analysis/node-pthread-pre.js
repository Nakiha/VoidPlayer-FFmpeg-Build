// Bun supplies both Web Worker and parentPort channels. Its pthreads must use parentPort.
if (typeof process === 'object' && process.versions?.bun) {
  const { workerData } = await import('node:worker_threads');
  if (workerData === 'em-pthread') globalThis.postMessage = undefined;
}
