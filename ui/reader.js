"use strict";
const { el, call } = ULB;
let prefs = { theme: "sepia", font: "serif", size: 19 };

function applyPrefs() {
  document.body.className = `${prefs.theme} ${prefs.font}`;
  document.documentElement.style.setProperty("--size", prefs.size + "px");
  document.querySelectorAll(".theme").forEach((b) => b.classList.toggle("on", b.dataset.theme === prefs.theme));
}
const save = () => { applyPrefs(); call("setReaderPrefs", prefs); };

// The article HTML comes from an arbitrary page: parse it inertly and keep only
// content elements. The page CSP additionally forbids every script source.
function sanitize(html, baseUrl) {
  const doc = new DOMParser().parseFromString(`<body>${html}</body>`, "text/html");
  doc.querySelectorAll("script,style,link,meta,base,iframe,frame,object,embed,form,input,button,textarea,select,svg,math,template,noscript").forEach((n) => n.remove());
  for (const node of doc.body.querySelectorAll("*")) {
    for (const attr of [...node.attributes]) {
      const name = attr.name.toLowerCase();
      const value = attr.value.trim().toLowerCase();
      if (name.startsWith("on") || name === "style" || name === "srcdoc" || name === "class" || name === "id") { node.removeAttribute(attr.name); continue; }
      if ((name === "href" || name === "src" || name === "srcset" || name === "poster") && /^(javascript|data:text|vbscript)/.test(value)) node.removeAttribute(attr.name);
    }
    if (node.tagName === "A") {
      try { node.href = new URL(node.getAttribute("href") || "", baseUrl).href; } catch { node.removeAttribute("href"); }
    }
  }
  return [...doc.body.childNodes].map((n) => document.importNode(n, true));
}

(async () => {
  const key = location.hash.slice(1);
  const art = document.getElementById("article");
  let data;
  try { data = await call("getArticle", { key }); } catch { data = null; }
  if (!data || !data.content) {
    art.replaceChildren(el("p", { class: "error", text: "无法载入阅读器内容。" }));
    return;
  }
  prefs = Object.assign(prefs, data.prefs || {});
  applyPrefs();
  document.title = data.title || "阅读器";
  const meta = [data.siteName, data.byline, data.length ? `约 ${Math.max(1, Math.round(data.length / 500))} 分钟阅读` : null].filter(Boolean).join(" · ");
  art.replaceChildren(el("h1", { class: "title", text: data.title || "" }), el("div", { class: "meta", text: meta }), ...sanitize(data.content, data.url));
  if (data.dir) art.dir = data.dir;
  if (data.lang) document.documentElement.lang = data.lang;
})();

document.getElementById("smaller").onclick = () => { prefs.size = Math.max(13, prefs.size - 2); save(); };
document.getElementById("bigger").onclick = () => { prefs.size = Math.min(32, prefs.size + 2); save(); };
document.getElementById("font").onclick = () => { prefs.font = prefs.font === "serif" ? "sans" : "serif"; save(); };
document.getElementById("exit").onclick = () => call("exitReader");
document.querySelectorAll(".theme").forEach((b) => (b.onclick = () => { prefs.theme = b.dataset.theme; save(); }));
