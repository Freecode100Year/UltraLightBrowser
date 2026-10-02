#pragma once
namespace UltraLight {
// Local identity self-test, served from https://ulb-selftest.example/ (a secure
// origin, so userAgentData high-entropy values and service workers are available).
inline constexpr const char* kSelfTestHtml = R"html(<!doctype html>
<html lang="zh-CN"><head><meta charset="utf-8"><title>浏览器标识自检</title>
<style>
body{font:14px/1.5 -apple-system,system-ui,sans-serif;margin:24px;color:#222}
table{border-collapse:collapse;width:100%}td{border-bottom:1px solid #e5e5e5;padding:6px 8px;vertical-align:top;word-break:break-all}
td:first-child{width:220px;color:#666;white-space:nowrap}.warn{color:#c00;font-weight:600}
</style></head><body>
<h2>浏览器标识自检</h2>
<p>标红项与 macOS 不一致（Windows 模式下标红属正常）。请求头与字体请另用 browserleaks.com 等站点核对。</p>
<table id="t"></table>
<script>
const rows = [];
const add = (k, v, ok = true) => rows.push([k, typeof v === "string" ? v : JSON.stringify(v), ok]);
const render = () => { document.getElementById("t").innerHTML = rows.map(([k, v, ok]) =>
  `<tr><td>${k}</td><td class="${ok ? "" : "warn"}">${v.replace(/</g, "&lt;")}</td></tr>`).join(""); };
const mac = (s) => /Mac/.test(s);
const probe = `navigator.userAgentData.getHighEntropyValues(["platformVersion","architecture","fullVersionList"])
  .then(h => [navigator.userAgent, navigator.platform, navigator.appVersion, navigator.userAgentData.platform, h.platformVersion, h.architecture])`;
const fromWorker = (make) => new Promise((resolve) => {
  setTimeout(() => resolve(null), 3000);
  try { make(resolve); } catch (e) { resolve(String(e)); }
});
(async () => {
  add("navigator.userAgent", navigator.userAgent, mac(navigator.userAgent));
  add("navigator.platform", navigator.platform, navigator.platform === "MacIntel");
  const h = await navigator.userAgentData.getHighEntropyValues(
    ["platformVersion", "architecture", "bitness", "wow64", "fullVersionList", "formFactors"]);
  add("userAgentData.platform", navigator.userAgentData.platform, navigator.userAgentData.platform === "macOS");
  add("platformVersion / arch / bitness", `${h.platformVersion} / ${h.architecture} / ${h.bitness} / wow64=${h.wow64}`,
    navigator.userAgentData.platform === "macOS" && h.architecture === "arm");
  add("fullVersionList", h.fullVersionList.map((b) => `${b.brand} ${b.version}`).join(", "));
  const gl = document.createElement("canvas").getContext("webgl");
  const ext = gl && gl.getExtension("WEBGL_debug_renderer_info");
  const renderer = ext ? gl.getParameter(ext.UNMASKED_RENDERER_WEBGL) : "n/a";
  add("WebGL vendor", ext ? gl.getParameter(ext.UNMASKED_VENDOR_WEBGL) : "n/a");
  add("WebGL renderer", renderer, /Apple/.test(renderer));
  if (navigator.gpu) {
    const a = await navigator.gpu.requestAdapter().catch(() => null);
    if (a && a.info) add("WebGPU adapter", `${a.info.vendor} / ${a.info.architecture}`, a.info.vendor === "apple");
  }
  const voices = await new Promise((r) => {
    const v = speechSynthesis.getVoices(); if (v.length) return r(v);
    speechSynthesis.onvoiceschanged = () => r(speechSynthesis.getVoices()); setTimeout(() => r(speechSynthesis.getVoices()), 1500);
  });
  const local = voices.filter((v) => v.localService).length;
  add("语音列表", `${voices.length} 个，本地 ${local} 个`, local === 0);
  const sb = document.createElement("div");
  sb.style.cssText = "width:100px;height:100px;overflow:scroll;position:absolute;top:-999px";
  document.body.appendChild(sb); const sw = sb.offsetWidth - sb.clientWidth; sb.remove();
  add("滚动条宽度", `${sw}px`, sw === 0);
  add("toString 检查", WebGLRenderingContext.prototype.getParameter.toString(),
    /\[native code\]/.test(WebGLRenderingContext.prototype.getParameter.toString()));
  add("SharedWorker", typeof SharedWorker, typeof SharedWorker === "undefined");
  render();
  const w = await fromWorker((done) => {
    const wk = new Worker(URL.createObjectURL(new Blob([`${probe}.then(postMessage)`])));
    wk.onmessage = (e) => done(e.data);
  });
  add("Dedicated Worker", w || "超时", !!w && mac(w[0]) && w[1] === "MacIntel" && w[3] === "macOS");
  render();
  const s = await fromWorker(async (done) => {
    const reg = await navigator.serviceWorker.register("sw.js");
    await navigator.serviceWorker.ready;
    navigator.serviceWorker.onmessage = (e) => done(e.data);
    (reg.active || reg.waiting || reg.installing).postMessage(1);
  });
  add("Service Worker", s || "超时", !!s && Array.isArray(s) && mac(s[0]) && s[1] === "MacIntel" && s[3] === "macOS");
  render();
})().catch((e) => { add("错误", String(e), false); render(); });
</script></body></html>
)html";

inline constexpr const char* kSelfTestServiceWorker = R"js(
self.addEventListener("install", () => self.skipWaiting());
self.addEventListener("message", (e) => {
  navigator.userAgentData.getHighEntropyValues(["platformVersion", "architecture"]).then((h) =>
    e.source.postMessage([navigator.userAgent, navigator.platform, navigator.appVersion,
      navigator.userAgentData.platform, h.platformVersion, h.architecture]));
});
)js";
}
