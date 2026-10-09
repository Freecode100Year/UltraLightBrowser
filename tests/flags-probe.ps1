# Which Chromium switches and profile preferences actually take effect inside
# WebView2? Launches the browser once per configuration and reports, over CDP,
# the command line the browser process received and what a page can observe.
param([string]$Exe)
$ErrorActionPreference = 'Continue'
if (-not (Test-Path "$env:RUNNER_TEMP\node_modules\pngjs")) { npm i --silent --prefix $env:RUNNER_TEMP ws pngjs | Out-Null }
$env:NODE_PATH = Join-Path $env:RUNNER_TEMP 'node_modules'
$root = "$env:LOCALAPPDATA\UltraLightBrowser"

# Local test server: 127.0.0.1 and localhost are different sites, so a frame
# from one inside the other is third-party.
$server = Start-Job {
  $l = [System.Net.HttpListener]::new(); $l.Prefixes.Add('http://127.0.0.1:8765/'); $l.Prefixes.Add('http://localhost:8765/'); $l.Start()
  while ($true) {
    $c = $l.GetContext(); $p = $c.Request.Url.AbsolutePath; $r = $c.Response
    $body = switch ($p) {
      '/top'   { '<html><body>top<iframe src="http://localhost:8765/frame"></iframe></body></html>' }
      '/frame' { $r.AddHeader('Set-Cookie', 'tp=1; SameSite=None; Secure; Path=/'); '<script>addEventListener("load",async()=>{let h=null;try{h=await document.hasStorageAccess()}catch(e){h="err"};parent.postMessage({cookie:document.cookie,hsa:h},"*")})</script>frame' }
      default  { '<html><body style="background:#fff;color:#000"><h1>white page</h1></body></html>' }
    }
    $b = [Text.Encoding]::UTF8.GetBytes($body); $r.ContentType = 'text/html'; $r.OutputStream.Write($b, 0, $b.Length); $r.Close()
  }
}
Start-Sleep 2

@'
const WebSocket = require('ws'); const { PNG } = require('pngjs');
(async () => {
  let page;
  let last = '';
  for (let i = 0; i < 120 && !page; i++) {
    try { const l = await (await fetch('http://127.0.0.1:9223/json/list')).json(); last = JSON.stringify(l.map(t => t.type + ' ' + t.url)); page = l.find(t => t.type === 'page'); } catch (e) { last = String(e); }
    if (!page) await new Promise(r => setTimeout(r, 500));
  }
  if (!page) { console.log('NO PAGE ' + last); process.exit(0); }
  const bv = await (await fetch('http://127.0.0.1:9223/json/version')).json();
  const conn = async (url) => { const ws = new WebSocket(url); await new Promise(r => ws.on('open', r)); let id = 0; const pend = {};
    ws.on('message', m => { const d = JSON.parse(m); if (d.id && pend[d.id]) { pend[d.id](d); delete pend[d.id]; } });
    return (method, params = {}) => new Promise(r => { const i = ++id; pend[i] = r; ws.send(JSON.stringify({id: i, method, params})); setTimeout(() => r({}), 20000); }); };
  const browser = await conn(bv.webSocketDebuggerUrl);
  const cl = await browser('Browser.getBrowserCommandLine');
  console.log('VERSION ' + bv.Browser);
  console.log('CMDLINE ' + JSON.stringify(cl.result ? cl.result.arguments.filter(a => /features|proxy|disable|enable/.test(a)) : cl.error));
  const send = await conn(page.webSocketDebuggerUrl);
  const ev = async (e) => { const r = await send('Runtime.evaluate', {expression: e, returnByValue: true, awaitPromise: true}); return r.result && r.result.result && r.result.result.value; };
  const sleep = (ms) => new Promise(r => setTimeout(r, ms));
  const nav = async (u, ms = 3000) => { await send('Page.navigate', {url: u}); await sleep(ms); };

  await nav('http://127.0.0.1:8765/white');
  console.log('APIS ' + await ev(`JSON.stringify({topics: 'browsingTopics' in document, joinAdInterestGroup: 'joinAdInterestGroup' in navigator, runAdAuction: 'runAdAuction' in navigator, sharedStorage: 'sharedStorage' in window, fencedFrame: typeof HTMLFencedFrameElement, attribution: 'attributionReporting' in window, privateAggregation: 'privateAggregation' in window, hasStorageAccess: 'hasStorageAccess' in document, gpc: navigator.globalPrivacyControl, dnt: navigator.doNotTrack})`));
  const shot = await send('Page.captureScreenshot', {format: 'png', clip: {x: 400, y: 300, width: 4, height: 4, scale: 1}});
  if (shot.result) { const png = PNG.sync.read(Buffer.from(shot.result.data, 'base64')); console.log('WHITE-PAGE-PIXEL ' + [png.data[0], png.data[1], png.data[2]].join(',')); }
  console.log('PREFERS-DARK ' + await ev(`matchMedia('(prefers-color-scheme: dark)').matches`));

  await nav('http://127.0.0.1:8765/top', 1000);
  console.log('THIRD-PARTY ' + await ev(`new Promise(r => { addEventListener('message', e => r(JSON.stringify(e.data))); setTimeout(() => r('timeout'), 6000); document.querySelector('iframe').src = document.querySelector('iframe').src; })`));

  await nav('http://neverssl.com/', 6000);
  console.log('HTTP-NAV ' + await ev(`location.href + ' | ' + document.title`));
  process.exit(0);
})();
'@ | Set-Content -Encoding utf8NoBOM "$env:RUNNER_TEMP\probe.js"

