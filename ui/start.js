"use strict";
const { el, icon, call, host, modal } = ULB;

function siteTile(item, removable) {
  const a = el("a", { class: "site", href: item.url, title: item.title + "\n" + item.url },
    icon(item.url, 62),
    el("span", { class: "name", text: item.title || host(item.url) }));
  if (removable) {
    a.append(el("button", { class: "rm", title: "移除", onclick: async (e) => {
      e.preventDefault(); e.stopPropagation();
      await call("removeBookmark", { id: item.id });
      load();
    } }, "✕"));
  }
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
  fav.replaceChildren(...s.favorites.map((f) => siteTile(f, true)),
    el("a", { class: "site add", href: "#", onclick: (e) => { e.preventDefault(); addFavorite(); } },
      el("span", { class: "tile", style: { width: "62px", height: "62px", fontSize: "28px", borderRadius: "14px" } }, "+"),
      el("span", { class: "name", text: "添加" })));
  document.getElementById("favorites").classList.toggle("hidden", show.favorites === false);

  const freq = document.getElementById("freq-grid");
  freq.replaceChildren(...s.frequent.map((f) => siteTile(f, false)));
  document.getElementById("frequent").classList.toggle("hidden", show.frequent === false || !s.frequent.length);

  const p = s.privacy;
  const pr = document.getElementById("privacy-row");
  pr.replaceChildren(
    el("a", { class: "card pr pr-card", href: "privacy.html" },
      el("div", { class: "shield" }, shieldSvg()),
      el("div", {}, el("div", { class: "big", text: p.total.toLocaleString("zh-CN") }),
        el("div", { class: "muted", style: { fontSize: "12px" }, text: p.adblock
          ? `过去 7 天拦截的跟踪器 · 涉及 ${p.siteCount} 个网站`
          : "广告与跟踪拦截已关闭" }))),
    el("div", { class: "card rl" }, p.top.length
      ? p.top.slice(0, 3).map((t) => el("div", {}, el("span", { text: t.name }), el("span", { class: "muted", text: t.count + " 次" })))
      : el("div", { class: "muted" }, "暂无拦截记录")));
  document.getElementById("privacy").classList.toggle("hidden", show.privacy === false);

  const rr = document.getElementById("reading-row");
  rr.replaceChildren(...s.reading.map((r) => el("a", { class: "card read-card", href: r.url, onclick: () => call("markRead", { id: r.id, read: true }) },
    el("b", { text: r.title }), el("div", { class: "muted", style: { fontSize: "12px", marginTop: "6px" }, text: host(r.url) }))));
  document.getElementById("reading").classList.toggle("hidden", show.reading === false || !s.reading.length);
}

function shieldSvg() {
  const ns = "http://www.w3.org/2000/svg";
  const svg = document.createElementNS(ns, "svg");
  svg.setAttribute("viewBox", "0 0 24 24");
  svg.setAttribute("width", "26"); svg.setAttribute("height", "26");
  const path = document.createElementNS(ns, "path");
  path.setAttribute("d", "M12 3l8 3v6c0 5-3.5 8-8 9-4.5-1-8-4-8-9V6z");
  path.setAttribute("fill", "none"); path.setAttribute("stroke", "#fff"); path.setAttribute("stroke-width", "1.8");
  path.setAttribute("stroke-linejoin", "round");
  svg.append(path);
  return svg;
}

ULB.on("library", load);
load().catch((e) => document.body.append(el("p", { class: "empty", text: "起始页加载失败：" + e.message })));
