"use strict";
const { el, call, icon } = ULB;
(async () => {
  const r = await call("getPrivacy");
  document.getElementById("hero").replaceChildren(
    el("div", { class: "shield" }, "🛡"),
    el("div", {}, el("div", { class: "big", text: r.total.toLocaleString("zh-CN") }),
      el("div", { class: "muted", text: `在过去七天中，UltraLightBrowser 已阻止 ${r.total} 个跟踪器为你建立档案 · ${r.siteCount} 个网站曾联系跟踪器` }),
      r.adblock ? null : el("div", { class: "off", text: "内容拦截器当前已停用" })));
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
      el("span", { class: "cnt", text: isSite ? `${t.count} 个跟踪器` : `${t.count} 次` }))));
  };
  fill("trackers", r.trackers, false);
  fill("sites", r.sites, true);
})();
