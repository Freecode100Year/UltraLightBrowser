"use strict";
const { el, icon, call, host, fmtTime, dayLabel } = ULB;
const list = document.getElementById("list");
const q = document.getElementById("q");
let timer = 0;

async function load() {
  const r = await call("getHistory", { q: q.value, limit: 1000 });
  document.getElementById("off").style.display = r.enabled ? "none" : "block";
  if (!r.items.length) {
    list.replaceChildren(el("div", { class: "empty", text: q.value ? "没有找到匹配的记录" : "没有历史记录" }));
    return;
  }
  const out = [];
  let lastDay = "";
  for (const h of r.items) {
    const d = dayLabel(h.last);
    if (d !== lastDay) { out.push(el("div", { class: "day", text: d })); lastDay = d; }
    out.push(el("a", { class: "item", href: h.url },
      el("span", { class: "time", text: fmtTime(h.last) }),
      icon(h.url, 16),
      el("div", { class: "t" }, el("div", { text: h.title || h.url }), el("div", { class: "url", text: host(h.url) })),
      el("button", { class: "icon x", title: "删除", onclick: async (e) => {
        e.preventDefault(); e.stopPropagation();
        await call("deleteHistory", { url: h.url });
        load();
      } }, "✕")));
  }
  list.replaceChildren(...out);
}

q.addEventListener("input", () => { clearTimeout(timer); timer = setTimeout(load, 150); });
document.getElementById("range").addEventListener("change", async (e) => {
  const range = e.target.value;
  e.target.value = "";
  if (!range) return;
  const label = e.target.querySelector(`option[value="${range}"]`).textContent;
  if (!confirm(`清除“${label}”的历史记录？\n这会移除相关的历史记录项目。此操作无法撤销。`)) return;
  await call("clearHistory", { range });
  load();
});
load();
