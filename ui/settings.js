"use strict";
const { el, call, toast } = ULB;
let s = {};

const set = async (key, value) => {
  s[key] = value;
  await call("setSetting", { key, value });
};

function sw(key) {
  const input = el("input", { type: "checkbox", checked: !!s[key], onchange: (e) => set(key, e.target.checked) });
  return el("label", { class: "switch" }, input, el("span"));
}
function select(key, options) {
  return el("select", { onchange: (e) => set(key, isNaN(+e.target.value) || e.target.value === "" ? e.target.value : +e.target.value) },
    options.map(([v, t]) => el("option", { value: v, selected: String(s[key]) === String(v) }, t)));
}
function text(key, placeholder) {
  return el("input", { type: "text", value: s[key] || "", placeholder, onchange: (e) => set(key, e.target.value.trim()) });
}
function row(label, control, hint) {
  return el("div", { class: "setting" }, el("div", { class: "label" }, label, hint ? el("small", { text: hint }) : null), control);
}
function section(id, title, ...rows) {
  return el("section", { id }, el("h2", { text: title }), el("div", { class: "card" }, ...rows));
}
function action(label, text, fn, danger) {
  return row(label, el("button", { class: danger ? "danger" : "", onclick: fn }, text));
}

async function load() {
  s = await call("getSettings");
  document.getElementById("main").replaceChildren(
    section("general", "通用",
      row("启动时打开", select("startupPage", [["start", "起始页"], ["home", "主页"], ["restore", "上次打开的标签页"]])),
      row("新标签页打开", select("newTabPage", [["start", "起始页"], ["blank", "空白页"], ["home", "主页"]])),
      row("主页", text("homeUrl", "https://")),
      row("搜索引擎", select("searchEngine", [["google", "Google"], ["bing", "Bing"], ["duckduckgo", "DuckDuckGo"], ["startpage", "Startpage"], ["baidu", "百度"]]))),
    section("tabs", "标签页",
      row("后台标签页自动挂起", select("tabSuspendMinutes", [[0, "从不"], [5, "5 分钟后"], [10, "10 分钟后"], [30, "30 分钟后"], [60, "1 小时后"]]),
        "未播放声音的后台标签页会暂停运行以节省内存和电量，切换回来时自动恢复。")),
    section("start", "起始页",
      row("个人收藏", sw("startShowFavorites")),
      row("常去网站", sw("startShowFrequent")),
      row("隐私报告", sw("startShowPrivacy")),
      row("阅读列表", sw("startShowReading")),
      row("背景", select("startBackground", [["aurora", "极光"], ["ocean", "海洋"], ["sunset", "日落"], ["plain", "纯色"]]))),
    section("privacy", "隐私",
      row("记录浏览历史", sw("saveHistory"), "无痕窗口从不记录历史。"),
      row("退出时清除历史记录", sw("clearHistoryOnExit")),
      row("广告与跟踪拦截", sw("enableAdBlock"), "可在“此网站的设置”中对单个网站关闭。"),
      action("历史记录", "清除所有历史记录…", async () => {
        if (confirm("清除所有历史记录？")) { await call("clearHistory", { range: "all" }); toast("已清除历史记录"); }
      }, true),
      action("网站设置", `清除 ${s.siteCount} 个网站的设置…`, async () => {
        if (confirm("清除所有网站的单独设置（缩放、权限、阅读器等）？")) { await call("clearSiteSettings"); toast("已清除网站设置"); load(); }
      }, true),
      action("隐私报告", "清除统计数据…", async () => {
        if (confirm("清除隐私报告统计数据？")) { await call("clearPrivacy"); toast("已清除"); }
      }, true),
      row("Cookie 与网站数据", el("span", { class: "muted", text: "每次退出时自动清除" }))),
    section("reader", "阅读器",
      row("主题", select("readerTheme", [["sepia", "米黄"], ["light", "白色"], ["gray", "灰色"], ["dark", "深色"]])),
      row("字体", select("readerFont", [["serif", "衬线（宋体）"], ["sans", "无衬线（黑体）"]])),
      row("字号", select("readerFontSize", [15, 17, 19, 21, 24, 28].map((n) => [n, n + " px"])))),
    section("advanced", "高级",
      row("硬件加速", sw("hardwareAcceleration"), "更改后重新启动浏览器生效。"),
      row("版本", el("span", { class: "muted", text: "UltraLightBrowser " + s.version })),
      row("项目主页", el("a", { href: "https://github.com/Freecode100Year/UltraLightBrowser", style: { color: "var(--accent)" }, text: "github.com/Freecode100Year/UltraLightBrowser" }))));
  if (location.hash) document.querySelector(location.hash)?.scrollIntoView();
}
load();
