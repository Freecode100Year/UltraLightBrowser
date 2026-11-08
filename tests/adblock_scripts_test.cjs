// Executes the actual page scripts embedded in src/AdBlockRules.cpp (cosmetic
// filtering, YouTube scriptlet and script surrogates) against small DOM stand-ins.
//   node tests/adblock_scripts_test.cjs
'use strict';
const assert = require('assert');
const fs = require('fs');
const path = require('path');
const vm = require('vm');

const source = fs.readFileSync(path.join(__dirname, '..', 'src', 'AdBlockRules.cpp'), 'utf8');
function embedded(name) {
  const re = new RegExp('const char ' + name + '\\[\\] = R"ULBJS\\(([\\s\\S]*?)\\)ULBJS";');
  const m = source.match(re);
  assert(m, 'missing embedded script ' + name);
  return m[1];
}

function cosmetic(cfg) {
  return embedded('kCosmeticScript').replace('__ULB_CONFIG__', JSON.stringify(cfg));
}

function makeEnv({ host, topHost, ancestor } = {}) {
  const appended = [];
  const intervals = [];
  const listeners = {};
  const elements = {};
  const documentElement = { appendChild(n) { appended.push(n); } };
  const document = {
    head: { appendChild(n) { appended.push(n); } },
    documentElement,
    createElement(tag) { return { tag, id: '', textContent: '' }; },
    addEventListener(type, fn) { listeners[type] = fn; },
    querySelector(sel) { return elements[sel] || null; },
  };
  class Response { constructor(body) { this.body = body; } json() { return Promise.resolve(JSON.parse(this.body)); } }
  const win = {
    location: { hostname: host, ancestorOrigins: ancestor ? [ancestor] : [] },
    document,
    JSON: { parse: JSON.parse, stringify: JSON.stringify },
    Response,
    URL,
    Promise,
    Object,
    Array,
    Date,
    isFinite,
    setInterval(fn, ms) { intervals.push({ fn, ms }); return intervals.length; },
    setTimeout(fn) { fn(); return 1; },
  };
  win.window = win;
  if (topHost && topHost !== host) {
    win.top = { get location() { throw new Error('cross-origin'); } };
  } else {
    win.top = win;
  }
  Response.prototype.constructor = Response;
  const ctx = vm.createContext(win);
  return { ctx, win, appended, intervals, listeners, elements };
}

function run(env, code) { vm.runInContext(code, env.ctx); }
function css(env) { return env.appended.map((n) => n.textContent).join('\n'); }

