import assert from 'node:assert/strict';
import {readFile,writeFile} from 'node:fs/promises';
import {createHash} from 'node:crypto';
import {execFileSync} from 'node:child_process';
import path from 'node:path';
import {pathToFileURL} from 'node:url';
const [directory,product,stem,demuxers='',decoders='']=process.argv.slice(2);
assert.ok(directory&&['playback','analysis'].includes(product)&&/^voidplayer-(core(?:-mt)?|analysis)$/.test(stem));
const wasm=await readFile(path.join(directory,stem+'.wasm'));
const create=(await import(pathToFileURL(path.resolve(directory,stem+'.js')).href)).default;
const core=await create({wasmBinary:wasm});
assert.equal(core._vp_capability_abi(),1);
const string=pointer=>{assert.ok(pointer>0&&pointer<core.HEAPU8.length);const end=core.HEAPU8.indexOf(0,pointer);assert.ok(end>=pointer&&end-pointer<=1024);return new TextDecoder().decode(core.HEAPU8.subarray(pointer,end));};
const registry=kind=>{const n=core[`_vp_${kind}_count`]();assert.ok(Number.isInteger(n)&&n>=0&&n<=1024);return Array.from({length:n},(_,i)=>string(core[`_vp_${kind}_name`](i))).sort();};
const actualDemuxers=registry('demuxer'),actualDecoders=registry('decoder');
const declaredDemuxers=demuxers.split(/\s+/).filter(Boolean).sort(),declaredDecoders=decoders.split(/\s+/).filter(Boolean).sort();
const configureName=name=>{const first=name.split(',')[0];return first==='mpeg'?'mpegps':first;};
assert.deepEqual(actualDemuxers.map(configureName).sort(),declaredDemuxers,'actual linked demux registry differs from configure declaration');
// FFmpeg's pinned configure selects h263_decoder for mpeg4_decoder. Keep
// that dependency explicit rather than misreporting the linked registry.
const decoderDependencies=declaredDecoders.includes('mpeg4')?['h263']:[];
assert.deepEqual(actualDecoders,[...new Set([...declaredDecoders,...decoderDependencies])].sort(),'actual linked codec registry differs from configure declaration and dependencies');
const independentDemuxExports=Object.keys(core).filter(name=>name.startsWith('_vpd_')).sort();
assert.deepEqual(independentDemuxExports,[],'independent container demux ABI must be removed');
const containerEntries=['_vp_open','_vp_open_blob','_vp_open_range'].filter(name=>typeof core[name]==='function');
if(product==='analysis')assert.deepEqual(containerEntries,[]);
const root=path.resolve(import.meta.dirname,'..'),revision=execFileSync('git',['-C',root,'rev-parse','HEAD'],{encoding:'utf8'}).trim();
assert.equal(string(core._vp_build_id()),revision,'actual WASM build identity differs from source');
const packetIdentityAbi=product==='playback'?core._vp_packet_identity_abi():0;
if(product==='playback')assert.equal(packetIdentityAbi,1);
const capability={packetIdentityAbi,schema:1,abi:1,product,variant:stem.endsWith('-mt')?'pthreads':'single',revision,wasmSha256:createHash('sha256').update(wasm).digest('hex'),declaredDemuxers,registeredDemuxers:actualDemuxers,declaredDecoders,decoderDependencies,registeredDecoders:actualDecoders,libraries:product==='analysis'?['avcodec','avutil']:['avformat','avcodec','swscale','avutil','dav1d'],containerEntries,independentDemuxExports};
const file=path.join(directory,stem.endsWith('-mt')?'capabilities-mt.json':'capabilities.json');
await writeFile(file,JSON.stringify(capability,null,2)+'\n');console.log(`Verified ${product} registry: ${actualDemuxers.join(' ')||'(no container demux)'}`);
// The capability probe owns this isolated Node process and any pthread pool.
process.exit(0);
