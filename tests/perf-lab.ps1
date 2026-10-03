# Experiments for "first screen first": Chromium feature flags, preconnect and
# speculation-rules prefetch. Each run starts the browser fresh (empty cache).
param([string]$Exe)
$ErrorActionPreference = 'Continue'
Add-Type @"
using System; using System.Runtime.InteropServices;
public static class P2 {
  [DllImport("user32.dll")] public static extern IntPtr FindWindow(string c, string t);
  [DllImport("user32.dll")] public static extern bool PostMessage(IntPtr h, uint m, IntPtr w, IntPtr l);
}
"@
if (-not (Test-Path "$env:RUNNER_TEMP\node_modules\ws")) { npm i --silent --prefix $env:RUNNER_TEMP ws | Out-Null }
$env:NODE_PATH = Join-Path $env:RUNNER_TEMP 'node_modules'
@'
const WebSocket = require('ws');
const mode = process.env.LAB_MODE;
const sites = ['https://www.wikipedia.org/', 'https://github.com/', 'https://www.bbc.com/news', 'https://www.theguardian.com/international',
  'https://edition.cnn.com/', 'https://www.yahoo.com/', 'https://www.msn.com/', 'https://www.theverge.com/'];
(async () => {
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
  const send = (method, params = {}) => new Promise(r => { const i = ++id; pend[i] = r; ws.send(JSON.stringify({id: i, method, params})); setTimeout(() => r({}), 20000); });
  const ev = async (expression) => { const r = await send('Runtime.evaluate', {expression, returnByValue: true, awaitPromise: true}); return r.result && r.result.result && r.result.result.value; };
  const sleep = (ms) => new Promise(r => setTimeout(r, ms));
  const loaded = () => new Promise(r => { waiters.push(r); setTimeout(r, 30000); });
  await send('Page.enable');
  const rows = [];
  for (const url of sites) {
    if (mode !== 'direct') {
      const l = loaded(); await send('Page.navigate', {url: 'https://example.com/'}); await l; await sleep(300);
      if (mode === 'preconnect') {
        const origin = new URL(url).origin;
        await ev(`for (const rel of ['dns-prefetch', 'preconnect']) { const l = document.createElement('link'); l.rel = rel; l.href = ${JSON.stringify(origin)}; document.head.append(l); } true`);
        await sleep(1500);
      } else if (mode === 'prefetch') {
        await ev(`(() => { const s = document.createElement('script'); s.type = 'speculationrules'; s.textContent = JSON.stringify({prefetch: [{source: 'list', urls: [${JSON.stringify(url)}]}]}); document.head.append(s); return true; })()`);
        await sleep(2500);
      }
    }
    const l = loaded();
    const t0 = Date.now();
    if (mode === 'direct') await send('Page.navigate', {url}); else await ev(`location.href = ${JSON.stringify(url)}; true`);
    await l;
    const load = Date.now() - t0;
    await sleep(1000);
    const v = JSON.parse(await ev(`new Promise(res => { let lcp = 0; try { new PerformanceObserver(l => { for (const e of l.getEntries()) lcp = e.startTime; }).observe({type: 'largest-contentful-paint', buffered: true}); } catch {}
      setTimeout(() => { const n = performance.getEntriesByType('navigation')[0] || {}; const fcp = performance.getEntriesByName('first-contentful-paint')[0];
        res(JSON.stringify({lcp: Math.round(lcp), fcp: fcp ? Math.round(fcp.startTime) : 0, ttfb: Math.round(n.responseStart || 0), dns: Math.round((n.domainLookupEnd || 0) - (n.domainLookupStart || 0)),
          conn: Math.round((n.connectEnd || 0) - (n.connectStart || 0)), type: n.deliveryType || '', res: performance.getEntriesByType('resource').length})); }, 200); })`) || '{}');
    rows.push({load, ...v});
    console.log(`  ${url.padEnd(44)} load ${String(load).padStart(6)}  ttfb ${String(v.ttfb).padStart(5)}  dns ${String(v.dns).padStart(4)}  conn ${String(v.conn).padStart(4)}  fcp ${String(v.fcp).padStart(5)}  lcp ${String(v.lcp).padStart(5)}  res ${String(v.res).padStart(4)} ${v.type}`);
  }
  const sum = (k) => rows.reduce((a, r) => a + (r[k] || 0), 0);
  console.log(`  SUM load ${sum('load')}  ttfb ${sum('ttfb')}  fcp ${sum('fcp')}  lcp ${sum('lcp')}`);
  process.exit(0);
})().catch(e => { console.log('ERR', e.message); process.exit(0); });
'@ | Set-Content "$env:RUNNER_TEMP\lab.js"

$base = 'DnsOverHttps,HighEfficiencyModeAvailable,PageDiscarding,Freezer,BatterySaverModeAvailable'
$lcpFirst = $base + ',DelayAsyncScriptExecution:delay_async_exec_delay_type/till_first_lcp_candidate/cross_site_only/true/delay_async_exec_delay_limit/2s,ThrottleUnimportantFrameTimers,SpeculativeImageDecodes,ThreadedPreloadScanner'
$runs = @(
  @{Name = 'direct';            Mode = 'direct';     Features = $base},
  @{Name = 'direct+lcp-first';  Mode = 'direct';     Features = $lcpFirst},
  @{Name = 'from-page';         Mode = 'from-page';  Features = $base},
  @{Name = 'from-page+preconnect'; Mode = 'preconnect'; Features = $base},
  @{Name = 'from-page+prefetch';   Mode = 'prefetch';   Features = $base}
)
foreach ($i in 1..2) {
  foreach ($r in $runs) {
    "== $($r.Name) #$i"
    $env:WEBVIEW2_ADDITIONAL_BROWSER_ARGUMENTS = "--remote-debugging-port=9223 --enable-features=$($r.Features)"
    $env:LAB_MODE = $r.Mode
    $p = Start-Process $Exe -PassThru
    Start-Sleep 6
    node "$env:RUNNER_TEMP\lab.js"
    [P2]::PostMessage([P2]::FindWindow("UltraLightBrowserMainWindow", [NullString]::Value), 0x10, [IntPtr]::Zero, [IntPtr]::Zero) | Out-Null
    $p.WaitForExit(15000) | Out-Null
    if (-not $p.HasExited) { Stop-Process $p -Force }
    Start-Sleep 2
  }
}
