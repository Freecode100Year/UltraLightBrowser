// Checks the synthesized small-room impulse response used by the cinema preset
// (getSmallRoomBuffer in src/WebViewManager.cpp).
//   node tests/room_impulse_test.cjs
'use strict';
const assert = require('node:assert/strict');
const fs = require('node:fs');
const source = fs.readFileSync('src/WebViewManager.cpp', 'utf8');
const start = source.indexOf('function getSmallRoomBuffer(ctx) {');
const end = source.indexOf('// 4. Safe URL & CORS Check');
assert(start > 0 && end > start, 'getSmallRoomBuffer found');
const body = source.slice(start, end);

function build(rate) {
  const make = new Function('cachedReverbBuffer', body + '; return getSmallRoomBuffer;');
  const ctx = {
    sampleRate: rate,
    createBuffer(ch, n, r) {
      const data = Array.from({ length: ch }, () => new Float32Array(n));
      return { sampleRate: r, length: n, getChannelData: (i) => data[i] };
    },
  };
  return make(null)(ctx);
}
const energy = (x) => x.reduce((s, v) => s + v * v, 0);
// Share of energy above 8 kHz, from a plain DFT over the IR.
function highShare(x, rate) {
  let hi = 0, all = 0;
  for (let k = 1; k < x.length / 2; ++k) {
    let re = 0, im = 0;
    for (let n = 0; n < x.length; ++n) { const a = 2 * Math.PI * k * n / x.length; re += x[n] * Math.cos(a); im -= x[n] * Math.sin(a); }
    const p = re * re + im * im;
    all += p;
    if (k * rate / x.length > 8000) hi += p;
  }
  return hi / all;
}

for (const rate of [44100, 48000]) {
  const a = build(rate), b = build(rate);
  const L = a.getChannelData(0), R = a.getChannelData(1);
  assert.equal(a.length, Math.floor(rate * 0.035));
  // Deterministic: the same room every time.
  assert.deepEqual(Array.from(L), Array.from(b.getChannelData(0)));
  // Short pre-delay keeps the direct sound clean.
  for (let i = 0; i < Math.round(rate * 0.0015); ++i) assert.equal(L[i], 0);
  // Same level as the former white-noise room: sum of (0.5 env)^2 / 3.
  let target = 0;
  for (let i = 0; i < a.length - Math.round(rate * 0.0015); ++i) target += Math.pow(0.5 * Math.exp(-(i / rate) / 0.009), 2) / 3;
  assert(Math.abs(energy(L) / target - 1) < 1e-3, `left energy ${energy(L)} vs ${target}`);
  assert(Math.abs(energy(R) / target - 1) < 1e-3, 'right energy');
  // Left and right are decorrelated.
  let dot = 0; for (let i = 0; i < L.length; ++i) dot += L[i] * R[i];
  assert(Math.abs(dot / Math.sqrt(energy(L) * energy(R))) < 0.2, 'L/R correlation');
  // Highs are damped: white noise would put about two thirds of its energy above 8 kHz.
  const share = highShare(Array.from(L), rate);
  assert(share < 0.15, `energy above 8 kHz ${share.toFixed(3)}`);
  console.log(`PASS: room IR at ${rate} Hz (above 8 kHz: ${(share * 100).toFixed(1)}%)`);
}
