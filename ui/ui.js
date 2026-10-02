// Shared helpers for UltraLightBrowser internal pages (https://ulb.internal/).
"use strict";
if (window.top !== window) { document.documentElement.innerHTML = ""; throw new Error("framed"); }

const ULB = (() => {
  const wv = window.chrome && window.chrome.webview;
  const pending = new Map();
  const listeners = {};
  let seq = 0;
  if (wv) {
    wv.addEventListener("message", (e) => {
      const m = e.data;
      if (!m || typeof m !== "object") return;
      if (m.id && pending.has(m.id)) {
        const p = pending.get(m.id);
        pending.delete(m.id);
        if (m.error) p.reject(new Error(m.error)); else p.resolve(m.result);
      } else if (m.event) {
        (listeners[m.event] || []).forEach((f) => f(m.data));
      }
    });
  }
  const call = (cmd, args = {}) => new Promise((resolve, reject) => {
    if (!wv) { reject(new Error("bridge unavailable")); return; }
    const id = ++seq;
    pending.set(id, { resolve, reject });
    wv.postMessage({ id, cmd, args });
  });
  const on = (event, fn) => { (listeners[event] = listeners[event] || []).push(fn); };

  const el = (tag, attrs = {}, ...children) => {
    const n = document.createElement(tag);
    for (const [k, v] of Object.entries(attrs)) {
      if (v === undefined || v === null || v === false) continue;
      if (k === "class") n.className = v;
      else if (k === "text") n.textContent = v;
      else if (k.startsWith("on") && typeof v === "function") n.addEventListener(k.slice(2), v);
      else if (k === "style" && typeof v === "object") Object.assign(n.style, v);
      else n.setAttribute(k, v === true ? "" : v);
    }
    for (const c of children.flat()) {
      if (c === null || c === undefined || c === false) continue;
      n.append(c instanceof Node ? c : document.createTextNode(String(c)));
    }
    return n;
  };

  const host = (url) => { try { return new URL(url).hostname.replace(/^www\./, ""); } catch { return ""; } };
  const hue = (s) => { let h = 0; for (const ch of s) h = (h * 31 + ch.codePointAt(0)) >>> 0; return h % 360; };
  const faviconFile = (h) => "cache/fav/" + h.toLowerCase().replace(/[^a-z0-9.\-]/g, "_") + ".png";

  // Site icon: cached favicon when we have one, otherwise a coloured letter tile.
  const icon = (url, size = 16, dataUrl = null) => {
    const h = host(url);
    const letter = (h.replace(/^(m|www)\./, "")[0] || "•").toUpperCase();
    const tile = el("span", { class: "tile", style: {
      width: size + "px", height: size + "px", fontSize: Math.round(size * 0.55) + "px",
      background: `hsl(${hue(h || url)} 45% 42%)`, borderRadius: Math.round(size * 0.22) + "px" } }, letter);
    const src = dataUrl || (h ? faviconFile(new URL(url).hostname) : null);
    if (src) {
      const img = new Image();
      img.alt = "";
      img.onload = () => { if (img.naturalWidth > 1) { tile.classList.add("has-img"); tile.append(img); } };
      img.src = src;
    }
    return tile;
  };

  const fmtTime = (ms) => new Date(ms).toLocaleTimeString("zh-CN", { hour: "2-digit", minute: "2-digit" });
  const dayLabel = (ms) => {
    const d = new Date(ms); const today = new Date(); today.setHours(0, 0, 0, 0);
    const day = new Date(d); day.setHours(0, 0, 0, 0);
    const diff = Math.round((today - day) / 86400000);
    if (diff === 0) return "今天";
    if (diff === 1) return "昨天";
    return d.toLocaleDateString("zh-CN", { year: "numeric", month: "long", day: "numeric", weekday: "long" });
  };

  const modal = (title, fields, okText = "完成") => new Promise((resolve) => {
    const inputs = {};
    const close = (v) => { bg.remove(); resolve(v); };
    const form = el("form", { onsubmit: (e) => { e.preventDefault(); const out = {}; for (const [k, i] of Object.entries(inputs)) out[k] = i.value.trim(); close(out); } },
      el("h3", { text: title }),
      fields.map((f) => {
        let input;
        if (f.options) {
          input = el("select", {}, f.options.map((o) => el("option", { value: o, selected: o === f.value }, o)));
        } else {
          input = el("input", { type: f.type || "text", value: f.value || "", placeholder: f.placeholder || "", required: f.required });
        }
        inputs[f.name] = input;
        return [el("label", { text: f.label }), input];
      }),
      el("div", { class: "actions" },
        el("button", { type: "button", onclick: () => close(null) }, "取消"),
        el("button", { type: "submit", class: "primary" }, okText)));
    const bg = el("div", { class: "modal-bg", onmousedown: (e) => { if (e.target === bg) close(null); } }, el("div", { class: "modal" }, form));
    document.body.append(bg);
    const first = form.querySelector("input,select");
    if (first) first.focus();
    bg.addEventListener("keydown", (e) => { if (e.key === "Escape") close(null); });
  });

  const toast = (text) => {
    const t = el("div", { class: "toast", text });
    document.body.append(t);
    setTimeout(() => t.remove(), 1800);
  };

  const normalizeUrl = (s) => {
    s = (s || "").trim();
    if (!s) return "";
    if (/^[a-z][a-z0-9+.-]*:/i.test(s)) return s;
    return "https://" + s;
  };

  return { call, on, el, host, icon, fmtTime, dayLabel, modal, toast, normalizeUrl, hue };
})();
