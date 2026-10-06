param([string]$Exe, [string]$Out)
$ErrorActionPreference = 'Continue'
New-Item -ItemType Directory -Force $Out | Out-Null
Add-Type -AssemblyName System.Windows.Forms, System.Drawing, Microsoft.VisualBasic
Add-Type @"
using System; using System.Runtime.InteropServices;
public static class U {
  [DllImport("user32.dll")] public static extern bool SetCursorPos(int x, int y);
  [DllImport("user32.dll")] public static extern void mouse_event(uint f, uint dx, uint dy, uint d, IntPtr e);
  [DllImport("user32.dll")] public static extern bool GetWindowRect(IntPtr h, out RECT r);
  [DllImport("user32.dll")] public static extern bool SetForegroundWindow(IntPtr h);
  [DllImport("user32.dll")] public static extern IntPtr FindWindow(string c, string t);
  [DllImport("user32.dll")] public static extern bool PostMessage(IntPtr h, uint m, IntPtr w, IntPtr l);
  [DllImport("user32.dll")] public static extern bool GetClientRect(IntPtr h, out RECT r);
  [DllImport("user32.dll")] public static extern bool ClientToScreen(IntPtr h, ref POINT p);
  [StructLayout(LayoutKind.Sequential)] public struct RECT { public int L, T, R, B; }
  [StructLayout(LayoutKind.Sequential)] public struct POINT { public int X, Y; }
}
"@
function Shot($name) {
  $b = [System.Windows.Forms.Screen]::PrimaryScreen.Bounds
  $bmp = New-Object System.Drawing.Bitmap $b.Width, $b.Height
  $g = [System.Drawing.Graphics]::FromImage($bmp)
  $g.CopyFromScreen($b.Location, [System.Drawing.Point]::Empty, $b.Size)
  $bmp.Save("$Out\$name.png", [System.Drawing.Imaging.ImageFormat]::Png)
  $g.Dispose(); $bmp.Dispose()
  "shot $name"
}
function Win { [U]::FindWindow("UltraLightBrowserMainWindow", [NullString]::Value) }
function Focus { $h = Win; if ($h -ne [IntPtr]::Zero) { [U]::SetForegroundWindow($h) | Out-Null }; Start-Sleep -Milliseconds 300 }
function Keys($k, $wait = 1500) { Focus; [System.Windows.Forms.SendKeys]::SendWait($k); Start-Sleep -Milliseconds $wait }
function ClickRel($dx, $dy, $fromRight = $false) {
  $r = New-Object U+RECT; [U]::GetClientRect((Win), [ref]$r) | Out-Null
  $pt = New-Object U+POINT; $pt.X = $(if ($fromRight) { $r.R - $dx } else { $dx }); $pt.Y = $dy
  [U]::ClientToScreen((Win), [ref]$pt) | Out-Null
  [U]::SetCursorPos($pt.X, $pt.Y) | Out-Null
  [U]::mouse_event(2, 0, 0, 0, [IntPtr]::Zero); [U]::mouse_event(4, 0, 0, 0, [IntPtr]::Zero)
  Start-Sleep -Milliseconds 1200
}
"Screen: " + [System.Windows.Forms.Screen]::PrimaryScreen.Bounds
$env:WEBVIEW2_ADDITIONAL_BROWSER_ARGUMENTS = '--remote-debugging-port=9222'
npm i --silent --prefix $env:RUNNER_TEMP ws | Out-Null
$env:NODE_PATH = Join-Path $env:RUNNER_TEMP 'node_modules'
@'
const WebSocket = require('ws');
(async () => {
  const list = await (await fetch('http://127.0.0.1:9222/json/list')).json();
  for (const t of list) {
    console.log('TARGET', t.type, t.url, '|', t.title);
    if (!t.url.includes('ulb.internal')) continue;
    const ws = new WebSocket(t.webSocketDebuggerUrl);
    await new Promise(r => ws.on('open', r));
    const logs = [];
    ws.on('message', m => { const d = JSON.parse(m); if (d.id === 1) console.log('  RESULT', JSON.stringify(d.result).slice(0, 600)); if (d.method) logs.push(d.method + ' ' + JSON.stringify(d.params).slice(0, 300)); });
    ws.send(JSON.stringify({id: 1, method: 'Runtime.evaluate', params: {returnByValue: true, expression:
      `JSON.stringify({ready: document.readyState, href: location.href, ulb: typeof ULB, wv: !!(window.chrome && chrome.webview), body: (document.body ? document.body.innerText : '').slice(0, 160), bg: getComputedStyle(document.body).backgroundColor, scripts: [...document.scripts].map(s => s.src)})`}}));
    ws.send(JSON.stringify({id: 2, method: 'Log.enable'}));
    ws.send(JSON.stringify({id: 3, method: 'Runtime.enable'}));
    await new Promise(r => setTimeout(r, 800));
    ws.on('message', () => {});
    console.log('  EVAL', logs.length, logs.join('\n  ').slice(0, 1500));
    ws.close();
  }
  process.exit(0);
})().catch(e => { console.log('CDP-ERR', e.message); process.exit(0); });
'@ | Set-Content cdp.js
$p = Start-Process $Exe -PassThru
Start-Sleep 12
$h = Win
"window: $h"
# maximize for a stable layout
Add-Type -Name W -Namespace N -MemberDefinition '[DllImport("user32.dll")] public static extern bool ShowWindow(IntPtr h, int c);'
[N.W]::ShowWindow($h, 3) | Out-Null
Start-Sleep 2
Shot "01-start"
# Hover the empty title-bar gap where Windows keeps its hidden caption buttons.
$r = New-Object U+RECT; [U]::GetClientRect((Win), [ref]$r) | Out-Null
$pt = New-Object U+POINT; $pt.X = $r.R - 45; $pt.Y = 6; [U]::ClientToScreen((Win), [ref]$pt) | Out-Null
[U]::SetCursorPos($pt.X, $pt.Y) | Out-Null; Start-Sleep -Milliseconds 300
[U]::SetCursorPos($pt.X + 2, $pt.Y + 1) | Out-Null; Start-Sleep 3
Shot "00-caption-hover"
node cdp.js
Keys "^l" 500; Keys "example.com{ENTER}" 5000; Shot "02-example"
Keys "^t" 1500; Keys "en.wikipedia.org/wiki/Web_browser{ENTER}" 7000; Shot "03-two-tabs"
ClickRel 27 26 $true; Shot "04-menu"; Keys "{ESC}" 800
ClickRel 135 26 $true; Shot "05-share"; Keys "{ESC}" 800
Keys "^+r" 4000; Shot "06-reader"
Keys "^+r" 3000
Keys "^f" 800; Keys "browser" 2000; Shot "07-find"; Keys "{ESC}" 800
Keys "^+l" 3000; Shot "08-sidebar"
Keys "^+\" 3000; Shot "09-overview"; Keys "{ESC}" 1000
Keys "^h" 3000; Shot "10-history"
Keys "^d" 1500; Keys "^+b" 3000; Shot "11-bookmarks"
Keys "^," 3000; Shot "12-settings"
Keys "^l" 500; Keys "ulb.internal/settings.html{ENTER}" 3000; Shot "13-settings-from-address"
Keys "^t" 2500; Shot "14-new-start"
Keys "^+n" 6000; Shot "15-private"
Keys "^+w" 2500
Keys "^2" 1500; Keys "^d" 1200; Keys "^+d" 1200; Shot "16-bookmarked-toast"
Keys "^l" 500; Keys "wiki" 1500; Shot "17-suggest"; Keys "{ESC}" 500
Keys "^w" 1500; Shot "18-closed"; Keys "^+t" 4000; Shot "19-reopened"
"alive before exit: " + (-not $p.HasExited)
[U]::PostMessage((Win), 0x10, [IntPtr]::Zero, [IntPtr]::Zero) | Out-Null
Start-Sleep 8
"exited after close: " + $p.HasExited + " code=" + $(if ($p.HasExited) { $p.ExitCode } else { 'n/a' })
# Trace audit: everything left on disk after a normal exit (built-in page files excluded).
$root = "$env:LOCALAPPDATA\UltraLightBrowser"
"TRACE files left:"
Get-ChildItem $root -Recurse -File -Force | Where-Object { $_.FullName -notmatch '\\ui\\[^\\]+\.(html|js|css)$' -and $_.FullName -notmatch '\\ui\\(vendor|\.stamp)' } |
  ForEach-Object { "  " + $_.FullName.Substring($root.Length) + "  " + $_.Length }
