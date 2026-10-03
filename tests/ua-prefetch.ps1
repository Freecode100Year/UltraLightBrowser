# Mac mode: which User-Agent and client hints does the server see for a normal
# navigation and for a speculation-rules prefetch started from the start page?
param([string]$Exe)
$ErrorActionPreference = 'Continue'
if (-not (Test-Path "$env:RUNNER_TEMP\node_modules\ws")) { npm i --silent --prefix $env:RUNNER_TEMP ws | Out-Null }
$env:NODE_PATH = Join-Path $env:RUNNER_TEMP 'node_modules'
$env:WEBVIEW2_ADDITIONAL_BROWSER_ARGUMENTS = '--remote-debugging-port=9223'
Remove-Item -Recurse -Force "$env:LOCALAPPDATA\UltraLightBrowser" -ErrorAction SilentlyContinue
New-Item -ItemType Directory -Force "$env:LOCALAPPDATA\UltraLightBrowser" | Out-Null
'{"settings":{"userAgentProfile":"macos-edge","startupPage":"start","newTabPage":"start","homeVersion":2}}' | Set-Content -Encoding utf8NoBOM "$env:LOCALAPPDATA\UltraLightBrowser\config.json"
@'
const WebSocket = require('ws');
(async () => {
  let target;
  for (let i = 0; i < 40 && !target; i++) {
    try { target = (await (await fetch('http://127.0.0.1:9223/json/list')).json()).find(t => t.type === 'page' && t.url.includes('start.html')); } catch {}
    if (!target) await new Promise(r => setTimeout(r, 500));
  }
  const ws = new WebSocket(target.webSocketDebuggerUrl);
  await new Promise(r => ws.on('open', r));
  let id = 0; const pend = {};
  ws.on('message', m => { const d = JSON.parse(m); if (d.id && pend[d.id]) { pend[d.id](d); delete pend[d.id]; } });
  const send = (method, params = {}) => new Promise(r => { const i = ++id; pend[i] = r; ws.send(JSON.stringify({id: i, method, params})); setTimeout(() => r({}), 15000); });
  const ev = async (e) => { const r = await send('Runtime.evaluate', {expression: e, returnByValue: true, awaitPromise: true}); return r.result && r.result.result && r.result.result.value; };
  const sleep = (ms) => new Promise(r => setTimeout(r, ms));
  const show = async (label) => {
    const v = await ev(`(() => { const n = performance.getEntriesByType('navigation')[0] || {}; return location.href + ' delivery=' + (n.deliveryType || 'network') + '\\n' + document.body.innerText.slice(0, 1500); })()`);
    console.log('--- ' + label + '\n' + v);
  };
  await ev(`(() => { const s = document.createElement('script'); s.type = 'speculationrules'; s.textContent = JSON.stringify({prefetch: [{source: 'list', urls: ['https://httpbin.org/headers'], referrer_policy: 'no-referrer'}]}); document.head.append(s); return 1; })()`);
  await sleep(3000);
  await ev(`location.href = 'https://httpbin.org/headers'; 1`);
  await sleep(4000);
  await show('prefetched navigation');
  await send('Page.navigate', {url: 'https://httpbin.org/anything?normal'});
  await sleep(4000);
  await show('normal navigation');
  process.exit(0);
})().catch(e => { console.log('ERR', e.message); process.exit(0); });
'@ | Set-Content "$env:RUNNER_TEMP\ua.js"
$p = Start-Process $Exe -PassThru
Start-Sleep 10
node "$env:RUNNER_TEMP\ua.js"
Stop-Process $p -Force -ErrorAction SilentlyContinue
