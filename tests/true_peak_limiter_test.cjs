// Runs the true-peak limiter from src/WebViewManager.cpp outside the browser
// and checks its output with an independent true-peak meter (16x, 128-tap
// Blackman-windowed sinc).
const fs = require('node:fs');
const assert = require('node:assert/strict');
const source = fs.readFileSync('src/WebViewManager.cpp', 'utf8');
const start = source.indexOf('function ultraLightTruePeakLimiter() {');
const end = source.indexOf('const LIMITER_SOURCE');
assert(start > 0 && end > start, 'limiter source found');
const limiterSource = source.slice(start, end);
const RATE = 48000;
let Processor;
new Function('sampleRate', 'AudioWorkletProcessor', 'registerProcessor', '(' + limiterSource + ')()')(
  RATE, class { constructor() { this.port = {}; } }, (name, cls) => { assert.equal(name, 'ultralight-true-peak-limiter'); Processor = cls; });

const OPTIONS = { ceilingDb: -1, lookaheadMs: 3, releaseMs: 80 };
function run(channels, options = OPTIONS) {
  const lim = new Processor({ processorOptions: options });
  const out = channels.map((c) => new Float32Array(c.length));
  for (let i = 0; i < channels[0].length; i += 128) {
    lim.process([channels.map((c) => c.subarray(i, i + 128))], [out.map((c) => c.subarray(i, i + 128))]);
  }
  return { lim, out };
}
function truePeak(x) {
  let samplePeak = 0;
  for (const v of x) samplePeak = Math.max(samplePeak, Math.abs(v));
  let peak = samplePeak;
  const H = 64;
  for (let n = H; n < x.length - H; ++n) {
    if (Math.max(Math.abs(x[n]), Math.abs(x[n + 1])) < samplePeak * 0.5) continue;
    for (let p = 1; p < 16; ++p) {
      let s = 0;
      for (let k = -H + 1; k <= H; ++k) {
        const t = k - p / 16;
        const w = 0.42 + 0.5 * Math.cos(Math.PI * t / H) + 0.08 * Math.cos(2 * Math.PI * t / H);
        s += x[n + k] * Math.sin(Math.PI * t) / (Math.PI * t) * w;
      }
      peak = Math.max(peak, Math.abs(s));
    }
  }
  return peak;
}
const dB = (v) => 20 * Math.log10(v);
let seed = 7;
const noise = () => ((seed = (seed * 1103515245 + 12345) >>> 0) / 4294967296) * 2 - 1;
function signal(seconds, f) {
  const x = new Float32Array(Math.round(RATE * seconds));
  for (let i = 0; i < x.length; ++i) x[i] = f(i);
  return x;
}

let failures = 0;
function test(name, fn) {
  try { fn(); console.log('PASS:', name); } catch (e) { ++failures; console.error('FAIL:', name, e.message); }
}

test('a quiet signal passes untouched, only delayed', () => {
  const x = signal(0.2, (i) => 0.3 * Math.sin(2 * Math.PI * 440 * i / RATE));
  const { lim, out } = run([x]);
  const d = lim.latency;
  assert(d > 0 && d < 0.005 * RATE, `latency ${d} samples`);
  for (let i = d; i < x.length; ++i) assert.equal(out[0][i], x[i - d]);
});

const loud = {
  'sine at fs/4, 45 degrees (+3 dB between samples)': [() => 0, (i) => Math.sin(Math.PI / 2 * i + Math.PI / 4), -0.97],
  'music-like content boosted 300% with clicks': [() => 0, (i) => 3 * (0.3 * Math.sin(2 * Math.PI * 110 * i / RATE) + 0.2 * Math.sin(2 * Math.PI * 1234 * i / RATE) + 0.1 * noise() + (i % 9600 < 3 ? 0.9 : 0)), -0.9],
  'hard-clipped 11 kHz tone at +9 dB': [() => 0, (i) => 2 * Math.tanh(4 * Math.sin(2 * Math.PI * 11000 * i / RATE)), -0.97],
  'white noise boosted 300%': [() => 0, () => 1.5 * noise(), -0.9],
};
for (const [name, [, f, limit]] of Object.entries(loud)) {
  test(`${name} stays at or below -1 dBTP`, () => {
    const x = signal(0.5, f);
    const { out } = run([x, x.map((v) => -v)]);
    // Skip the first 50 ms: the signals start abruptly at full level.
    const tp = Math.max(truePeak(out[0].subarray(2400)), truePeak(out[1].subarray(2400)));
    assert(dB(tp) <= limit, `${dB(tp).toFixed(2)} dBTP`);
  });
}

test('channels share one gain, so the stereo image does not move', () => {
  const left = signal(0.2, (i) => 2 * Math.sin(2 * Math.PI * 300 * i / RATE));
  const right = left.map((v) => v * 0.25);
  const { out } = run([left, right]);
  for (let i = 0; i < left.length; ++i) assert(Math.abs(out[1][i] - out[0][i] * 0.25) < 1e-6);
});

test('gain recovers after a loud burst', () => {
  const x = signal(1, (i) => (i < RATE * 0.2 ? 3 : 0.3) * Math.sin(2 * Math.PI * 200 * i / RATE));
  const { out } = run([x]);
  const tail = out[0].subarray(RATE * 0.9);
  let p = 0;
  for (const v of tail) p = Math.max(p, Math.abs(v));
  assert(Math.abs(p - 0.3) < 0.003, `peak after release ${p}`);
});

if (failures) process.exitCode = 1;