"WARP account: " + ((Get-Content "$root\warp\account.json" -Raw -ErrorAction SilentlyContinue | ConvertFrom-Json) | ForEach-Object { "best=$($_.best_endpoint) rtt=$($_.best_rtt_ms)ms" })
"TRACE Local State keys: " + ((Get-Content "$root\UserData\EBWebView\Local State" -Raw -ErrorAction SilentlyContinue | ConvertFrom-Json).PSObject.Properties.Name -join ',')
Get-Content "$env:LOCALAPPDATA\UltraLightBrowser\library.json" -ErrorAction SilentlyContinue
$p = Start-Process $Exe -PassThru
Start-Sleep 10
[N.W]::ShowWindow((Win), 3) | Out-Null
Start-Sleep 1
Keys "^+b" 3000; Shot "20-restart-bookmarks"
ClickRel 27 26 $true; Keys "{DOWN}{DOWN}{DOWN}{DOWN}{DOWN}{DOWN}{DOWN}{ENTER}" 3000; Shot "21-reading-list"
Stop-Process -Name UltraLightBrowser -Force -ErrorAction SilentlyContinue
"WARP account after 2nd run: " + ((Get-Content "$env:LOCALAPPDATA\UltraLightBrowser\warp\account.json" -Raw -ErrorAction SilentlyContinue | ConvertFrom-Json) | ForEach-Object { "best=$($_.best_endpoint) rtt=$($_.best_rtt_ms)ms scanned=$($_.scanned_at)" })
Start-Sleep 3
$cfgPath = "$env:LOCALAPPDATA\UltraLightBrowser\config.json"
$cfg = Get-Content $cfgPath -Raw | ConvertFrom-Json
$cfg.settings.userAgentProfile = 'macos-edge'
$cfg | ConvertTo-Json -Depth 8 | Set-Content $cfgPath -Encoding utf8NoBOM
$p = Start-Process $Exe -PassThru
Start-Sleep 10
Keys "^t" 1500; Keys "example.com{ENTER}" 5000
Keys "^t" 1500; Keys "example.org" 1500; Keys "{ENTER}" 5000; Shot "22-mac-tabs"
@'
const WebSocket = require('ws');
(async () => {
  const list = await (await fetch('http://127.0.0.1:9222/json/list')).json();
  for (const t of list.filter(t => t.type === 'page' && /example\.(com|org)/.test(t.url))) {
    const ws = new WebSocket(t.webSocketDebuggerUrl);
    await new Promise(r => ws.on('open', r));
    ws.send(JSON.stringify({id: 1, method: 'Runtime.evaluate', params: {returnByValue: true, awaitPromise: true, expression:
      `navigator.userAgentData.getHighEntropyValues(['platformVersion']).then(h => [location.host, navigator.platform, navigator.userAgentData.platform, h.platformVersion, navigator.userAgent.includes('Macintosh'), 'delivery=' + (performance.getEntriesByType('navigation')[0].deliveryType || 'network')].join(' '))`}}));
    await new Promise(r => ws.on('message', m => { const d = JSON.parse(m); if (d.id === 1) { console.log('MAC', JSON.stringify(d.result.result.value)); r(); } }));
    ws.close();
  }
  // WARP: exit address seen by Cloudflare, and WebRTC candidates (must not show the real address).
  const page = list.find(t => t.type === 'page' && /example\.(com|org)/.test(t.url));
  if (page) {
    const ws = new WebSocket(page.webSocketDebuggerUrl);
    await new Promise(r => ws.on('open', r));
    const ev = (id, expression) => new Promise(r => { ws.on('message', m => { const d = JSON.parse(m); if (d.id === id) r(d.result && d.result.result && d.result.result.value); });
      ws.send(JSON.stringify({id, method: 'Runtime.evaluate', params: {returnByValue: true, awaitPromise: true, expression}})); });
    console.log('WARP-TRACE', JSON.stringify(await ev(11, `fetch('https://www.cloudflare.com/cdn-cgi/trace').then(r => r.text()).then(t => t.split('\\n').filter(l => /^(ip|warp|loc)=/.test(l)).join(' ')).catch(e => 'ERR ' + e.message)`)));
    console.log('WEBRTC', JSON.stringify(await ev(12, `new Promise(res => { const pc = new RTCPeerConnection({iceServers: [{urls: 'stun:stun.l.google.com:19302'}]}); const c = []; pc.onicecandidate = e => { if (e.candidate) c.push(e.candidate.candidate.split(' ').slice(4, 8).join(' ')); else res(c.join(' | ') || 'none'); }; pc.createDataChannel('x'); pc.createOffer().then(o => pc.setLocalDescription(o)); setTimeout(() => res(c.join(' | ') || 'none'), 5000); })`)));
    ws.close();
  }
  process.exit(0);
})().catch(e => { console.log('CDP-ERR', e.message); process.exit(0); });
'@ | Set-Content mac.js
node mac.js
"alive: " + (-not $p.HasExited)
Get-Process UltraLightBrowser -ErrorAction SilentlyContinue | Select-Object Id, MainWindowTitle | Format-Table | Out-String
Get-WinEvent -FilterHashtable @{LogName='Application'; StartTime=(Get-Date).AddMinutes(-5)} -ErrorAction SilentlyContinue |
  Where-Object { $_.ProviderName -in 'Application Error','Windows Error Reporting' } | ForEach-Object { $_.Message } | Select-Object -First 3
