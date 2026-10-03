# Loads a fixed set of sites twice (fresh profile, then after a restart) and prints
# navigation-to-load times measured over the DevTools protocol.
param([string]$Exe, [string]$Label, [switch]$Mac)
$ErrorActionPreference = 'Continue'
Add-Type @"
using System; using System.Runtime.InteropServices;
public static class P {
  [DllImport("user32.dll")] public static extern IntPtr FindWindow(string c, string t);
  [DllImport("user32.dll")] public static extern bool PostMessage(IntPtr h, uint m, IntPtr w, IntPtr l);
}
"@
$env:WEBVIEW2_ADDITIONAL_BROWSER_ARGUMENTS = '--remote-debugging-port=9223'
if (-not (Test-Path "$env:RUNNER_TEMP\node_modules\ws")) { npm i --silent --prefix $env:RUNNER_TEMP ws | Out-Null }
$env:NODE_PATH = Join-Path $env:RUNNER_TEMP 'node_modules'
@'
const WebSocket = require('ws');
const sites = ['https://www.wikipedia.org/', 'https://github.com/', 'https://www.bbc.com/news', 'https://www.theguardian.com/international', 'https://www.reddit.com/', 'https://stackoverflow.com/questions'];
(async () => {
  // Pick the visible tab: v2 keeps a hidden pre-rendered start page as well.
  const visible = async (t) => {
    const ws = new WebSocket(t.webSocketDebuggerUrl);
    await new Promise(r => ws.on('open', r));
    const v = await new Promise(r => { ws.on('message', m => { const d = JSON.parse(m); if (d.id === 1) r(d.result && d.result.result && d.result.result.value); });
      ws.send(JSON.stringify({id: 1, method: 'Runtime.evaluate', params: {expression: 'document.visibilityState', returnByValue: true}})); setTimeout(() => r(null), 2000); });
    ws.close();
    return v === 'visible';
  };
  let target;
  for (let i = 0; i < 40 && !target; i++) {
    try {
      const pages = (await (await fetch('http://127.0.0.1:9223/json/list')).json()).filter(t => t.type === 'page' && !t.url.includes('ulb-probe'));
      for (const p of pages) { if (await visible(p)) { target = p; break; } }
    } catch {}
    if (!target) await new Promise(r => setTimeout(r, 500));
  }
  const ws = new WebSocket(target.webSocketDebuggerUrl);
  await new Promise(r => ws.on('open', r));
  let id = 0; const pend = {}; const waiters = [];
  ws.on('message', m => { const d = JSON.parse(m); if (d.id && pend[d.id]) { pend[d.id](d); delete pend[d.id]; } if (d.method === 'Page.loadEventFired') waiters.splice(0).forEach(f => f()); });
  const send = (method, params = {}) => new Promise(r => { const i = ++id; pend[i] = r; ws.send(JSON.stringify({id: i, method, params})); });
  await send('Page.enable');
  const results = [], frames = [], slows = [];
  for (const url of sites) {
    const loaded = new Promise(r => { waiters.push(r); setTimeout(r, 30000); });
    const t0 = Date.now();
    await send('Page.navigate', {url});
    await loaded;
    const ms = Date.now() - t0;
    const r = await send('Runtime.evaluate', {returnByValue: true, expression: 'JSON.stringify((()=>{const n=performance.getEntriesByType("navigation")[0]||{};const fcp=performance.getEntriesByName("first-contentful-paint")[0];return {dcl:Math.round(n.domContentLoadedEventEnd||0),fcp:fcp?Math.round(fcp.startTime):0,res:performance.getEntriesByType("resource").length}})())'});
    const v = JSON.parse(r.result.result.value || '{}');
    results.push(ms);
    // Rendering: scroll for 3 s right after load (when the browser's own post-load work
    // runs too) and count frames and frames slower than 34 ms.
    const sc = await send('Runtime.evaluate', {returnByValue: true, awaitPromise: true, expression:
      'new Promise(res => { let n = 0, slow = 0, start = 0, last = 0; const f = (t) => { if (!start) start = last = t; else { n++; if (t - last > 34) slow++; last = t; } window.scrollBy(0, 40); if (t - start < 3000) requestAnimationFrame(f); else res(JSON.stringify({fps: Math.round(n * 1000 / (t - start)), slow})); }; requestAnimationFrame(f); setTimeout(() => res("{}"), 6000); })'});
    const s = JSON.parse((sc.result && sc.result.result && sc.result.result.value) || '{}');
    frames.push(s.fps || 0); slows.push(s.slow || 0);
    console.log(`  ${url.padEnd(46)} load ${String(ms).padStart(6)} ms  fcp ${String(v.fcp).padStart(5)}  dcl ${String(v.dcl).padStart(5)}  resources ${String(v.res).padStart(4)}  scroll ${s.fps} fps, ${s.slow} slow frames`);
  }
  console.log('  TOTAL', results.reduce((a, b) => a + b, 0), 'ms', ' FPS-AVG', Math.round(frames.reduce((a, b) => a + b, 0) / frames.length), ' SLOW', slows.reduce((a, b) => a + b, 0));
  process.exit(0);
})().catch(e => { console.log('ERR', e.message); process.exit(0); });
'@ | Set-Content "$env:RUNNER_TEMP\perf.js"
Remove-Item -Recurse -Force "$env:LOCALAPPDATA\UltraLightBrowser" -ErrorAction SilentlyContinue
if ($Mac) {
  New-Item -ItemType Directory -Force "$env:LOCALAPPDATA\UltraLightBrowser" | Out-Null
  '{"settings":{"userAgentProfile":"macos-edge"}}' | Set-Content -Encoding utf8NoBOM "$env:LOCALAPPDATA\UltraLightBrowser\config.json"
}
foreach ($run in @('first visit', 'after restart')) {
  "== $Label / $run"
  $p = Start-Process $Exe -PassThru
  Start-Sleep 6
  node "$env:RUNNER_TEMP\perf.js"
  [P]::PostMessage([P]::FindWindow("UltraLightBrowserMainWindow", [NullString]::Value), 0x10, [IntPtr]::Zero, [IntPtr]::Zero) | Out-Null
  $p.WaitForExit(15000) | Out-Null
  if (-not $p.HasExited) { Stop-Process $p -Force }
  Start-Sleep 2
}