// --- cosmetic filtering on / off / per-site exception ------------------------
{
  const env = makeEnv({ host: 'news.example.com' });
  run(env, cosmetic({ enabled: true, off: [] }));
  assert.match(css(env), /^ins\.adsbygoogle\{display:none!important\}$/m);
  assert.match(css(env), /\[id\^="div-gpt-ad"\]\{display:none!important\}/);
  assert(!/ytd-ad-slot-renderer/.test(css(env)), 'YouTube rules must stay on YouTube');
  // Running twice in one document (re-registration) must not duplicate.
  run(env, cosmetic({ enabled: true, off: [] }));
  assert.strictEqual(env.appended.length, 1);
}
{
  const env = makeEnv({ host: 'news.example.com' });
  run(env, cosmetic({ enabled: false, off: [] }));
  assert.strictEqual(env.appended.length, 0);
}
{
  const env = makeEnv({ host: 'news.example.com' });
  run(env, cosmetic({ enabled: true, off: ['news.example.com'] }));
  assert.strictEqual(env.appended.length, 0);
}
{
  // Cross-origin ad iframe inside an exempted site: decided by the top-level site.
  const env = makeEnv({ host: 'frame.adhost.net', topHost: 'news.example.com', ancestor: 'https://news.example.com' });
  run(env, cosmetic({ enabled: true, off: ['news.example.com'] }));
  assert.strictEqual(env.appended.length, 0);
  const env2 = makeEnv({ host: 'frame.adhost.net', topHost: 'other.example.com', ancestor: 'https://other.example.com' });
  run(env2, cosmetic({ enabled: true, off: ['news.example.com'] }));
  assert.strictEqual(env2.appended.length, 1);
}
{
  const env = makeEnv({ host: 'ulb.internal' });
  run(env, cosmetic({ enabled: true, off: [] }));
  assert.strictEqual(env.appended.length, 0, 'built-in pages are left alone');
}
{
  const env = makeEnv({ host: 'www.zhihu.com' });
  run(env, cosmetic({ enabled: true, off: [] }));
  assert.match(css(env), /\.TopstoryItem--advertCard\{/);
}

// --- YouTube scriptlet --------------------------------------------------------
(async () => {
  const env = makeEnv({ host: 'www.youtube.com' });
  run(env, cosmetic({ enabled: true, off: [] }));
  assert.match(css(env), /ytd-ad-slot-renderer\{display:none!important\}/);

  const parsed = vm.runInContext('JSON.parse(\'{"videoDetails":{"videoId":"x"},"adPlacements":[1],"playerAds":[2],"adSlots":[3]}\')', env.ctx);
  assert.deepStrictEqual(Object.keys(parsed), ['videoDetails']);
  const nested = vm.runInContext('JSON.parse(\'[{"playerResponse":{"adPlacements":[1],"streamingData":{}}}]\')', env.ctx);
  assert.deepStrictEqual(Object.keys(nested[0].playerResponse), ['streamingData']);
  assert.strictEqual(vm.runInContext('JSON.parse("5")', env.ctx), 5);
  assert.strictEqual(vm.runInContext('JSON.parse.name', env.ctx), 'parse');

  run(env, 'var ytInitialPlayerResponse = {adPlacements:[1], streamingData:{a:1}};');
  assert.deepStrictEqual(Object.keys(env.win.ytInitialPlayerResponse), ['streamingData']);

  const viaFetch = await vm.runInContext('new Response(\'{"playerAds":[1],"ok":true}\').json()', env.ctx);
  assert.deepStrictEqual(Object.keys(viaFetch), ['ok']);

  // Skip fallback ends a playing ad and presses "Skip".
  assert.strictEqual(env.intervals.length, 1);
  let clicked = false;
  const video = { duration: 15, currentTime: 2 };
  env.elements['.html5-video-player.ad-showing'] = {
    querySelector(sel) {
      if (sel.startsWith('video')) return video;
      if (sel.includes('ytp-skip-ad-button')) return { click() { clicked = true; } };
      return null;
    },
  };
  env.intervals[0].fn();
  assert.strictEqual(video.currentTime, 15);
  assert(clicked);
  delete env.elements['.html5-video-player.ad-showing'];
  video.currentTime = 3;
  env.intervals[0].fn();
  assert.strictEqual(video.currentTime, 3, 'regular playback is untouched');

  // Embedded players in frames get pruning but no polling timer.
  const frame = makeEnv({ host: 'www.youtube.com', topHost: 'blog.example.com', ancestor: 'https://blog.example.com' });
  run(frame, cosmetic({ enabled: true, off: [] }));
  assert.strictEqual(frame.intervals.length, 0);

  // --- surrogates -------------------------------------------------------------
  {
    const s = makeEnv({ host: 'shop.example.com' });
    run(s, 'var hits=0; window.ga=function(){(ga.q=ga.q||[]).push(arguments)}; ga("create","UA-1","auto"); ga("send","event",{hitCallback:function(){hits++}});');
    run(s, embedded('kSurrogateAnalytics'));
    run(s, 'ga("send","pageview",{hitCallback:function(){hits++}}); ga(function(t){ if(t && t.get) hits++; });');
    assert.strictEqual(s.win.hits, 3);
    assert.strictEqual(typeof s.win.ga.getByName('t0').send, 'function');
  }
  {
    const s = makeEnv({ host: 'shop.example.com' });
    run(s, 'var hits=0; window.dataLayer=[{eventCallback:function(){hits++}}]; function gtag(){dataLayer.push(arguments)}');
    run(s, embedded('kSurrogateGtag'));
    run(s, 'gtag("event","purchase",{event_callback:function(){hits++}}); dataLayer.push({event:"x",eventCallback:function(){hits++}});');
    assert.strictEqual(s.win.hits, 3);
  }
  {
    const s = makeEnv({ host: 'news.example.com' });
    run(s, 'var ran=0; window.googletag={cmd:[function(){ran++}]};');
    run(s, embedded('kSurrogateGpt'));
    run(s, `googletag.cmd.push(function(){
      var slot = googletag.defineSlot('/1/a',[300,250],'div-1').addService(googletag.pubads()).setTargeting('k','v');
      googletag.pubads().enableSingleRequest().collapseEmptyDivs();
      googletag.enableServices(); googletag.display('div-1');
      if (slot.getSlotElementId()==='div-1' && googletag.sizeMapping().addSize([0,0],[]).build().length===0) ran++;
    });`);
    assert.strictEqual(s.win.ran, 2);
  }
  {
    const s = makeEnv({ host: 'news.example.com' });
    run(s, embedded('kSurrogateAdsByGoogle'));
    run(s, '(adsbygoogle = window.adsbygoogle || []).push({});');
    assert.strictEqual(s.win.adsbygoogle.loaded, true);
  }
  console.log('adblock scripts: all checks passed');
})().catch((e) => { console.error(e); process.exit(1); });