Stop-Process -Name UltraLightBrowser -Force -ErrorAction SilentlyContinue

# "WARP 断开时阻止联网" must hold even when the WARP helper cannot start at all:
# the helper file is replaced by a directory so extraction fails.
Start-Sleep 3
$cfg = Get-Content $cfgPath -Raw | ConvertFrom-Json
$cfg.settings.userAgentProfile = 'default'
$cfg.settings | Add-Member -NotePropertyName warpFailClosed -NotePropertyValue $true -Force
$cfg.settings.startupPage = 'start'   # built-in page: loads without network
$cfg.settings.newTabPage = 'start'
$cfg | ConvertTo-Json -Depth 8 | Set-Content $cfgPath -Encoding utf8NoBOM
$helper = "$env:LOCALAPPDATA\UltraLightBrowser\warp\ulb-warp.exe"
Remove-Item $helper -Force -ErrorAction SilentlyContinue
New-Item -ItemType Directory -Force $helper | Out-Null
$p = Start-Process $Exe -PassThru
Start-Sleep 12
@'
(async () => {
  const list = await (await fetch('http://127.0.0.1:9222/json/list')).json();
  const page = list.find(t => t.type === 'page' && t.url.includes('start.html'));
  console.log('FAILCLOSED pages', list.filter(t => t.type === 'page').map(t => t.url).join(' '));
  const ws = new (require('ws'))(page.webSocketDebuggerUrl);
  await new Promise(r => ws.on('open', r));
  // Page.navigate reports a network failure in errorText.
  ws.send(JSON.stringify({id: 1, method: 'Page.navigate', params: {url: 'https://www.cloudflare.com/cdn-cgi/trace'}}));
  await new Promise(r => ws.on('message', m => { const d = JSON.parse(m); if (d.id === 1) { console.log('FAILCLOSED-NOHELPER', JSON.stringify(d.result && d.result.errorText ? 'BLOCKED ' + d.result.errorText : 'REACHED')); r(); } }));
  process.exit(0);
})().catch(e => { console.log('CDP-ERR', e.message); process.exit(0); });
'@ | Set-Content failclosed.js
node failclosed.js
Stop-Process -Name UltraLightBrowser -Force -ErrorAction SilentlyContinue
Remove-Item $helper -Force -Recurse -ErrorAction SilentlyContinue

