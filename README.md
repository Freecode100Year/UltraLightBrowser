<p align="center">
  <img src="resources/icon.png" width="128" height="128" alt="UltraLightBrowser">
</p>

<h1 align="center">UltraLightBrowser</h1>

<p align="center">
  极简、轻量的 Windows 原生浏览器 · Win32 + C++20 + Microsoft Edge WebView2
</p>

<p align="center">
  <a href="https://github.com/Freecode100Year/UltraLightBrowser/releases/latest"><img src="https://img.shields.io/github/v/release/Freecode100Year/UltraLightBrowser?color=blue&logo=github" alt="Release"></a>
  <img src="https://img.shields.io/badge/Windows-10%20%7C%2011%20x64-0078d4?logo=windows" alt="Platform">
  <img src="https://img.shields.io/badge/C%2B%2B-20-blue?logo=cplusplus" alt="C++20">
  <a href="LICENSE"><img src="https://img.shields.io/badge/License-MIT-green" alt="MIT"></a>
</p>

<p align="center">
  <img src="docs/screenshot.jpg" width="820" alt="UltraLightBrowser 截图">
</p>

## 简介

UltraLightBrowser 是一个注重隐私的 Windows 浏览器，界面仿照 macOS Safari：紧凑式标签栏（当前标签就是地址栏）、起始页、边栏、阅读器、标签页概览，所有选项集中在右上角一个“⋯”菜单里。

- **隐私优先**：内置 Cloudflare WARP 加密隧道（默认开启），也可改用自己的私人线路（VLESS + REALITY），关闭浏览器即清除全部浏览痕迹，内置广告与跟踪拦截。
- **单文件、免安装**：一个约 21MB 的 exe（浏览器本体约 1.3MB，其余是内置的网络组件：WARP 和私人线路），双击即可运行，不需要管理员权限。
- **系统内核**：界面用原生 Win32 + GDI+ 绘制，网页由 Windows 自带的 Microsoft Edge WebView2（Chromium）渲染，内核随系统自动更新安全补丁，网页兼容性与 Edge 一致。

## 下载

