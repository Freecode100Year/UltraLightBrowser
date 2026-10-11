const fs = require('node:fs');
const vm = require('node:vm');
const assert = require('node:assert/strict');
const source = fs.readFileSync('src/WebViewManager.cpp', 'utf8');
const section = source.slice(source.indexOf('void WebViewManager::InjectSurroundSoundScript'), source.indexOf('void WebViewManager::UpdateAudioEnhancer'));
function script(nativeOutput = false) {
  const values = { initEnabled:'true', initMode:'standard', effectiveDevice:'speakers', initVocalBoost:'false', initVolumeBoost:'1.000000', initMonoDownmix:'false', initNativeOutput:String(nativeOutput), initDeEsser:'false', initNightMode:'false' };
  let js = '';
  for (const match of section.matchAll(/jsCode \+= (?:R"raw\(([\s\S]*?)\)raw"|([A-Za-z]+));/g)) {
    js += match[1] === undefined ? values[match[2]] : match[1];
  }
  return js;
}
function run(nativeOutput = false, worklet = 'ok') {
  const nodes = [], contexts = [], timers = new Map(); let nextTimer = 0;
  const param = () => ({ value:1, setValueAtTime(v) { this.value=v; }, setTargetAtTime(v) { this.value=v; }, cancelScheduledValues() {}, linearRampToValueAtTime(v) {this.value=v;} });
  function node(type) {
    const n={type, outputs:[], connect(to, output=0, input=0) { this.outputs.push({to,output,input}); return to; }, disconnect() {this.outputs=[];}, gain:param(), frequency:param(), Q:param(), threshold:param(), knee:param(), ratio:param(), attack:param(), release:param(), delayTime:param(), positionX:param(), positionY:param(), positionZ:param()};
    nodes.push(n); return n;
  }
  class Context {
    constructor(){this.state='running';this.currentTime=0;this.sampleRate=48000;this.destination=node('destination');contexts.push(this);}
    createMediaElementSource(el){const n=node('source');n.element=el;return n;}
    createGain(){return node('gain');} createChannelSplitter(){return node('splitter');}
    createChannelMerger(){return node('merger');} createBiquadFilter(){return node('filter');}
    createDelay(){return node('delay');} createPanner(){return node('panner');}
    createConvolver(){return node('convolver');} createDynamicsCompressor(){return node('compressor');}
    createBuffer(ch,len,rate){return {sampleRate:rate,getChannelData(){return new Float32Array(len);}};}
    resume(){this.state='running';return Promise.resolve();} suspend(){this.state='suspended';return Promise.resolve();}
  }
  const modules=[];
  if (worklet!=='none') Context.prototype.audioWorklet={addModule(url){modules.push(url);return worklet==='ok'?Promise.resolve():Promise.reject(new Error('csp'));}};
  class AudioWorkletNode { constructor(ctx,name,options){const n=node('worklet');n.name=name;n.options=options;return n;} }
  const media={currentSrc:'https://example.com/video',src:'',paused:false,ended:false,readyState:4,addEventListener(){},querySelectorAll(){return [];}};
  const window={AudioContext:Context,AudioWorkletNode:worklet==='none'?undefined:AudioWorkletNode,Blob,location:{href:'https://example.com/',origin:'https://example.com'},addEventListener(){}};
  const document={documentElement:{},querySelectorAll(){return [media];},addEventListener(){}};
  const sandbox={window,document,URL,Blob,Math,WeakMap,WeakSet,Set,Number,JSON,console,MutationObserver:class {observe(){}},setTimeout(fn){const id=++nextTimer;timers.set(id,fn);return id;},clearTimeout(id){timers.delete(id);}};
  vm.runInNewContext(script(nativeOutput),sandbox);
  const flush=()=>{for(const [id,fn] of [...timers]){timers.delete(id);fn();}};
  return {nodes,contexts,window,media,flush,modules};
}
let failures=0;
function test(name,fn){try{fn();console.log('PASS:',name);}catch(e){++failures;console.error('FAIL:',name,e.message);}}
test('speaker enhanced output retains separate left/right merger inputs',()=>{
  const e=run(); const mergers=e.nodes.filter(n=>n.type==='merger');
  assert(mergers.some(m=>e.nodes.some(n=>n.outputs.some(o=>o.to===m&&o.input===0))&&e.nodes.some(n=>n.outputs.some(o=>o.to===m&&o.input===1))));
});
test('effects disabled routes original source around gain boost and compression',()=>{
  const e=run();e.window.__UltraLightSurround.updateConfig({enabled:false,volumeBoost:1,vocalBoost:false,monoDownmix:false});e.flush();
  const s=e.nodes.find(n=>n.type==='source'); const visited=new Set(); const walk=n=>{if(visited.has(n))return;visited.add(n);n.outputs.forEach(o=>walk(o.to));};walk(s);
  assert(![...visited].some(n=>n.type==='compressor'||n.type==='filter'||n.type==='splitter'));
});
test('native system output leaves media unclaimed and does not create AudioContext',()=>{
  const e=run(true);assert.equal(e.nodes.filter(n=>n.type==='source').length,0);assert.equal(e.contexts.length,0);
});
test('speaker standard mode does not allocate HRTF nodes',()=>{const e=run();assert.equal(e.nodes.filter(n=>n.type==='panner').length,0);});
test('native diagnostics do not initialize WebAudio',()=>{
  const e=run(true);const status=e.window.__UltraLightSurround.getStatus();
  assert.equal(status.output,'native');assert.equal(status.media[0].processed,false);assert.equal(e.contexts.length,0);
});
test('dialogue creates no HRTF and uses mild compression',()=>{
  const e=run();e.window.__UltraLightSurround.updateConfig({mode:'dialogue',device:'headphones'});e.flush();
  const s=e.nodes.find(n=>n.type==='source');const visited=new Set();const walk=n=>{if(visited.has(n))return;visited.add(n);n.outputs.forEach(o=>walk(o.to));};walk(s);
  assert(![...visited].some(n=>n.type==='panner'||n.type==='convolver'));
  assert([...visited].some(n=>n.type==='compressor'&&n.ratio.value===3&&n.threshold.value===-18));
});
test('volume and EQ updates retain media source without rebuilding it',()=>{
  const e=run();e.window.__UltraLightSurround.updateConfig({volumeBoost:2,vocalBoost:true});e.flush();
  assert.equal(e.nodes.filter(n=>n.type==='source').length,1);
  assert(e.nodes.some(n=>n.type==='gain'&&Math.abs(n.gain.value-2*Math.pow(10,-5/20))<1e-8));
});
test('vocal EQ automatically reserves headroom',()=>{
  const e=run();e.window.__UltraLightSurround.updateConfig({vocalBoost:true});e.flush();
  const source=e.nodes.find(n=>n.type==='source');
  assert(source.outputs[0].to.gain.value<0.60);
});
test('de-esser uses LR4 split and high-band compression',()=>{
  const e=run();e.window.__UltraLightSurround.updateConfig({deEsser:true});e.flush();
  assert(e.nodes.some(n=>n.type==='delay'&&n.delayTime.value===0.006));
  assert.equal(e.nodes.filter(n=>n.type==='lowpass'&&n.frequency.value===5500).length,2);
  assert.equal(e.nodes.filter(n=>n.type==='highpass'&&n.frequency.value===5500).length,2);
  assert(e.nodes.filter(n=>(n.type==='highpass'||n.type==='lowpass')&&n.frequency.value===5500).every(n=>Math.abs(n.Q.value+3.01029995664)<1e-9));
  assert(e.nodes.some(n=>n.type==='compressor'&&n.threshold.value===-32&&n.ratio.value===4));
});
test('night mode creates separate gentle compression',()=>{
  const e=run();e.window.__UltraLightSurround.updateConfig({nightMode:true});e.flush();
  assert(e.nodes.some(n=>n.type==='compressor'&&n.ratio.value===3&&n.release.value===0.24));
});
const reach=(e)=>{const s=e.nodes.filter(n=>n.type==='source').pop();const v=new Set();const walk=n=>{if(v.has(n))return;v.add(n);n.outputs.forEach(o=>walk(o.to));};walk(s);return [...v];};
const settle=()=>new Promise(r=>setImmediate(r));
async function asyncTest(name,fn){try{await fn();console.log('PASS:',name);}catch(e){++failures;console.error('FAIL:',name,e.message);}}
(async()=>{
await asyncTest('true-peak limiter replaces the protection compressor once the worklet loads',async()=>{
  const e=run(); await settle(); e.flush();
  assert.equal(e.modules.length,1); assert.match(e.modules[0],/^blob:/);
  const v=reach(e); const lim=v.find(n=>n.type==='worklet');
  assert(lim,'limiter in the chain'); assert.equal(lim.name,'ultralight-true-peak-limiter');
  assert.equal(lim.options.processorOptions.ceilingDb,-1);
  assert(lim.outputs.some(o=>o.to.type==='gain'),'limiter feeds the output gain');
  assert(!v.some(n=>n.type==='compressor'&&n.ratio.value===12),'no fallback compressor');
  assert.equal(e.window.__UltraLightSurround.getStatus().limiter,'true-peak');
});
await asyncTest('dialogue keeps its leveller ahead of the limiter',async()=>{
  const e=run(); await settle(); e.window.__UltraLightSurround.updateConfig({mode:'dialogue'}); e.flush();
  const v=reach(e); const lev=v.find(n=>n.type==='compressor'&&n.threshold.value===-18&&n.ratio.value===3);
  assert(lev); assert(lev.outputs.some(o=>o.to.type==='worklet'));
});
await asyncTest('a page that refuses the worklet keeps the compressor fallback',async()=>{
  const e=run(false,'blocked'); await settle(); e.flush();
  const v=reach(e); assert(!v.some(n=>n.type==='worklet'));
  assert(v.some(n=>n.type==='compressor'&&n.ratio.value===12&&n.threshold.value===-3));
  assert.equal(e.window.__UltraLightSurround.getStatus().limiter,'unavailable');
});
await asyncTest('no AudioWorklet support keeps the compressor fallback',async()=>{
  const e=run(false,'none'); await settle(); e.flush();
  assert(!reach(e).some(n=>n.type==='worklet')); assert.equal(e.modules.length,0);
});
await asyncTest('limiter is not used when no processing is needed',async()=>{
  const e=run(); await settle(); e.window.__UltraLightSurround.updateConfig({enabled:false,volumeBoost:1,vocalBoost:false,monoDownmix:false}); e.flush();
  assert(!reach(e).some(n=>n.type==='worklet'));
});
if(failures)process.exitCode=1;
})();
