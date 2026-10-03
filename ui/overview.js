"use strict";
const { el, call, icon, host } = ULB;
let tabs = [];
const q = document.getElementById("q");

function render() {
  const term = q.value.trim().toLowerCase();
  const shown = term ? tabs.filter((t) => (t.title + " " + t.url).toLowerCase().includes(term)) : tabs;
  const grid = document.getElementById("grid");
  grid.replaceChildren(...shown.map((t) => {
    const shot = el("div", { class: "shot" });
    if (t.thumb) shot.style.backgroundImage = `url("${t.thumb}")`;
    else shot.append(icon(t.url || "about:blank", 48, t.favicon));
    return el("div", { class: "th" + (t.active ? " act" : ""), title: t.title + "\n" + t.url, onclick: () => call("activateTab", { id: t.id }) },
      shot,
      el("div", { class: "tt" }, icon(t.url || "about:blank", 16, t.favicon), el("span", { class: "name", text: t.title || host(t.url) || "新标签页" }),
        t.audio ? el("span", { class: "badge audio", text: "播放中" }) : null,
        t.suspended ? el("span", { class: "badge sleep", text: "已挂起" }) : null),
      el("button", { class: "close", title: "关闭此标签页", onclick: (e) => { e.stopPropagation(); call("closeTab", { id: t.id }); } }, "✕"));
  }), el("div", { class: "th new", title: "新建标签页", onclick: () => call("newTab") }, "+"));
}

async function load() { tabs = await call("getTabs"); render(); }
q.addEventListener("input", render);
document.addEventListener("keydown", (e) => {
  if (e.key === "Escape") call("closeOverview");
  if (e.key === "Enter") {
    const term = q.value.trim().toLowerCase();
    const hit = tabs.find((t) => (t.title + " " + t.url).toLowerCase().includes(term));
    if (hit) call("activateTab", { id: hit.id });
  }
});
ULB.on("tabs", (data) => { tabs = data; render(); });
ULB.on("focus", () => { q.value = ""; q.focus(); load(); });
load();
