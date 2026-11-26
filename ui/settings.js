"use strict";
const { el, call, toast } = ULB;
let s = {};

const set = async (key, value) => {
  try {
    await call("setSetting", { key, value });
    s[key] = value;
    if (key === "networkMode") load();
  } catch (e) {
    toast(e.message);
    load();
  }
};

function sw(key, disabled) {
  const input = el("input", { type: "checkbox", checked: !!s[key] && !disabled, disabled: !!disabled, onchange: (e) => set(key, e.target.checked) });
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

// ---------------------------------------------------------------- network

let L = { lines: [], subs: [], selected: "public", autoMax: 20 };
const probe = {};  // line id -> { ms } | { error } | { busy: true }
const restartNote = "重新启动 UltraLightBrowser 后生效";

function networkSection() {
  const lineName = (id) => id === "public" ? "内置公共线路" : id === "auto" ? "自动选择" : (L.lines.find((l) => l.id === id) || {}).name || "";
  return section("network", "网络",
    row("网络线路：", select("networkMode", [["direct", "直接连接"], ["warp", "Cloudflare WARP"], ["line", "我的线路"]]),
      "WARP：所有网页经 Cloudflare 加密隧道访问，网站看到的是 Cloudflare 的地址。我的线路：经你选择的服务器访问，可穿过网络封锁，" +
      "断开时一律不直接连接。当前状态：" + s.warpStatus + "。更改后需重新启动。"),
    row("我的线路使用：", el("select", { onchange: async (e) => {
        try { await call("selectLine", { id: e.target.value }); toast("已改用“" + lineName(e.target.value) + "”，" + restartNote); load(); }
        catch (err) { toast(err.message); load(); }
      } },
      el("option", { value: "public", selected: L.selected === "public" }, "内置公共线路"),
      el("option", { value: "auto", selected: L.selected === "auto", disabled: !L.lines.length }, `自动选择（前 ${L.autoMax} 条中最快的）`),
      L.lines.map((l) => el("option", { value: l.id, selected: L.selected === l.id }, l.name))),
      "内置公共线路需要 IPv6 网络。自动选择：启动时同时测试，用最快的一条；它断开时自动换到另一条，并每小时重新比较一次。"),
    s.lineMode ? null : action("WARP 入口：", "重新优选 IP", async () => {
      if (!(await call("warpRescan"))) { toast("WARP 未在此窗口进程中运行，无法优选"); return; }
      toast("正在优选 WARP 入口 IP，约需 5 秒"); setTimeout(load, 6000);
    }),
    row("WARP 断开时阻止联网", sw("warpFailClosed", s.networkMode !== "warp"), "开启后，WARP 无法连接时不会改用直接连接，避免暴露真实 IP。使用我的线路时始终阻止。更改后需重新启动。"));
}

function latency(id) {
  const p = probe[id];
  if (!p) return null;
  if (p.busy) return el("span", { class: "lat muted", text: "测速中…" });
  if (p.error) return el("span", { class: "lat bad", title: p.error, text: "不可用" });
  return el("span", { class: "lat " + (p.ms < 400 ? "good" : p.ms < 1000 ? "mid" : "slow"), text: p.ms + " ms" });
}

async function runProbe(ids) {
  ids = ids.slice(0, 64);
  if (!ids.length) return;
  ids.forEach((id) => (probe[id] = { busy: true }));
  render();
  try {
    const r = await call("probeLines", { ids });
    for (const id of ids) probe[id] = r[id] || { error: "超时" };
  } catch (e) {
    ids.forEach((id) => (probe[id] = { error: e.message }));
  }
  render();
}

function linesSection() {
  const subName = (id) => (L.subs.find((x) => x.id === id) || {}).name;
  const tools = el("div", { class: "row", style: { padding: "4px 4px 12px", flexWrap: "wrap" } },
    el("button", { class: "primary", onclick: importLinks }, "导入链接…"),
    el("button", { onclick: addSub }, "添加订阅…"),
    el("button", { disabled: !L.lines.length, onclick: () => runProbe(L.lines.map((l) => l.id)) }, "全部测速"),
    el("span", { class: "spacer" }),
    el("small", { class: "muted", text: L.lines.length ? `共 ${L.lines.length} 条` : "" }));
  const subs = L.subs.map((x) => el("div", { class: "item" },
    el("div", { class: "t" }, el("div", { text: "订阅：" + x.name }),
      el("small", { class: "muted", text: `${x.count} 条线路 · ` + (x.updated ? "更新于 " + new Date(x.updated).toLocaleString("zh-CN") : "尚未更新") })),
    el("button", { onclick: async (e) => {
      e.target.disabled = true; e.target.textContent = "更新中…";
      try { const r = await call("updateSub", { id: x.id }); toast(`已更新：${r.count} 条` + (r.skipped ? `，${r.skipped} 条格式不支持已跳过` : "")); }
      catch (err) { toast(err.message); }
      load();
    } }, "更新"),
    el("button", { class: "danger", onclick: async () => {
      if (confirm(`删除订阅“${x.name}”和它的 ${x.count} 条线路？`)) { await call("removeSub", { id: x.id }); toast("已删除订阅"); load(); }
    } }, "删除")));
  const items = L.lines.map((l) => el("div", { class: "item" + (L.selected === l.id ? " current" : "") },
    el("div", { class: "t" },
      el("div", {}, l.name, L.selected === l.id ? el("span", { class: "tag", text: "使用中" }) : null),
      el("small", { class: "muted", text: [l.proto, l.server, l.sub ? "来自 " + (subName(l.sub) || "订阅") : ""].filter(Boolean).join(" · ") })),
    latency(l.id),
    el("button", { class: "x", onclick: () => runProbe([l.id]) }, "测速"),
    L.selected === l.id ? null : el("button", { class: "x", onclick: async () => {
      await call("selectLine", { id: l.id }); toast("已改用“" + l.name + "”，" + restartNote); load();
    } }, "使用"),
    el("button", { class: "x", onclick: async () => {
      const r = await ULB.modal("重命名线路", [{ name: "name", label: "名称", value: l.name }]);
      if (r) { await call("renameLine", { id: l.id, name: r.name }); load(); }
    } }, "重命名"),
    el("button", { class: "x danger", onclick: async () => {
      if (confirm(`删除线路“${l.name}”？`)) { await call("removeLine", { id: l.id }); delete probe[l.id]; load(); }
    } }, "删除")));
  return el("section", { id: "lines" }, el("h2", { text: "我的线路" }),
    el("div", { class: "card" }, tools,
      el("div", { class: "list" }, subs, items),
      L.lines.length || L.subs.length ? null : el("div", { class: "empty", text: "还没有线路。点“导入链接…”粘贴分享链接，或“添加订阅…”。" }),
      el("small", { class: "muted", style: { display: "block", padding: "10px 4px 0" },
        text: "支持 vless://、trojan://、vmess://、ss:// 分享链接，传输方式 TCP / WebSocket / HTTPUpgrade / XHTTP / gRPC，安全 TLS / REALITY。" +
          "也支持 hysteria2:// / hy2://（可带 Salamander 混淆）和 gRPC 传输。暂不支持 TUIC、跳过证书验证的链接。线路和订阅经 Windows 加密保存在本机，只有当前 Windows 账户能读取。" + restartNote + "。" })));
}

async function importLinks() {
  const r = await ULB.modal("导入线路链接", [{ name: "text", label: "每行一条分享链接（vless:// trojan:// vmess:// ss:// hysteria2:// hy2://）", multiline: true, rows: 8, required: true }], "导入");
  if (!r || !r.text) return;
  toast("正在检查链接…");
  try {
    const res = await call("addLines", { text: r.text });
    let msg = `已导入 ${res.added} 条`;
    if (res.duplicate) msg += `，${res.duplicate} 条已存在`;
    if (res.invalid.length) {
      msg += `，${res.invalid.length} 条无法使用`;
      alert("以下链接无法使用：\n\n" + res.invalid.map((i) => `• ${i.name}：${i.reason}`).join("\n"));
    }
    toast(msg);
  } catch (e) { toast(e.message); }
  load();
}

async function addSub() {
  const r = await ULB.modal("添加订阅", [
    { name: "url", label: "订阅地址（https://…，通用 / v2rayN 格式）", type: "url", required: true, placeholder: "https://" },
    { name: "name", label: "名称（可不填）" }], "添加");
  if (!r || !r.url) return;
  toast("正在下载订阅…");
  try {
    const res = await call("addSub", { url: r.url, name: r.name });
    toast(`已添加 ${res.count} 条线路` + (res.skipped ? `，${res.skipped} 条格式不支持已跳过` : ""));
  } catch (e) { toast(e.message); }
  load();
}

// ---------------------------------------------------------------- extensions & scripts

let X = { list: [], private: false };
let R = [];  // 推荐扩展
const busy = new Set();  // recommended extensions being downloaded
let U = [];

function toggle(checked, onchange) {
  return el("label", { class: "switch" }, el("input", { type: "checkbox", checked, onchange: (e) => onchange(e.target.checked) }), el("span"));
}

function extensionsSection() {
  const items = X.list.map((x) => el("div", { class: "item" },
    el("div", { class: "t" },
      el("div", {}, x.name, el("span", { class: "muted", text: "  " + x.version })),
      el("small", { class: x.error ? "bad" : "muted", text: x.error || x.description || "" }),
      x.proxy ? el("small", { class: "warn", text: "⚠ 会接管网络代理：开着它时 ULB 的 WARP / 我的线路不生效" }) : null),
    x.popup ? el("button", { class: "x", disabled: !x.loaded || !x.enabled, onclick: () => openExt(x, "popup") }, "打开") : null,
    x.options ? el("button", { class: "x", disabled: !x.loaded || !x.enabled, onclick: () => openExt(x, "options") }, "选项") : null,
    el("button", { class: "x danger", onclick: async () => {
      if (confirm(`删除扩展“${x.name}”？`)) { await call("removeExtension", { dir: x.dir }); toast("已删除"); load(); }
    } }, "删除"),
    toggle(x.enabled, async (on) => { await call("setExtensionEnabled", { dir: x.dir, enabled: on }); load(); })));
  return el("section", { id: "extensions" }, el("h2", { text: "扩展程序" }),
    el("div", { class: "card" },
      el("div", { class: "row", style: { padding: "4px 4px 12px", flexWrap: "wrap" } },
        el("button", { class: "primary", onclick: () => installExt("installExtensionFolder") }, "加载已解压的扩展…"),
        el("button", { onclick: () => installExt("installExtensionFile") }, "安装 .crx / .zip…")),
      el("div", { class: "list" }, items),
      X.list.length ? null : el("div", { class: "empty", text: "还没有扩展。可加载 Chrome / Edge 扩展：解压后的文件夹，或 .crx / .zip 安装包。" }),
      recommendedCard(),
      el("small", { class: "muted", style: { display: "block", padding: "10px 4px 0" },
        text: "扩展只在普通窗口运行，不在无痕窗口运行。为保护隐私，关闭浏览器时会清除扩展自己保存的设置和数据（扩展本身保留）。" +
          "只安装你信任的扩展：扩展能读取和修改你打开的网页。部分扩展用到 WebView2 不支持的功能，可能无法加载。" })));
}

// 推荐扩展: downloaded from the official source when asked, checked, then installed.
function recommendedCard() {
  if (!R.length) return null;
  const installed = new Set(X.list.map((x) => x.source).filter(Boolean));
  const items = R.map((r) => el("div", { class: "item rec" },
    el("div", { class: "t" },
      el("div", {}, r.name, installed.has(r.key) ? el("span", { class: "tag", text: "已安装" }) : null),
      el("small", { class: "muted wrap", text: r.summary }),
      r.note ? el("small", { class: r.proxy ? "warn wrap" : "muted wrap", text: (r.proxy ? "⚠ " : "提示：") + r.note }) : null),
    el("a", { class: "more", href: r.homepage, target: "_blank", text: "了解更多" }),
    el("button", { class: installed.has(r.key) ? "" : "primary", disabled: busy.has(r.key) || X.private, onclick: () => installRecommended(r, installed.has(r.key)) },
      busy.has(r.key) ? "正在下载…" : installed.has(r.key) ? "更新" : "安装")));
  return el("div", { style: { marginTop: "14px" } },
    el("h3", { text: "推荐扩展" }),
    el("small", { class: "muted wrap", style: { padding: "0 4px 6px" },
      text: "点「安装」时才从 Chrome 网上应用店下载，并校验开发者签名，确认是原版后再安装。下载走 ULB 当前的线路。" }),
    el("div", { class: "list" }, items),
    el("small", { class: "muted wrap", style: { padding: "6px 4px 0" },
      text: "想用油猴 / 篡改猴脚本？不用装扩展：用下面的「用户脚本」（ULB 自带）导入 .user.js 即可。WebView2 加载不了篡改猴、暴力猴这类脚本管理扩展。" }));
}

async function installRecommended(r, update) {
  if (r.proxy && !update && !confirm(`${r.name}\n\n${r.note}\n\n仍要安装吗？`)) return;
  busy.add(r.key);
  const y = scrollY;
  build();
  scrollTo(0, y);
  try {
    await call("downloadRecommendedExtension", { key: r.key });
    const res = await call("installRecommendedExtension", { key: r.key });
    toast("已安装“" + res.installed + "”。刷新网页后生效");
  } catch (e) { toast(e.message); }
  busy.delete(r.key);
  load();
}

async function installExt(cmd) {
  try {
    const r = await call(cmd);
    if (r) toast("已安装“" + r.installed + "”。刷新网页后生效");
  } catch (e) { toast(e.message); }
  load();
}

async function openExt(x, page) {
  try { await call("openExtensionPage", { dir: x.dir, page }); } catch (e) { toast(e.message); }
}

const runAtText = { "document-start": "网页开始加载时", "document-body": "网页正文出现时", "document-end": "网页加载完成时", "document-idle": "网页加载完成后" };

function scriptsSection() {
  const items = U.map((sc) => el("div", { class: "item" },
    el("div", { class: "t" },
      el("div", {}, sc.name, sc.version ? el("span", { class: "muted", text: "  " + sc.version }) : null),
      el("small", { class: "muted", title: sc.sites.join("\n"),
        text: (sc.description ? sc.description + " · " : "") + "运行于 " + sc.sites.slice(0, 3).join("、") + (sc.sites.length > 3 ? ` 等 ${sc.sites.length} 条规则` : "") +
          " · " + (runAtText[sc.runAt] || "") + (sc.ignored.length ? " · 不支持 " + sc.ignored.join(" ") : "") })),
    el("button", { class: "x danger", onclick: async () => {
      if (confirm(`删除脚本“${sc.name}”？`)) { await call("removeScript", { id: sc.id }); toast("已删除，刷新网页后生效"); load(); }
    } }, "删除"),
    toggle(sc.enabled, async (on) => { await call("setScriptEnabled", { id: sc.id, enabled: on }); toast("刷新网页后生效"); load(); })));
  return el("section", { id: "scripts" }, el("h2", { text: "用户脚本" }),
    el("div", { class: "card" },
      el("div", { class: "row", style: { padding: "4px 4px 12px", flexWrap: "wrap" } },
        el("button", { class: "primary", onclick: importScript }, "导入脚本文件…"),
        el("button", { onclick: pasteScript }, "粘贴脚本…")),
      el("div", { class: "list" }, items),
      U.length ? null : el("div", { class: "empty", text: "还没有脚本。可导入油猴（Tampermonkey）.user.js 脚本，或普通 JavaScript。" }),
      el("small", { class: "muted", style: { display: "block", padding: "10px 4px 0" },
        text: "支持 @match、@include、@exclude、@run-at、@noframes，以及常用的 GM_addStyle、GM_getValue / GM_setValue、GM_xmlhttpRequest、GM_openInTab 等。" +
          "不支持 @require 和 @resource（不会从网上下载代码）。GM_xmlhttpRequest 受网站跨域限制。脚本在普通和无痕窗口都会运行。只导入你信任的脚本。" })));
}

async function askSites(name, code) {
  const r = await ULB.modal("脚本“" + name + "”在哪些网站运行？", [
    { name: "sites", label: "网址规则，例如 https://example.com/* 或 *://*.example.com/*；*://*/* 表示所有网站", value: "*://*/*", required: true }], "安装");
  if (!r) return;
  try { await call("installScript", { code, name, sites: r.sites }); toast("已安装，刷新网页后生效"); }
  catch (e) { toast(e.message); }
  load();
}

async function importScript() {
  try {
    const r = await call("importScriptFile");
    if (!r) return;
    if (r.needSites) { await askSites(r.name, r.code); return; }
    toast("已安装“" + r.installed + "”，刷新网页后生效" + (r.ignored.length ? "（" + r.ignored.join(" ") + " 不支持，已忽略）" : ""));
  } catch (e) { toast(e.message); }
  load();
}

async function pasteScript() {
  const r = await ULB.modal("粘贴脚本", [{ name: "code", label: "油猴脚本（带 ==UserScript== 头）或普通 JavaScript", multiline: true, rows: 12, required: true }], "下一步");
  if (!r || !r.code) return;
  if (!/==UserScript==/.test(r.code)) { await askSites("粘贴的脚本", r.code); return; }
  try { await call("installScript", { code: r.code, name: "", sites: "" }); toast("已安装，刷新网页后生效"); }
  catch (e) { toast(e.message); }
  load();
}

let render = () => {};

async function load() {
  s = await call("getSettings");
  L = await call("getLines");
  X = await call("getExtensions");
  if (!R.length) R = await call("getRecommendedExtensions");
  U = await call("getScripts");
  const y = scrollY;
  build();
  if (rendered) scrollTo(0, y);
}

function build() {
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
    networkSection(),
    linesSection(),
    extensionsSection(),
    scriptsSection(),
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
      row("显卡视频增强", sw("videoEnhancement", !s.hardwareAcceleration),
        "让显卡在播放视频时做超分辨率（NVIDIA RTX Video Super Resolution、Intel 显卡超分），NVIDIA 显卡另可把 SDR 视频转成 HDR（RTX Video HDR）。" +
        "还需在 NVIDIA App →「系统 → 视频」里开启对应功能；HDR 还需 HDR 显示器并打开 Windows HDR。只在画面被放大且电脑接着电源时起作用，DRM 视频可能不支持。更改后需重新启动 UltraLightBrowser。"),
      row("预先载入链接", sw("preloadLinks", !s.preloadAvailable), "指针停在同一网站的链接上时提前下载该网页，点按后打开更快。对新标签页生效。" +
        (s.preloadAvailable ? "" : "当前不可用：使用 WARP 或 macOS 浏览器UA标识时浏览器内核不进行预先载入。")),
      row("版本：", el("span", { class: "muted", text: "UltraLightBrowser " + s.version })),
      row("项目主页：", el("a", { href: "https://github.com/Freecode100Year/UltraLightBrowser", style: { color: "var(--accent)" }, text: "github.com/Freecode100Year/UltraLightBrowser" }))));
  if (location.hash && !rendered) document.querySelector(location.hash)?.scrollIntoView();
  rendered = true;
}
let rendered = false;
render = () => { const y = scrollY; build(); scrollTo(0, y); };
load();