# "我的线路" never goes direct: (1) chosen but nothing imported, (2) an imported
# link whose server cannot be reached. The helper must start for (2).
$cfg = Get-Content $cfgPath -Raw | ConvertFrom-Json
$cfg.settings.warpFailClosed = $false
$cfg.settings | Add-Member -NotePropertyName useLine -NotePropertyValue $true -Force
$cfg | ConvertTo-Json -Depth 8 | Set-Content $cfgPath -Encoding utf8NoBOM
$lineFile = "$env:LOCALAPPDATA\UltraLightBrowser\warp\line.dat"
Remove-Item $lineFile -Force -ErrorAction SilentlyContinue
(Get-Content failclosed.js -Raw) -replace 'FAILCLOSED-NOHELPER', 'LINE-NOLINK' | Set-Content line1.js
(Get-Content failclosed.js -Raw) -replace 'FAILCLOSED-NOHELPER', 'LINE-UNREACHABLE' | Set-Content line2.js
$p = Start-Process $Exe -PassThru
Start-Sleep 12
node line1.js
Stop-Process -Name UltraLightBrowser -Force -ErrorAction SilentlyContinue
Start-Sleep 3
Add-Type -AssemblyName System.Security
$link = 'vless://00000000-0000-4000-8000-000000000000@[2001:db8::1]:443?encryption=none&flow=xtls-rprx-vision&security=reality&sni=www.amd.com&fp=chrome&pbk=AAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAA&sid=01&type=tcp'
$blob = [System.Security.Cryptography.ProtectedData]::Protect([Text.Encoding]::UTF8.GetBytes($link), $null, 'CurrentUser')
[IO.File]::WriteAllBytes($lineFile, $blob)
$p = Start-Process $Exe -PassThru
Start-Sleep 12
"LINE helper running: " + [bool](Get-Process ulb-warp -ErrorAction SilentlyContinue)
node line2.js
Stop-Process -Name UltraLightBrowser -Force -ErrorAction SilentlyContinue
Remove-Item $lineFile -Force -ErrorAction SilentlyContinue