前往 **[Releases 最新版](https://github.com/Freecode100Year/UltraLightBrowser/releases/latest)** 下载：

| 文件 | 说明 |
| :--- | :--- |
| `UltraLightBrowser.exe` | 单文件，双击运行 |
| `UltraLightBrowser-vX.Y.Z-windows-x64.zip` | 便携压缩包 |
| `SHA256SUMS.txt` | 校验值 |

所有发布文件均由 GitHub Actions 从本仓库源码自动编译，并附带构建来源证明（artifact attestation）。下载后建议校验：

```powershell
Get-FileHash .\UltraLightBrowser.exe -Algorithm SHA256   # 与 SHA256SUMS.txt 对比
gh attestation verify .\UltraLightBrowser.exe -R Freecode100Year/UltraLightBrowser
```

运行需要 Windows 10/11 x64 和 [WebView2 Runtime](https://developer.microsoft.com/microsoft-edge/webview2/)（Windows 11 已自带）。部分杀毒软件可能对内置的网络组件误报。

## 隐私

### 内置 Cloudflare WARP（默认开启）

浏览器启动时在后台运行一个用户态 WireGuard 组件（无驱动、无需管理员权限），所有网页经 Cloudflare WARP 加密隧道访问：

- **出口与 DNS**：网站看到的是 Cloudflare 的地址；域名在隧道内由 Cloudflare DNS（1.1.1.1）解析，不经过本机或运营商的 DNS。网站支持 IPv6 时优先走 IPv6。
- **不泄露真实 IP**：
  - WebRTC 只走隧道，网页拿不到本机地址。
  - 在隧道内解析不到的域名直接报错，不会改用本机 DNS 直接连接。
- **IP 优选**：
  - **测速**：向 Cloudflare 的多个 WARP 入口地址段和 50 多个 UDP 端口发送 WireGuard 握手包测延迟，自动切到最快的入口，切换时已打开的连接不中断。
  - **何时重新优选**：每 6 小时、所有入口都连不上时，或手动点“重新优选 IP”。
- **WARP 断开时**：默认改为直接连接，网页照常打开，但网站会看到真实 IP。开启“WARP 断开时阻止联网”后，WARP 不通或组件无法启动时都不联网。
- **免注册**：第一次运行时自动注册免费 WARP 账号，不需要安装 Cloudflare 客户端。
- **自动恢复**：组件随浏览器退出；组件意外退出会自动重启。

设置位置：菜单“⋯ → 网络线路”或“设置 → 隐私 → 网络线路”。切换线路需重新启动浏览器。

需要知道的：
- 账号注册使用 WARP 官方 App 的同一接口（非公开 API），Cloudflare 可能随时更改或限制；失效时按上面的断开规则处理。
- 在中国大陆等网络环境下，WARP 入口和 WireGuard 协议可能被封锁或限速，IP 优选只能尽量寻找可用入口。
- Cloudflare 能看到你访问了哪些网站（加密网页的内容看不到）；网站看到的是 Cloudflare 的地址。
- 开启 WARP 时，“预先载入链接”自动停用（Chromium 内核在使用代理时不进行预先载入）。

### 我的线路（VLESS + REALITY）

在 WARP 被封锁或限速的网络中，可以改用 [Xray](https://github.com/XTLS/Xray-core) 服务器：

- **内置公共线路**：未导入链接时使用作者提供的公共服务器，菜单“⋯ → 网络线路 → 我的线路”选中并重新启动即可，无需任何设置。仅可经 IPv6 连接（所在网络需有 IPv6）；出口经 Cloudflare WARP，网站看不到服务器的真实地址；服务器不记录访问日志，不允许 BT 下载和发送邮件。
- **导入自己的线路**：“设置 → 隐私 → 我的线路 → 导入链接…”，粘贴 `vless://` 链接（仅支持 REALITY，TCP 传输，可带 `flow=xtls-rprx-vision`），再在“网络线路”中选择“我的线路”并重新启动。
- **全部经过线路**：网页和域名解析都由服务器完成；线路不通时网页打不开，**从不改用直接连接**；WebRTC 同样只走线路。
- **链接保存**：用 Windows DPAPI 加密保存在本机，只有当前 Windows 账户能解密；不写入设置文件，可随时“移除”（移除后改用公共线路）。
- **不记录**：浏览器内置的线路组件不输出任何日志。

需要知道的：
- 线路服务器能看到你访问了哪些网站（加密网页的内容看不到），只导入你信任的人提供的链接。
- 服务器地址只有 IPv6 时，本机网络也必须有 IPv6。
- 本项目不提供任何线路或服务器。

### 关闭即清除全部痕迹

关闭浏览器时删除：
- 历史记录、Cookie 和网站数据
- 网页缓存、下载列表
- 网站设置（缩放、权限、弹窗、单站拦截例外、自动阅读器）
- 非书签网站的图标

同时清空系统 DNS 缓存和任务栏“最近”记录。历史记录只保存在内存中，从不写入磁盘。意外退出或被强制结束时，下次启动会先补做清理。下载的文件不记录来源网址（保留 Windows“来自互联网”安全标记）。

留在磁盘上的只有（`%LOCALAPPDATA%\UltraLightBrowser`）：
- 书签、阅读列表、标签页组和设置
- 元素隐藏规则
- 书签网站的图标
- 浏览器内核的加密密钥
- `warp\` 中的 WARP 组件、WARP 账号和优选结果（不含任何浏览记录）

### 拦截

- 内置广告 / 跟踪域名拦截（200 多个常见国内外广告与统计域名），第三方子框架和跟踪像素同样过滤；可对单个网站关闭。
- WebView2 严格防跟踪。
- 元素隐藏：Ctrl+Shift+H 点选页面元素永久隐藏。

## 功能

<p align="center">
  <img src="docs/screenshot-start.jpg" width="400" alt="起始页">
  <img src="docs/screenshot-reader.jpg" width="400" alt="阅读器">
  <img src="docs/screenshot-overview.jpg" width="400" alt="标签页概览">
  <img src="docs/screenshot-sidebar.jpg" width="400" alt="边栏与地址栏建议">
</p>

**标签页与窗口**
- 紧凑式标签栏：当前标签即地址栏（网站域名 + 锁），其他标签显示图标和标题；悬停可关闭，可拖动排序，标签太多时显示“+N”
- 标签页概览（Ctrl+Shift+\）：缩略图网格，可搜索、关闭、新建
- 后台标签页自动挂起（默认 10 分钟，可设置），正在播放声音的标签不挂起；标签显示播放图标，点击即可静音
- 多窗口、无痕浏览窗口（独立的 InPrivate 数据）；重新打开关闭的标签页
- 链接右键菜单：在新标签 / 后台标签 / 无痕浏览窗口中打开，添加到阅读列表

**统一菜单（右上角 ⋯）**
- 新建标签页 / 窗口 / 无痕浏览窗口；书签、历史记录、下载、阅读列表
- 显示阅读器、页内查找、缩放、打印
- 此网站的设置、内容拦截器、声音、Cloudflare WARP、浏览器UA标识、节能
- 全屏、设置、关于

**起始页、书签与历史**
- 启动和新标签页默认打开 Brave 搜索（地址栏保持空白，标签显示“起始页”）；也可改用“个人收藏”页或空白页
- 搜索引擎：Google（默认）、Brave、Bing、DuckDuckGo、Startpage、百度
- 书签：文件夹、拖动排序、搜索；可导入 / 导出 HTML 书签（Chrome、Edge、Firefox、Safari 通用格式）
- 历史记录：按天分组、搜索、删除单条或按时间段清除（仅本次运行期间）
- 地址栏建议：输入时从书签和历史记录中匹配，↑↓ 选择
- 标签页组（边栏）：把当前标签页存为一组，随时重新打开

**阅读器与查找**
- 阅读器（Ctrl+Shift+R）：基于 Mozilla Readability 提取正文，四种主题、两种字体、可调字号；可设为对某网站自动启用；文章在严格的内容安全策略下显示，不执行网页脚本
- 页内查找（Ctrl+F）：高亮全部匹配、显示“第几个 / 共几个”

**速度**
- 网页加载完成前不做阅读器检测、缩略图等附加工作，先让网页显示出来
- 关闭 WARP 时可用“预先载入链接”：在起始页输入网址时提前下载该网页，指针停在同一网站的链接上时预先载入

**音频**
- 默认原声输出：不接管网页音频，可配合 Windows 空间音效 / Dolby Access 使用
- 可选浏览器音频增强：对白、轻柔、标准、影院四种模式；人声增强、齿音抑制、夜间模式、单声道合并、音量放大（最高 300%）
- 播放诊断：音频增强状态、视频分辨率、缓冲余量、区间丢帧率与显示帧/秒

**节能**
- 后台标签页按设置的时间自动挂起，切换回来时自动恢复；可立即挂起其他标签页
- 可选：最小化时自动挂起网页（播放声音时除外）

**浏览器UA标识**
- Microsoft Edge — Windows（默认）/ Microsoft Edge — macOS，对所有标签页生效，详见下文

## macOS Edge 伪装

切换到 macOS 后，网站从请求头和 JavaScript 看到的都是一台 Apple Silicon Mac 上的 Edge，各项数值彼此一致。改写通过 DevTools 协议在内核层完成，首个请求发出前就已生效。

| 检测面 | macOS 模式下的值 |
| :--- | :--- |
| `User-Agent` 请求头 / `navigator.userAgent` | `Mozilla/5.0 (Macintosh; Intel Mac OS X 10_15_7) … Edg/<本机内核版本>`，只替换系统段，版本号与内核一致 |
| Client Hints（`Sec-CH-UA-*`）/ `navigator.userAgentData` | 平台 `macOS`、版本 `26.2.0`、架构 `arm`、64 位；品牌列表和完整版本号取自本机 Edge 内核实测值（去掉 WebView2 特有的品牌项） |
| `navigator.platform` | `MacIntel` |
| WebGL / WebGPU 显卡信息 | `ANGLE (Apple, ANGLE Metal Renderer: Apple M1)` / `apple` · `metal-3` |
| 语音列表 | 隐藏 Windows 本地语音，只保留在线语音 |
| 滚动条 | 隐藏（与 macOS 默认的悬浮滚动条一致，不占布局宽度） |
| 跨进程 iframe、Dedicated Worker、Service Worker | 同样改写 UA、Client Hints 和 `platform` |
| SharedWorker | 无法改写，macOS 模式下隐藏该接口 |

被改写的函数调用 `toString()` 时仍显示为原生代码（`[native code]`）。macOS 模式下停用预先载入，因为预取请求不经过伪装，会暴露 Windows 和 WebView2。

**自检**：菜单“⋯ → 浏览器UA标识 → 检查当前浏览器UA标识…”列出上述各项以及各类 Worker 中的实际取值，与 macOS 不一致的项标红。请求头与字体请另用 [BrowserLeaks](https://browserleaks.com/) 等站点核对。

**系统版本号**：默认报告 macOS `26.2.0`，可在 `%LOCALAPPDATA%\UltraLightBrowser\config.json` 的 `settings.macPlatformVersion` 中修改（仅允许数字和点）。

**局限**：无法改变系统字体（字体探测可区分 Segoe UI / 微软雅黑 与 PingFang 等）、Emoji 和 Canvas / WebGL 的实际渲染结果，专业指纹检测仍可能识别出 Windows，而“伪装过”本身也是一种特征。非必要请保持默认的 Windows 模式。

## 快捷键

| 按键 | 功能 |
| :--- | :--- |
| Ctrl+T / Ctrl+W | 新建 / 关闭标签页 |
| Ctrl+Shift+T | 重新打开关闭的标签页 |
| Ctrl+Shift+W | 关闭窗口 |
| Ctrl+Tab / Ctrl+Shift+Tab | 下一个 / 上一个标签页 |
| Ctrl+1…8 / Ctrl+9 | 切换到第 N 个 / 最后一个标签页 |
| Ctrl+N / Ctrl+Shift+N | 新建窗口 / 无痕浏览窗口 |
| Ctrl+L / Alt+D / F6 | 聚焦地址栏 |
| Ctrl+R / F5 | 刷新 |
| Alt+← / Alt+→ / Alt+Home | 后退 / 前进 / 主页 |
| Ctrl+F，F3 / Shift+F3 | 页内查找，下一个 / 上一个 |
| Ctrl+Shift+R | 阅读器 |
| Ctrl+D / Ctrl+Shift+D | 添加书签 / 添加到阅读列表 |
| Ctrl+Shift+B / Ctrl+H / Ctrl+J | 书签 / 历史记录 / 下载 |
| Ctrl+Shift+L | 边栏 |
| Ctrl+Shift+\ | 标签页概览 |
| Ctrl+, | 设置 |
| Ctrl+加号 / 减号 / 0 | 放大 / 缩小 / 实际大小 |
| F11 / Esc | 进入 / 退出全屏 |
| Ctrl+Shift+H | 元素隐藏 |

## 已知限制

- 因为关闭即清除缓存，重新启动后第一次打开网站不会比首次访问更快。
- 界面没有动画效果（GDI+ 绘制）；标签页组为“保存并重新打开”的简化方式，不会像 Safari 那样整组切换窗口。
- 内置页面（起始页、历史、书签等）在 `https://ulb.internal/` 下显示，网页无法跳转或嵌入这些页面。
- 浏览器音频增强基于 Web Audio：跨域且未开放 CORS 的媒体、DRM 加密视频（如 Netflix）不会被处理；切换原声 / 增强模式会刷新页面。
- 不包含 Dolby 解码器或任何 Dolby 授权技术，不保证 Atmos 输出或比特流直通。

## 从源码构建

需要 Windows、Visual Studio 2022（C++ 桌面开发）、NuGet、CMake 3.25+ 和 Go（版本见 `warp/go.mod`）。

```powershell
.\build-windows.ps1          # 输出 dist\Release\UltraLightBrowser.exe
```

或手动：

```powershell
go build -C warp -trimpath -ldflags "-s -w -buildid=" -o bin/ulb-warp.exe .   # WARP 组件，作为资源嵌入 exe
nuget restore packages.config -PackagesDirectory packages
cmake -B build -S . -G "Visual Studio 17 2022" -A x64
cmake --build build --config Release
```

更多说明见 [FULL_VERSION_GUIDE.md](FULL_VERSION_GUIDE.md)，测试见 [tests/README.md](tests/README.md)。

## 项目结构

```text
src/
├── main.cpp                 入口、DPI 初始化、启动清理
├── AppShell.*               窗口管理、共享 WebView2 环境、退出清理
├── MainWindow.cpp           窗口消息、地址栏与建议、查找栏、全屏
├── MainWindowTabs.cpp       标签页生命周期、挂起、缩略图、阅读器、预先载入
├── MainWindowPaint.cpp      标题栏 / 标签栏布局与绘制、鼠标交互
├── MainWindowMenus.cpp      “⋯”菜单、分享菜单、标签右键菜单
├── MainWindowPages.cpp      边栏 / 总览面板，内置页面的消息接口
├── Library.*                书签、阅读列表、历史（内存）、标签页组、网站设置（内存）
├── InternalPages.*          内置页面资源、阅读器与查找脚本
├── Icons.*                  线条图标（标题栏与菜单）
├── WebViewManager.*         WebView2 环境与标签页、痕迹清除、音频增强、标识切换
├── WarpManager.*            启动和监控 WARP 组件
├── NativeRequestFilter.*    广告 / 跟踪请求拦截
├── ElementBlocker.*         元素隐藏
├── PowerManager.*           后台节能与 EcoQoS
├── UserAgent.hpp / MacStealth.hpp / SelfTestPage.hpp   macOS 标识
└── Config.*                 设置读写
warp/                        网络组件（Go）：WARP 注册、WireGuard 隧道、IP 优选、私人线路（Xray）、本机 SOCKS5
ui/                          内置页面（起始页、历史、书签、设置、总览、边栏、阅读器）
ui/vendor/                   Mozilla Readability（Apache-2.0）
tests/                       单元测试、页面资源检查、Windows 界面冒烟测试、性能测试
```

## 最近更新

- **v2.2.2**：公共线路更换伪装站点（Xray 提示以 Apple 为目标易被封锁），旧版本的公共线路已停用，请更新。
- **v2.2.1**：“我的线路”内置公共线路，未导入链接时也可直接使用；“用户代理”改称“浏览器UA标识”。
- **v2.2.0**：新增“我的线路”：可导入自己的 VLESS + REALITY 服务器链接，菜单和设置中可在直接连接、Cloudflare WARP、我的线路之间切换；线路不通时一律不直接联网；链接经 Windows DPAPI 加密保存在本机。内置 Xray 内核使 exe 增大约 10MB。
- **v2.1.4**：审计修复：开启“WARP 断开时阻止联网”后，即使 WARP 组件根本无法启动（被杀毒软件拦截、文件损坏等），也不再直接联网；首次启动等待 WARP 组件的时间从 3 秒延长到 8 秒（新解压的组件可能正被杀毒软件扫描）；WARP 组件重启与浏览器退出同时发生时不再可能遗留组件进程或重复关闭句柄；保留下来的旧浏览器配置也会补上 WebRTC 防泄露设置；书签等资料写入磁盘失败后，下次保存或退出时会重试，不再丢失。
- **v2.1.3**：审计修复：WARP 入口测速使用与 WireGuard 相同精度的握手时间戳，并在切换入口前稍作等待，避免切换后的首次握手被服务器当作重放丢弃而断流几秒；第二个浏览器进程中点“重新优选 IP”会提示无法优选，不再假装已开始；`build-windows.ps1` 和编译说明补上 WARP 组件的编译步骤（之前按说明从源码编译会失败）；自述文件重写。
- **v2.1.2**：开启 WARP 时“预先载入链接”和起始页地址栏预载自动停用，设置中注明原因（浏览器内核在使用代理时不进行预先载入，实测确认）；删除不起作用的启动参数（WebView2 忽略或 Chromium 已移除的开关）和未使用的节能模式设置；同时运行两个浏览器进程时，第二个进程的 WARP 状态显示为“由另一个浏览器进程运行”，不再一律显示“已连接”。
- **v2.1.1**：移除“公共 DNS”设置（开启 WARP 时域名已在隧道内由 Cloudflare 解析，该设置不起作用；旧版本保存的 DNS 选择在启动时清除）。审计修复：WARP 已连接时，隧道内解析不到的域名不再改用本机 DNS 直连（会泄露访问的域名和真实 IP）；同时运行两个浏览器窗口进程时，先关闭的一个不再删掉另一个仍在用的 WARP 端口记录；WARP 组件反复启动失败时逐步延长重试间隔；切换入口失败时不再显示为已切换；“重新优选 IP”在入口无变化时也能正确结束。
- **v2.1.0**：内置 Cloudflare WARP（默认开启，带 IP 优选）：浏览器经 WARP 加密隧道上网，优先 IPv6，域名在隧道内由 Cloudflare DNS 解析；无需安装 WARP 客户端或管理员权限；WebRTC 不会绕过隧道泄露真实 IP；可选“WARP 断开时阻止联网”。
- **v2.0.9**：审计修复：macOS 模式下停用网页预先载入（预取请求不经过身份伪装，会暴露 Windows 和 WebView2）；意外退出后下次启动补清网站图标与系统 DNS 缓存；下载的文件不再记录来源网址（保留“来自互联网”安全标记）。
- **v2.0.8**：启动和新标签页默认打开 Brave 搜索，地址栏不显示其网址；搜索引擎新增 Brave。
- **v2.0.7**：网页打开更快：在起始页输入网址时提前下载该网页（按回车时已下载好）；指针停在同一网站的链接上时预先载入；阅读器检测、缩略图、预备标签页改到页面加载完成后的空闲时再做；广告与跟踪拦截名单从 70 个扩充到 201 个域名。
- **v2.0.6**：移除隐私报告和常用网站；关闭浏览器时清除全部浏览痕迹（历史、缓存、Cookie、网站设置、网站图标、DNS 缓存等），历史记录不再写入磁盘，不再提供“恢复上次会话”和“保留网页缓存”。
- **v2.0.4**：网页加载提速：广告拦截只检查名单内的请求。
- **v2.0.2**：菜单、设置和内置页面的用语与 macOS Safari 简体中文版统一（边栏、标签页组、标签页概览、内容拦截器、用户代理、隐藏干扰项目等）。
- **v2.0.1**：操作流畅度优化：Ctrl+T 秒开、拖动窗口只重排当前标签、缩略图与存盘移到后台线程。
- **v2.0.0**：Safari 风格大改版——多标签与标签页概览、统一“⋯”菜单、起始页、书签 / 历史 / 阅读列表、阅读器、页内查找、边栏与标签页组、无痕浏览窗口、网站设置、隐私报告、地址栏建议、后台标签自动挂起。
- **v1.6.9**：分享菜单新增“关于”（可点击项目链接）；播放诊断增加区间丢帧率、显示帧/秒与缓冲余量，附 [2160p 播放审计](docs/2160p-playback-audit.md)。
- **v1.6.8**：修复上次选择 macOS Edge 后再次启动闪退的问题；macOS 模式不再暴露 WebView2 品牌项。
- **v1.6.7**：macOS 伪装覆盖 Service Worker 并隐藏 SharedWorker；新增标识自检页；系统版本号可配置。
- **v1.6.6**：macOS Edge 伪装改为内核层统一改写（请求头、Client Hints、WebGL、iframe、Worker）。

完整记录见 [Releases](https://github.com/Freecode100Year/UltraLightBrowser/releases)。

## 许可证

[MIT](LICENSE)。阅读器使用 [Mozilla Readability](https://github.com/mozilla/readability)（Apache-2.0，见 `ui/vendor/Readability-LICENSE.md`）。Safari 是 Apple Inc. 的商标，本项目与 Apple 无关；Dolby、Dolby Atmos 为其权利人的商标。


## 第三方组件

- 网络组件（`warp/`）使用 [wireguard-go](https://git.zx2c4.com/wireguard-go)（MIT）、[gVisor](https://gvisor.dev) 网络栈（Apache-2.0）和 [Xray-core](https://github.com/XTLS/Xray-core)（MPL-2.0）。WARP 是 Cloudflare 的服务，本项目与 Cloudflare 无关。
