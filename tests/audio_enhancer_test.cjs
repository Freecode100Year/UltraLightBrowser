const fs = require('node:fs');
const vm = require('node:vm');
const assert = require('node:assert/strict');
const source = fs.readFileSync('src/WebViewManager.cpp', 'utf8');
const section = source.slice(source.indexOf('void WebViewManager::InjectSurroundSoundScript'), source.indexOf('void WebViewManager::UpdateAudioEnhancer'));
function script(nativeOutput = false) {
  const values = { initEnabled:'true', initMode:'standard', effectiveDevice:'speakers', initVocalBoost:'false', initVolumeBoost:'1.000000', initMonoDownmix:'false', initNativeOutput:String(nativeOutput) };
  let js = '';
  for (const match of section.matchAll(/jsCode \+= (?:R"raw\(([\s\S]*?)\)raw"|([A-Za-z]+));/g)) {
    js += match[1] === undefined ? values[match[2]] : match[1];
  }
  return js;
}
function run(nativeOutput = false) {
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
  const media={currentSrc:'https://example.com/video',src:'',paused:false,ended:false,readyState:4,addEventListener(){},querySelectorAll(){return [];}};
  const window={AudioContext:Context,location:{href:'https://example.com/',origin:'https://example.com'},addEventListener(){}};
  const document={documentElement:{},querySelectorAll(){return [media];},addEventListener(){}};
  const sandbox={window,document,URL,Math,WeakMap,WeakSet,Set,Number,JSON,console,MutationObserver:class {observe(){}},setTimeout(fn){const id=++nextTimer;timers.set(id,fn);return id;},clearTimeout(id){timers.delete(id);}};
  vm.runInNewContext(script(nativeOutput),sandbox);
  const flush=()=>{for(const [id,fn] of [...timers]){timers.delete(id);fn();}};
  return {nodes,contexts,window,media,flush};
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
  assert(e.nodes.some(n=>n.type==='gain'&&n.gain.value===2));
});
if(failures)process.exitCode=1;
