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

UltraLightBrowser 是一个不到 1MB 的单文件浏览器。界面用原生 Win32 绘制（macOS Safari 风格的标题栏），网页由系统自带的 Microsoft Edge WebView2 内核（Chromium / Blink）渲染，因此：

- 不捆绑浏览器内核，程序本体约 700KB；
- 内核随 Windows 自动更新，及时获得 Chromium 安全补丁；
- 网页兼容性与 Edge 浏览器一致。

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

运行需要 Windows 10/11 x64 和 [WebView2 Runtime](https://developer.microsoft.com/microsoft-edge/webview2/)（Windows 11 已自带）。

## 功能

**浏览与界面**
- Safari 风格标题栏：居中地址栏，输入网址或关键词直接搜索
- 深色主题、Mica 背景、Win11 圆角；高 DPI（Per-Monitor V2）清晰显示
- 真全屏（F11）：覆盖整个显示器，无边框无圆角，退出后恢复原窗口
- 网页缩放、复制当前链接、用系统默认浏览器打开

**隐私与拦截**
- 内置广告 / 跟踪域名拦截（含常见国内外广告与统计服务），第三方子框架和跟踪像素同样过滤
- 开启 WebView2 严格防跟踪（Strict Tracking Prevention）
- 元素隐藏：Ctrl+Shift+H 点选页面元素永久隐藏
- 公共 DNS / DoH：Quad9（默认）、Cloudflare、Google、OpenDNS 或自定义 DoH；采用“加密优先、失败回退”模式，只写入本程序自己的数据目录，不修改系统或 Edge 设置

**音频**
- 默认原声输出：不接管网页音频，可配合 Windows 空间音效 / Dolby Access 使用
- 可选浏览器音频增强：对白、轻柔、标准、影院四种模式；人声增强、齿音抑制、夜间模式、单声道合并、音量放大（最高 300%）
- 播放诊断：显示增强状态、视频分辨率与丢帧统计

**节能**
- 最小化时隐藏并挂起网页、降低内存目标、启用 Windows EcoQoS
- 后台播放音频时不挂起，避免断音；音频停止 60 秒后才进入节能

**其他**
- 浏览器标识切换：Windows Edge（默认）/ macOS Edge。macOS 模式统一改写 UA、Client Hints 请求头、`navigator.platform`、`navigator.userAgentData`（品牌与完整版本取自本机 Edge 内核实测值）、WebGL / WebGPU 显卡信息、滚动条宽度和本地语音列表，并覆盖跨进程 iframe、Dedicated Worker 与 Service Worker（SharedWorker 无法改写，macOS 模式下隐藏）；改写过的函数保持原生 `toString()`。菜单“浏览器 UA 标识 → 标识自检页”可一键核对各项；系统版本号可在配置文件 `macPlatformVersion` 中修改（默认 26.2.0）
- WebView2 启动失败时显示错误码与排查建议

## 快捷键

| 按键 | 功能 |
| :--- | :--- |
| Ctrl+L | 聚焦地址栏 |
| Ctrl+R / F5 | 刷新 |
| Alt+← / Alt+→ | 后退 / 前进 |
| Ctrl+加号 / 减号 / 0 | 放大 / 缩小 / 重置缩放 |
| F11 / Esc | 进入 / 退出全屏 |
| Ctrl+Shift+H | 元素隐藏 |

## 已知限制

- 单窗口单页面，暂不支持多标签。
- 浏览器音频增强基于 Web Audio：跨域且未开放 CORS 的媒体、DRM 加密视频（如 Netflix）不会被处理；切换原声 / 增强模式会刷新页面。
- 不包含 Dolby 解码器或任何 Dolby 授权技术，不保证 Atmos 输出或比特流直通。
- macOS 伪装无法改变系统字体、Emoji 与 Canvas / WebGL 实际渲染结果（字体探测可区分 Segoe UI / 微软雅黑 与 PingFang 等）；专业指纹检测仍可能识别出 Windows，而“伪装过”本身也是特征。macOS 模式下页面滚动条隐藏（与 macOS 默认一致）。非必要请保持默认。

## 从源码构建

需要 Windows、Visual Studio 2022（C++ 桌面开发）、NuGet、CMake 3.25+。

```powershell
.\build-windows.ps1          # 输出 dist\Release\UltraLightBrowser.exe
```

或手动：

```powershell
nuget restore packages.config -PackagesDirectory packages
cmake -B build -S . -G "Visual Studio 17 2022" -A x64
cmake --build build --config Release
```

更多说明见 [FULL_VERSION_GUIDE.md](FULL_VERSION_GUIDE.md)，测试见 [tests/README.md](tests/README.md)。

## 项目结构

```text
src/
├── main.cpp                 入口、DPI 初始化、消息循环
├── MainWindow.*             标题栏、地址栏、菜单、全屏
├── WebViewManager.*         WebView2 创建、音频增强脚本、UA
├── NativeRequestFilter.*    广告 / 跟踪请求拦截
├── ElementBlocker.*         元素隐藏
├── DnsManager.*             公共 DNS / DoH
├── PowerManager.*           后台节能与 EcoQoS
├── WindowGeometry.hpp       全屏 / 最大化尺寸计算
├── UserAgent.hpp            UA 标识生成
└── Config.*                 设置读写
tests/                       节能、窗口几何、音频、UA 回归测试
```

## 许可证

[MIT](LICENSE)。Dolby、Dolby Atmos 为其权利人的商标。
