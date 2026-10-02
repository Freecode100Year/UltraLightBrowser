#pragma once
namespace UltraLight {
// Injected into every document (AddScriptToExecuteOnDocumentCreated) and every
// dedicated/service worker (Runtime.evaluate before the worker starts) in macOS mode.
// kMacUaPlaceholder is replaced with the JSON-quoted macOS user agent.
// Hides Windows-specific values that the UA/Client Hints override cannot reach.
// Patched functions keep their native name, length and toString() output.
inline constexpr const wchar_t* kMacUaPlaceholder = L"\"__ULB_MAC_UA__\"";
inline constexpr const wchar_t* kMacStealthScript = LR"js((() => {
  const G = globalThis;
  const fnToString = Function.prototype.toString;
  const masked = new WeakMap();
  const toStringProxy = new Proxy(fnToString, {
    apply(target, self, args) { return Reflect.apply(target, masked.get(self) || self, args); }
  });
  masked.set(toStringProxy, fnToString);
  Object.defineProperty(Function.prototype, "toString", { value: toStringProxy, writable: true, configurable: true, enumerable: false });
  const wrap = (orig, apply) => { const p = new Proxy(orig, { apply }); masked.set(p, orig); return p; };
  const hookMethod = (proto, name, apply) => {
    const d = proto && Object.getOwnPropertyDescriptor(proto, name);
    if (d && typeof d.value === "function") Object.defineProperty(proto, name, { ...d, value: wrap(d.value, apply) });
  };
  const hookGetter = (proto, name, value) => {
    const d = proto && Object.getOwnPropertyDescriptor(proto, name);
    if (d && d.get) Object.defineProperty(proto, name, { ...d, get: wrap(d.get, () => value) });
  };
  // WebGL: unmasked GPU strings as reported by Chromium on Apple Silicon.
  const VENDOR = "Google Inc. (Apple)";
  const RENDERER = "ANGLE (Apple, ANGLE Metal Renderer: Apple M1, Unspecified Version)";
  for (const C of [G.WebGLRenderingContext, G.WebGL2RenderingContext]) {
    hookMethod(C && C.prototype, "getParameter", (t, self, args) =>
      args[0] === 0x9245 ? VENDOR : args[0] === 0x9246 ? RENDERER : Reflect.apply(t, self, args));
  }
  // WebGPU adapter info.
  if (G.GPUAdapterInfo) {
    const P = G.GPUAdapterInfo.prototype;
    hookGetter(P, "vendor", "apple");
    hookGetter(P, "architecture", "metal-3");
    hookGetter(P, "device", "");
    hookGetter(P, "description", "");
  }
  // The DevTools override does not always reach navigator.platform in documents that
  // share a renderer with earlier pages; pin it here as well.
  if (G.Navigator && !G.WorkerNavigator) hookGetter(G.Navigator.prototype, "platform", "MacIntel");
  // Workers are outside Emulation.setUserAgentOverride's navigator.platform reach,
  // and service workers also keep the real navigator.userAgent.
  if (G.WorkerNavigator) {
    const UA = "__ULB_MAC_UA__";
    const P = G.WorkerNavigator.prototype;
    hookGetter(P, "platform", "MacIntel");
    hookGetter(P, "userAgent", UA);
    hookGetter(P, "appVersion", UA.slice(UA.indexOf("/") + 1));
  }
  // Shared workers are browser-level targets this page's DevTools session cannot
  // reach, so they would report the real OS; hide the constructor instead.
  if (G.SharedWorker && G.document) delete G.SharedWorker;
  // Drop locally installed Windows (SAPI) voices; Edge online voices exist on both systems.
  if (G.SpeechSynthesis) {
    hookMethod(G.SpeechSynthesis.prototype, "getVoices", (t, self, args) =>
      Reflect.apply(t, self, args).filter((v) => !v.localService));
  }
})();)js";
}
