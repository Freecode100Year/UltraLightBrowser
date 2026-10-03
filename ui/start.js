"use strict";
const { el, icon, call, host, modal } = ULB;

function siteTile(item) {
  const a = el("a", { class: "site", href: item.url, title: item.title + "\n" + item.url },
    icon(item.url, 62),
    el("span", { class: "name", text: item.title || host(item.url) }));
  a.append(el("button", { class: "rm", title: "从个人收藏中移除", onclick: async (e) => {
    e.preventDefault(); e.stopPropagation();
    await call("removeBookmark", { id: item.id });
    load();
  } }, "✕"));
  return a;
}

async function addFavorite() {
  const r = await modal("添加到个人收藏", [
    { name: "title", label: "名称", placeholder: "例如：维基百科" },
    { name: "url", label: "网址", placeholder: "https://", required: true, type: "text" }]);
  if (!r || !r.url) return;
  await call("addBookmark", { title: r.title, url: ULB.normalizeUrl(r.url), folder: "个人收藏" });
  load();
}

async function load() {
  const s = await call("getStart");
  document.body.className = "bg-" + (s.background || "aurora");
  const show = s.show || {};
  const fav = document.getElementById("fav-grid");
  fav.replaceChildren(...s.favorites.map((f) => siteTile(f)),
    el("a", { class: "site add", href: "#", onclick: (e) => { e.preventDefault(); addFavorite(); } },
      el("span", { class: "tile", style: { width: "62px", height: "62px", fontSize: "28px", borderRadius: "14px" } }, "+"),
      el("span", { class: "name", text: "添加" })));
  document.getElementById("favorites").classList.toggle("hidden", show.favorites === false);

  const rr = document.getElementById("reading-row");
  rr.replaceChildren(...s.reading.map((r) => el("a", { class: "card read-card", href: r.url, onclick: () => call("markRead", { id: r.id, read: true }) },
    el("b", { text: r.title }), el("div", { class: "muted", style: { fontSize: "12px", marginTop: "6px" }, text: host(r.url) }))));
  document.getElementById("reading").classList.toggle("hidden", show.reading === false || !s.reading.length);
}


ULB.on("library", load);
load().catch((e) => document.body.append(el("p", { class: "empty", text: "起始页加载失败：" + e.message })));
