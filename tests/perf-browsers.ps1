# Loads the same sites as perf-load.ps1 in Chrome, Edge and Firefox (fresh profile, then
# again after a restart with the profile and cache kept) for a side-by-side comparison.
param([string]$PerfJs)
$ErrorActionPreference = 'Continue'
$sites = @('https://www.wikipedia.org/', 'https://github.com/', 'https://www.bbc.com/news', 'https://www.theguardian.com/international', 'https://www.reddit.com/', 'https://stackoverflow.com/questions')

# Firefox speaks WebDriver BiDi rather than the DevTools protocol.
@'
const WebSocket = require('ws');
const sites = JSON.parse(process.env.SITES);
(async () => {
  let ws;
  for (let i = 0; i < 40 && !ws; i++) {
    try { ws = new WebSocket('ws://127.0.0.1:9223/session'); await new Promise((r, j) => { ws.on('open', r); ws.on('error', j); }); }
    catch { ws = null; await new Promise(r => setTimeout(r, 500)); }
  }
  let id = 0; const pend = {};
  ws.on('message', m => { const d = JSON.parse(m); if (d.id && pend[d.id]) { pend[d.id](d); delete pend[d.id]; } });
  const send = (method, params = {}) => new Promise(r => { const i = ++id; pend[i] = r; ws.send(JSON.stringify({id: i, method, params})); });
  await send('session.new', {capabilities: {}});
  const tree = await send('browsingContext.getTree', {});
  const context = tree.result.contexts[0].context;
  const results = [], frames = [], slows = [];
  for (const url of sites) {
    const t0 = Date.now();
    await Promise.race([send('browsingContext.navigate', {context, url, wait: 'complete'}), new Promise(r => setTimeout(r, 30000))]);
    const ms = Date.now() - t0;
    const ev = async (expression) => { const r = await send('script.evaluate', {expression, target: {context}, awaitPromise: true}); return r.result && r.result.result && r.result.result.value; };
    const v = JSON.parse(await ev('JSON.stringify((()=>{const n=performance.getEntriesByType("navigation")[0]||{};const fcp=performance.getEntriesByName("first-contentful-paint")[0];return {dcl:Math.round(n.domContentLoadedEventEnd||0),fcp:fcp?Math.round(fcp.startTime):0,res:performance.getEntriesByType("resource").length}})())') || '{}');
    const s = JSON.parse(await ev('new Promise(res => { let n = 0, slow = 0, start = 0, last = 0; const f = (t) => { if (!start) start = last = t; else { n++; if (t - last > 34) slow++; last = t; } window.scrollBy(0, 40); if (t - start < 3000) requestAnimationFrame(f); else res(JSON.stringify({fps: Math.round(n * 1000 / (t - start)), slow})); }; requestAnimationFrame(f); setTimeout(() => res("{}"), 6000); })') || '{}');
    results.push(ms); frames.push(s.fps || 0); slows.push(s.slow || 0);
    console.log(`  ${url.padEnd(46)} load ${String(ms).padStart(6)} ms  fcp ${String(v.fcp).padStart(5)}  dcl ${String(v.dcl).padStart(5)}  resources ${String(v.res).padStart(4)}  scroll ${s.fps} fps, ${s.slow} slow frames`);
  }
  console.log('  TOTAL', results.reduce((a, b) => a + b, 0), 'ms', ' FPS-AVG', Math.round(frames.reduce((a, b) => a + b, 0) / frames.length), ' SLOW', slows.reduce((a, b) => a + b, 0));
  await send('session.end', {});
  process.exit(0);
})().catch(e => { console.log('ERR', e.message); process.exit(0); });
'@ | Set-Content "$env:RUNNER_TEMP\perf-bidi.js"
$env:SITES = ($sites | ConvertTo-Json -Compress)

$browsers = @(
  @{Name = 'Chrome';  Exe = "$env:ProgramFiles\Google\Chrome\Application\chrome.exe"; Bidi = $false},
  @{Name = 'Edge';    Exe = "${env:ProgramFiles(x86)}\Microsoft\Edge\Application\msedge.exe"; Bidi = $false},
  @{Name = 'Firefox'; Exe = "$env:ProgramFiles\Mozilla Firefox\firefox.exe"; Bidi = $true}
)
foreach ($b in $browsers) {
  if (-not (Test-Path $b.Exe)) { "== $($b.Name): not installed"; continue }
  $profile = Join-Path $env:RUNNER_TEMP ("profile-" + $b.Name)
  Remove-Item -Recurse -Force $profile -ErrorAction SilentlyContinue
  New-Item -ItemType Directory -Force $profile | Out-Null
  foreach ($run in @('first visit', 'after restart')) {
    "== $($b.Name) / $run"
    if ($b.Bidi) {
      $args = @('--remote-debugging-port', '9223', '--profile', $profile, '--no-remote', '--new-instance', 'about:blank')
    } else {
      $args = @('--remote-debugging-port=9223', "--user-data-dir=$profile", '--no-first-run', '--no-default-browser-check', '--start-maximized', 'about:blank')
    }
    $p = Start-Process $b.Exe -ArgumentList $args -PassThru
    Start-Sleep 6
    if ($b.Bidi) { node "$env:RUNNER_TEMP\perf-bidi.js" } else { node $PerfJs }
    Get-Process -Name ([IO.Path]::GetFileNameWithoutExtension($b.Exe)) -ErrorAction SilentlyContinue | ForEach-Object { $_.CloseMainWindow() | Out-Null }
    Start-Sleep 4
    Get-Process -Name ([IO.Path]::GetFileNameWithoutExtension($b.Exe)) -ErrorAction SilentlyContinue | Stop-Process -Force
    Start-Sleep 2
  }
}
