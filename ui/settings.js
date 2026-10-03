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
      row("UltraLightBrowser 打开时：", select("startupPage", [["home", "主页"], ["start", "个人收藏"]])),
      row("新标签页打开方式：", select("newTabPage", [["home", "主页"], ["start", "个人收藏"], ["blank", "空白页"]])),
      row("主页：", text("homeUrl", "https://"))),
    section("tabs", "标签页",
      row("后台标签页自动挂起：", select("tabSuspendMinutes", [[0, "永不"], [5, "5 分钟后"], [10, "10 分钟后"], [30, "30 分钟后"], [60, "1 小时后"]]),
        "未播放声音的后台标签页会暂停运行以节省内存和电量，切换回来时自动恢复。")),
    section("search", "搜索",
      row("搜索引擎：", select("searchEngine", [["google", "Google"], ["brave", "Brave"], ["bing", "Bing"], ["duckduckgo", "DuckDuckGo"], ["startpage", "Startpage"], ["baidu", "百度"]]))),
    section("start", "起始页",
      row("个人收藏", sw("startShowFavorites")),
      row("阅读列表", sw("startShowReading")),
      row("背景图像：", select("startBackground", [["aurora", "极光"], ["ocean", "海洋"], ["sunset", "日落"], ["plain", "无"]]))),
    section("privacy", "隐私",
      row("记录浏览历史", sw("saveHistory"), "历史记录只保存在内存中，关闭 UltraLightBrowser 后即被移除。无痕浏览窗口从不记录。"),
      row("启用内容拦截器", sw("enableAdBlock"), "阻止广告和跨网站跟踪器。可在“此网站的设置”中对单个网站停用。"),
      action("历史记录：", "清除历史记录…", async () => {
        if (confirm("清除所有历史记录？此操作无法撤销。")) { await call("clearHistory", { range: "all" }); toast("已清除历史记录"); }
      }, true),
      action("网站设置：", `移除 ${s.siteCount} 个网站的设置…`, async () => {
        if (confirm("移除所有网站的设置（页面缩放、摄像头、麦克风、位置、弹出式窗口、阅读器）？")) { await call("clearSiteSettings"); toast("已移除网站设置"); load(); }
      }, true),
      row("关闭时移除：", el("span", { class: "muted", text: "历史记录、Cookie 和网站数据、网页缓存、下载列表、网站设置、网站图标、系统 DNS 缓存" }),
        "关闭 UltraLightBrowser 时自动执行，意外退出后会在下次启动时补做。书签、阅读列表和标签组会保留。")),
    section("reader", "阅读器",
      row("主题：", select("readerTheme", [["light", "白色"], ["sepia", "米色"], ["gray", "灰色"], ["dark", "夜间"]])),
      row("字体：", select("readerFont", [["serif", "宋体（衬线）"], ["sans", "黑体（无衬线）"]])),
      row("文本大小：", select("readerFontSize", [15, 17, 19, 21, 24, 28].map((n) => [n, n + " 像素"])))),
    section("advanced", "高级",
      row("使用硬件加速", sw("hardwareAcceleration"), "更改后需重新启动 UltraLightBrowser。"),
      row("预先载入链接", sw("preloadLinks"), "指针停在同一网站的链接上时提前下载该网页，点按后打开更快。对新标签页生效；macOS 用户代理下自动停用。"),
      row("版本：", el("span", { class: "muted", text: "UltraLightBrowser " + s.version })),
      row("项目主页：", el("a", { href: "https://github.com/Freecode100Year/UltraLightBrowser", style: { color: "var(--accent)" }, text: "github.com/Freecode100Year/UltraLightBrowser" }))));
  if (location.hash) document.querySelector(location.hash)?.scrollIntoView();
}
load();
