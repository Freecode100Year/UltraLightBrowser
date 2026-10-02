// Adapted from XQL-MUSIC commit 2573ca3. Tests filter frequency response, not audible quality.
import fs from 'fs';

const FS = 48000;

function coeffs(type, f0, Q, gainDb) {
  const w0 = (2 * Math.PI * f0) / FS;
  const cw = Math.cos(w0), sw = Math.sin(w0);
  const A = Math.pow(10, gainDb / 40);
  let b0, b1, b2, a0, a1, a2;
  if (type === 'peaking') {
    const alpha = sw / (2 * Q);
    b0 = 1 + alpha * A; b1 = -2 * cw; b2 = 1 - alpha * A;
    a0 = 1 + alpha / A; a1 = -2 * cw; a2 = 1 - alpha / A;
  } else if (type === 'lowshelf') {
    const alpha = (sw / 2) * Math.SQRT2;      // Web Audio fixes S = 1
    const sq = 2 * Math.sqrt(A) * alpha;
    b0 = A * ((A + 1) - (A - 1) * cw + sq);
    b1 = 2 * A * ((A - 1) - (A + 1) * cw);
    b2 = A * ((A + 1) - (A - 1) * cw - sq);
    a0 = (A + 1) + (A - 1) * cw + sq;
    a1 = -2 * ((A - 1) + (A + 1) * cw);
    a2 = (A + 1) + (A - 1) * cw - sq;
  } else if (type === 'highshelf') {
    const alpha = (sw / 2) * Math.SQRT2;
    const sq = 2 * Math.sqrt(A) * alpha;
    b0 = A * ((A + 1) + (A - 1) * cw + sq);
    b1 = -2 * A * ((A - 1) + (A + 1) * cw);
    b2 = A * ((A + 1) + (A - 1) * cw - sq);
    a0 = (A + 1) - (A - 1) * cw + sq;
    a1 = 2 * ((A - 1) - (A + 1) * cw);
    a2 = (A + 1) - (A - 1) * cw - sq;
  } else if (type === 'lowpass') {
    const alpha = sw / (2 * Math.pow(10, Q / 20));
    b0 = (1 - cw) / 2; b1 = 1 - cw; b2 = (1 - cw) / 2;
    a0 = 1 + alpha; a1 = -2 * cw; a2 = 1 - alpha;
  } else if (type === 'highpass') {
    const alpha = sw / (2 * Math.pow(10, Q / 20));
    b0 = (1 + cw) / 2; b1 = -(1 + cw); b2 = (1 + cw) / 2;
    a0 = 1 + alpha; a1 = -2 * cw; a2 = 1 - alpha;
  } else throw new Error('unhandled filter type ' + type);
  return [b0 / a0, b1 / a0, b2 / a0, a1 / a0, a2 / a0];
}

// H(e^jw) of one biquad as [re, im].
function resp([b0, b1, b2, a1, a2], f) {
  const w = (2 * Math.PI * f) / FS;
  const c1 = Math.cos(w), s1 = -Math.sin(w);
  const c2 = Math.cos(2 * w), s2 = -Math.sin(2 * w);
  const nr = b0 + b1 * c1 + b2 * c2, ni = b1 * s1 + b2 * s2;
  const dr = 1 + a1 * c1 + a2 * c2, di = a1 * s1 + a2 * s2;
  const d = dr * dr + di * di;
  return [(nr * dr + ni * di) / d, (ni * dr - nr * di) / d];
}
const mul = (x, y) => [x[0] * y[0] - x[1] * y[1], x[0] * y[1] + x[1] * y[0]];
const add = (x, y) => [x[0] + y[0], x[1] + y[1]];
const sub = (x, y) => [x[0] - y[0], x[1] - y[1]];
const scale = (x, k) => [x[0] * k, x[1] * k];
const db = (x) => 20 * Math.log10(Math.max(Math.hypot(x[0], x[1]), 1e-12));

const FREQS = [];
for (let i = 0; i <= 900; i++) FREQS.push(20 * Math.pow(1000, i / 900)); // 20 Hz .. 20 kHz

const src = fs.readFileSync('src/WebViewManager.cpp', 'utf8');
const body = src.match(/const CROSSFEED_PARAMS = \{([\s\S]*?)\n    \};/)[1];
const butterworthQDb = +src.match(/BUTTERWORTH_Q_DB = (-?[\d.]+)/)[1];
let failures = 0;
let crossfeedCount = 0;
function check(ok, message) { console.log((ok ? 'PASS: ' : 'FAIL: ') + message); if (!ok) failures++; }
for (const m of body.matchAll(/(\w+): \{ cutoff: ([\d.]+), level: ([\d.]+), compFreq: ([\d.]+), compDb: (-?[\d.]+) \}/g)) {
    crossfeedCount++;
    const lp = coeffs('lowpass', +m[2], butterworthQDb, 0);
    const comp = coeffs('lowshelf', +m[4], 0.707, +m[5]);
    let low = 99, high = -99;
    for (const f of FREQS) {
        const result = db(mul(add([1,0], scale(resp(lp, f), +m[3])), resp(comp,f)));
        low = Math.min(low,result); high = Math.max(high,result);
    }
    check(high-low <= 1.2 && high <= 0.1, m[1] + ' crossfeed centre ripple ' + (high-low).toFixed(3) + ' dB');
}
check(crossfeedCount === 3, 'all three crossfeed presets are verified');
const hz = +src.match(/DEESS_CROSSOVER_HZ = (\d+)/)[1];
const lp = coeffs('lowpass',hz,butterworthQDb,0), hp = coeffs('highpass',hz,butterworthQDb,0);
let low=99,high=-99;
for(const f of FREQS){const v=db(add(mul(resp(lp,f),resp(lp,f)),mul(resp(hp,f),resp(hp,f))));low=Math.min(low,v);high=Math.max(high,v);}
check(src.includes('lowDelay.delayTime.value = 0.006;'),'low path matches compressor 6ms lookahead');
check(high-low<0.01,'de-esser idle LR4 sum ripple '+(high-low).toFixed(5)+' dB');
const boost=+src.match(/return \(cfg.vocalBoost[^\n]+\? ([\d.]+) : 0;/)[1];
const margin=+src.match(/EQ_HEADROOM_DB = ([\d.]+)/)[1];
const eqHz=+src.match(/vocal.frequency.value = ([\d.]+)/)[1];
const eqQ=+src.match(/vocal.Q.value = ([\d.]+)/)[1];
const eq=coeffs('peaking',eqHz,eqQ,boost);
let peak=-99;for(const f of FREQS)peak=Math.max(peak,db(resp(eq,f)));
check(peak-boost-margin<=-0.49,'vocal EQ with unity user gain: net peak '+(peak-boost-margin).toFixed(3)+' dB');
check(margin>0,'positive EQ safety margin');
if(failures)process.exitCode=1;
