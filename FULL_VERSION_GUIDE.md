# UltraLightBrowser 1.6.3 整合修复版

这是基于 v1.6.0 的完整源码版本，包含全屏、后台节能与音频处理修复。
压缩包不含预编译 EXE；当前开发环境为 Linux，Windows 编译和实际播放仍需验证。

## Windows 编译

安装 Visual Studio 2022 的「使用 C++ 的桌面开发」、CMake 3.25 以上、NuGet CLI，
并确保 Microsoft Edge WebView2 Evergreen Runtime 可用。在源码目录运行：

```powershell
.\build-windows.ps1
# 可选：调试构建
.\build-windows.ps1 -Configuration Debug
```

输出程序位于 `dist\Release\UltraLightBrowser.exe`。
如果 PowerShell 的脚本策略阻止运行，可直接执行：

```powershell
nuget restore packages.config -PackagesDirectory packages
cmake -B build -S . -G "Visual Studio 17 2022" -A x64
cmake --build build --config Release --parallel
```

不要把本源码包当成已经编译、签名或实机验证的正式发布。

## Dolby Atmos 与原声输出

默认使用「原声输出」。该模式不创建浏览器 Web Audio 上下文，不接管媒体元素，
不进行浏览器端立体声降混、EQ、压缩、HRTF 或卷积混响。
这里的原声是浏览器增强旁路，不保证位完美、WASAPI 独占或 Dolby 比特流透传。
WebView2、网站播放器和 Windows 仍会执行其正常的解码及输出处理。

工具栏音频菜单 → 「Windows 声音设置 / Dolby Atmos」打开系统声音设置。
在 Windows 中选中实际使用的输出设备，进入空间音效设置。
需要 Dolby Atmos for Headphones 时，通过 Microsoft Store 安装 Dolby Access，
按其提示激活并选择 Dolby Atmos for Headphones。费用、试用和设备支持以应用显示为准。
也可以选择系统提供的 Windows Sonic for Headphones。

本项目不分发 Dolby 专有 DSP/SDK，不自动购买许可，也不会自动修改系统空间音效。
提供的是原声模式和系统设置入口，不是 Dolby 官方认证，也没有新增 Atmos 解码、
Atmos 对象输出、HDMI 比特流直通或 DRM 服务认证。是否实际获得 Dolby 处理、
是否播放真正 Atmos 内容，取决于系统、设备、网站、媒体音轨和运行时支持；不能由
本项目的模式名称确认。普通立体声不会因此获得原始 Atmos 对象元数据。

启用系统空间音效时建议保留原声模式，避免与浏览器 HRTF/混响叠加。
从原声切换增强或反向切换会刷新当前页面，以真正释放媒体处理绑定，播放进度可能重置。

参考：
- https://support.microsoft.com/en-us/windows/hardware/drivers/how-to-turn-on-spatial-sound-in-windows
- https://www.dolby.com/experience/headphones/
- https://professionalsupport.dolby.com/s/article/Windows-Implementation
- https://professional.dolby.com/licensing/

## 浏览器增强

音频菜单 → 「浏览器音频增强」 → 「浏览器增强选项」。

| 模式 | 处理 | 适用场景 |
| --- | --- | --- |
| 对白 | 3kHz +4.5dB EQ、轻度动态压缩；无 HRTF 或混响 | 电视剧、讲解 |
| 轻柔 | 轻度声场加宽；耳机可用交叉馈送和 HRTF | 轻度空间效果 |
| 标准 | 更宽声场；耳机使用交叉馈送和 HRTF | 立体声节目 |
| 影院 | 加宽、耳机虚拟后置、短混响 | 偏好明显空间效果时 |
| 全部关闭 | 已绑定媒体使用单位增益旁路 | 比较原声与效果 |

音箱路径使用明确的左右声道合并器，保留立体声分离。
单声道输入在增强前上混成左右声道；单耳模式把左右内容合并后输出到两边。
DSP 模式按需创建节点：音箱不创建 HRTF，非影院模式不创建混响或虚拟后置。
音量与 EQ 参数平滑调整；改变处理拓扑会短暂淡出/淡入，避免突然跳变。
软件压缩器用于动态保护，不是硬件级或真峰值砖墙限幅；高增益仍可能失真。
当前没有自动 LUFS 响度匹配、AudioWorklet 真峰值限幅或 AI 音源分离。

增强只适用于可安全接入 Web Audio 的媒体。受保护媒体、网站自己的 Web Audio、
跨域内容和动态切换媒体源可能不支持增强。若出现静音或播放器异常，切回原声并刷新。
不要为了增强而强改跨域属性、绕过 DRM 或关闭浏览器安全机制。

## 播放诊断

音频菜单 → 「播放诊断 / 视频丢帧统计」。显示当前页面音频输出模式、
媒体是否被增强、音频上下文状态，以及播放器可提供的视频尺寸、总帧数和丢帧数。
这是当前页面的快照，不是 GPU 利用率、解码器名称、Dolby 检测或端到端延迟测量。