$configs = @(
  @{Name = 'baseline-noauto'; Args = ''; Prefs = $null; NoAuto = $true},
  @{Name = 'baseline'; Args = ''; Prefs = $null},
  @{Name = 'privacy-sandbox-off'; Args = '--disable-features=Translate,OptimizationHints,MediaRouter,BrowsingTopics,InterestGroupStorage,Fledge,AdInterestGroupAPI,PrivateAggregationApi,SharedStorageAPI,FencedFrames,ConversionMeasurement,AttributionReportingCrossAppWeb,AutofillServerCommunication'; Prefs = $null},
  @{Name = 'force-dark'; Args = '--enable-features=PageDiscarding,Freezer,WebContentsForceDark'; Prefs = $null},
  @{Name = 'block-3pc-pref'; Args = ''; Prefs = '{"profile":{"cookie_controls_mode":1,"block_third_party_cookies":true}}'},
  @{Name = 'https-only-pref'; Args = ''; Prefs = '{"https_only_mode_enabled":true}'},
  @{Name = 'gpc-flag'; Args = '--enable-features=PageDiscarding,Freezer,GlobalPrivacyControl'; Prefs = $null}
)
foreach ($c in $configs) {
  "===== $($c.Name)"
  Get-Process UltraLightBrowser, ulb-warp, msedgewebview2 -ErrorAction SilentlyContinue | Stop-Process -Force
  Start-Sleep 2
  Remove-Item -Recurse -Force $root -ErrorAction SilentlyContinue
  New-Item -ItemType Directory -Force $root | Out-Null
  '{"settings":{"warpEnabled":false,"startupPage":"blank","homeVersion":2}}' | Set-Content -Encoding utf8NoBOM "$root\config.json"
  if ($c.Prefs) {
    New-Item -ItemType Directory -Force "$root\UserData\EBWebView\Default" | Out-Null
    $c.Prefs | Set-Content -Encoding utf8NoBOM "$root\UserData\EBWebView\Default\Preferences"
  }
  $auto = if ($c.NoAuto) { '' } else { '--enable-automation' }
  $env:WEBVIEW2_ADDITIONAL_BROWSER_ARGUMENTS = "--remote-debugging-port=9223 $auto $($c.Args)".Trim()
  $p = Start-Process $Exe -PassThru
  Start-Sleep 3
  Start-Sleep 5
  "ALIVE " + (-not $p.HasExited) + " webview2=" + @(Get-Process msedgewebview2 -ErrorAction SilentlyContinue).Count
  Get-CimInstance Win32_Process -Filter "Name='msedgewebview2.exe'" | Where-Object { $_.CommandLine -notmatch '--type=' } | ForEach-Object { "BROWSERPROC " + ($_.CommandLine -replace '--user-data-dir="[^"]*"', '') }
  "LISTEN " + ((Get-NetTCPConnection -State Listen -ErrorAction SilentlyContinue | Where-Object { $_.LocalPort -in 9222, 9223 } | ForEach-Object { "$($_.LocalAddress):$($_.LocalPort)" }) -join ' ')
  node "$env:RUNNER_TEMP\probe.js" 2>&1
  if (Test-Path "$root\UserData\EBWebView\Default\Preferences") {
    $pr = Get-Content -Raw "$root\UserData\EBWebView\Default\Preferences" | ConvertFrom-Json
    "PREFS-AFTER cookie_controls_mode=$($pr.profile.cookie_controls_mode) block_3pc=$($pr.profile.block_third_party_cookies) https_only=$($pr.https_only_mode_enabled)"
  }
  Stop-Process -Id $p.Id -Force -ErrorAction SilentlyContinue
}
Stop-Job $server; Remove-Job $server -Force
