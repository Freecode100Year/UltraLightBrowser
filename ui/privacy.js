"use strict";
const { el, call, icon } = ULB;
(async () => {
  const r = await call("getPrivacy");
  document.getElementById("hero").replaceChildren(
    el("div", { class: "shield" }, "🛡"),
    el("div", {}, el("div", { class: "big", text: r.total.toLocaleString("zh-CN") }),
      el("div", { class: "muted", text: `过去 7 天拦截的跟踪器请求 · 涉及 ${r.siteCount} 个网站` }),
      r.adblock ? null : el("div", { class: "off", text: "广告与跟踪拦截当前已关闭" })));
  const max = Math.max(1, ...r.perDay.map((d) => d[1]));
  document.getElementById("chart").replaceChildren(...r.perDay.map(([day, n]) => {
    const d = new Date(day + "T00:00:00");
    return el("div", { class: "bar", title: `${day}：${n}` },
      el("span", { text: n ? String(n) : "" }),
      el("i", { style: { height: Math.round((n / max) * 110) + "px" } }),
      el("span", { text: `${d.getMonth() + 1}/${d.getDate()}` }));
  }));
  const fill = (id, items, isSite) => {
    const box = document.getElementById(id);
    if (!items.length) { box.replaceChildren(el("div", { class: "empty", text: "暂无数据" })); return; }
    box.replaceChildren(...items.map((t) => el("div", { class: "item" },
      icon("https://" + t.name + "/", 16),
      el("div", { class: "t" }, el("div", { text: t.name })),
      el("span", { class: "cnt", text: isSite ? `${t.count} 次拦截` : `${t.count} 次` }))));
  };
  fill("trackers", r.trackers, false);
  fill("sites", r.sites, true);
})();