## 渲染、全屏与后台

- 全屏使用完整显示器区域，优先于最大化工作区；全屏关闭 DWM 圆角、边框及扩展边缘。
- 退出全屏恢复窗口样式与位置。
- 前台从正常内存预算启动，静音后台才降低预算；音频后台恢复正常预算并解除子进程节能限速。
- 挂起失败、恢复与异步挂起回调竞态均有处理；进程树一次快照遍历。
- `config.json` 的 `hardwareAcceleration: false` 现在会请求关闭 GPU 加速；需重启。
- 不强制宣称 Nvidia/Intel 视频超分已开启，也不强制把音频服务合并到浏览器进程。
- Debug 构建不再强行使用 `/O2 /GL /LTCG`，保留可调试性。

## 测试和限制

见 `tests/README.md`。本地已测试实际注入 JS 的路由行为与 C++ 进程/窗口策略。
测试用确定性的 API 替身，不能验证真实声音、COM 引用计数、GPU 驱动或 Windows DWM。

建议实机检查：普通/最大化窗口进入全屏；多显示器和 125%/150% DPI；
左右声道测试音与单声道；对白/影院切换；快速原声/增强切换；刷新和跳转后保留模式；
后台音频不断流；跨域和受保护网站在原声模式正常播放。

新增 CI 包含音频回归测试、窗口/节能测试和 Windows Debug/Release 构建。
当前 GitHub 写 API 返回 403，Git 推送缺少凭据；源码未推送，新增 CI 尚未运行。

## 1.6.2：借鉴 XQL-MUSIC 的音频改进

借鉴来源：Freecode100Year/XQL-MUSIC，版本 2573ca3。
本次移植算法与数值验证方法，没有引入 React、音乐搜索、Cloudflare API 或音乐资源代理。

- 人声 EQ 提升时自动预衰减 5dB（4.5dB 提升 + 0.5dB 余量）；100% 用户增益时该 EQ 段峰值保留约 0.5dB 余量。高用户增益和空间效果仍可能过载。
- 耳机交叉馈送改用低通与低架补偿，减少中置信号低频抬升。
- 增强选项新增「齿音抑制」：5.5kHz LR4 分频，仅高频动态压缩；低频路径增加 6ms 延迟，对齐 Web Audio 压缩器的前瞻延迟。
- 新增「夜间模式」：独立 -24dB 阈值、3:1 轻压缩；不是自动响度归一化。
- 原声模式不启用以上 DSP；两个新开关默认关闭，可分别开启。
- 关闭环绕但保留齿音/夜间模式时，相关处理仍然有效。真正回到原声请选择原声输出并刷新。

运行 `node tests/verify_audio_response.mjs` 做 48kHz 下滤波器数值验证。
检查的是交叉馈送子模块、静态分频合成和人声 EQ 电平余量，
不包含 HRTF、混响、实际压缩过程、驱动或端到端延迟的保证。
新增模块会增加处理延迟，实际音画同步需 Windows 播放验证。
Web Audio 压缩器延迟参考：https://www.w3.org/TR/webaudio/#DynamicsCompressorNode

移植审查修正了源项目数值检查中低通/高通 Q 单位的假设：
Web Audio 的这两类滤波器 Q 使用 dB，因此 Butterworth 设置为约 -3.0103dB，
而 peaking EQ 的 Q 仍使用线性值。数值验证采用相同的 Web Audio 单位转换。
本版不是未经修改地照搬源项目的参数。

## 1.6.3：浏览器 UA 标识切换

点击工具栏分享按钮 →「浏览器 UA 标识」→ Windows Edge（默认）或 macOS Edge。
切换成功后自动刷新当前页，选择保存到 config.json 的 settings.userAgentProfile，启动时在首次导航前应用。
macOS 使用 Macintosh / Intel Mac OS X 10_15_7 平台标识，保留当前 WebView2 的 Chrome 和 Edg 版本；此系统字段是兼容性标识，不代表实际 macOS 版本。
默认模式恢复初始化时读取的完整原始 UA，避免写死版本。失败时不保存菜单切换，旧 Runtime 不支持接口时显示提示。

这是 UA 兼容性切换，不是完整的系统指纹模拟。Windows 渲染引擎、字体、GPU、解码和 DRM 能力不会因此变成 macOS。
WebView2 设置 UA 可能清除 Sec-CH-UA-* 与 navigator.userAgentData，行为取决于 Runtime；不保证所有站点识别为 Mac。
页面 navigator.platform 仍可能显示 Windows。本版不额外伪造平台或高熵 Client Hints。
切换会刷新，表单或未保存内容可能丢失；已经运行的 worker 需要站点自行重新创建，某些共享 worker 的 UA 由最后设置的 WebView 决定。
Windows 实机验收：检查请求 User-Agent 与 navigator.userAgent，两个方向切换，退出重启验证保存；检查实际站点兼容性。
接口参考：https://learn.microsoft.com/en-us/microsoft-edge/webview2/reference/win32/icorewebview2settings2
