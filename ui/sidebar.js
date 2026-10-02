"use strict";
const { el, call, icon, host, modal } = ULB;
let state = null;
let seg = sessionStorage.getItem("seg") || "tabs";
let unreadOnly = false;
const openFolders = new Set(["个人收藏"]);

function item(glyph, label, opts = {}) {
  const row = el("div", { class: "si" + (opts.cls ? " " + opts.cls : ""), title: opts.title || label, onclick: opts.onclick },
    typeof glyph === "string" ? el("span", { class: "glyph", text: glyph }) : glyph,
    el("span", { class: "lbl", text: label }));
  if (opts.extra) row.append(...[].concat(opts.extra).filter(Boolean));
  if (opts.onclose) row.append(el("button", { class: "icon x", title: opts.closeTitle || "移除", onclick: (e) => { e.stopPropagation(); opts.onclose(); } }, "✕"));
  return row;
}

function renderTabs() {
  const out = [el("div", { class: "sh", text: "标签组" }),
    item("▭", "当前窗口", { cls: "act", extra: el("span", { class: "n", text: String(state.tabs.length) }) })];
  for (const g of state.groups) {
    out.push(item("▭", g.name, {
      title: `打开标签组“${g.name}”（${g.count} 个标签页）`,
      extra: el("span", { class: "n", text: String(g.count) }),
      onclick: () => call("openGroup", { id: g.id }),
      onclose: () => { if (confirm(`删除标签组“${g.name}”？`)) call("deleteGroup", { id: g.id }); },
      closeTitle: "删除标签组" }));
  }
  out.push(item("＋", "将当前标签页存为标签组", { cls: "link", onclick: async () => {
    const r = await modal("新建标签组", [{ name: "name", label: "名称", required: true, placeholder: "例如：工作" }], "存储");
    if (r && r.name) call("saveGroup", { name: r.name });
  } }));
  out.push(el("div", { class: "sh", text: "当前窗口的标签页" }));
  for (const t of state.tabs) {
    out.push(item(icon(t.url || "about:blank", 16, t.favicon), t.title || host(t.url) || "新标签页", {
      cls: t.active ? "act" : "",
      title: t.title + "\n" + t.url,
      extra: [t.audio ? el("span", { class: "audio", text: "🔊" }) : null, t.suspended ? el("span", { class: "sleep", text: "已挂起" }) : null],
      onclick: () => call("activateTab", { id: t.id }),
      onclose: () => call("closeTab", { id: t.id }), closeTitle: "关闭标签页" }));
  }
  if (!state.private) {
    out.push(el("div", { class: "sh", text: "无痕" }));
    out.push(item("◌", "新建无痕窗口", { onclick: () => call("newPrivateWindow") }));
  }
  return out;
}

function renderBookmarks() {
  const out = [el("div", { class: "sh" }, "书签", el("button", { onclick: () => call("open", { url: "https://ulb.internal/bookmarks.html" }) }, "编辑"))];
  for (const f of state.folders) {
    const items = state.bookmarks.filter((b) => b.folder === f);
    const open = openFolders.has(f);
    out.push(item(open ? "▾" : "▸", f, { cls: "fold", extra: el("span", { class: "n", text: String(items.length) }),
      onclick: () => { open ? openFolders.delete(f) : openFolders.add(f); render(); } }));
    if (open) {
      const sub = el("div", { class: "sub" }, items.length ? items.map((b) => item(icon(b.url, 16), b.title, {
        title: b.title + "\n" + b.url, onclick: () => call("open", { url: b.url }) })) : el("div", { class: "si read" }, el("span", { class: "lbl", text: "（空）" })));
      out.push(sub);
    }
  }
  return out;
}

function renderReading() {
  const items = state.reading.filter((r) => !unreadOnly || !r.read);
  const out = [el("div", { class: "sh" }, "阅读列表",
    el("button", { onclick: () => { unreadOnly = !unreadOnly; render(); } }, unreadOnly ? "显示全部" : "仅未读"))];
  if (!items.length) out.push(el("div", { class: "empty", text: "阅读列表为空。在分享菜单中选择“添加到阅读列表”。" }));
  for (const r of items) {
    out.push(item(icon(r.url, 16), r.title, {
      cls: r.read ? "read" : "", title: r.title + "\n" + host(r.url),
      onclick: () => { call("markRead", { id: r.id, read: true }); call("open", { url: r.url }); },
      onclose: () => call("removeReading", { id: r.id }) }));
  }
  return out;
}

function render() {
  if (!state) return;
  document.querySelectorAll("#seg button").forEach((b) => b.classList.toggle("on", b.dataset.seg === seg));
  const c = document.getElementById("content");
  c.replaceChildren(...(seg === "tabs" ? renderTabs() : seg === "bookmarks" ? renderBookmarks() : renderReading()));
}

async function load() { state = await call("getSidebar"); render(); }
document.querySelectorAll("#seg button").forEach((b) => b.addEventListener("click", () => { seg = b.dataset.seg; sessionStorage.setItem("seg", seg); render(); }));
ULB.on("tabs", (tabs) => { if (state) { state.tabs = tabs; render(); } });
ULB.on("library", load);
ULB.on("segment", (s) => { seg = s; sessionStorage.setItem("seg", seg); render(); });
load();
