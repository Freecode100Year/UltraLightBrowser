# UltraLightBrowser — 整合修复源码版

Windows 原生 Win32 / C++20 / Microsoft Edge WebView2 浏览器外壳。
当前版本 1.6.4，基于 v1.6.0，整合全屏、后台节能、音频保真和可选增强修复。

完整说明：[FULL_VERSION_GUIDE.md](FULL_VERSION_GUIDE.md)。
本包不含编译后的 EXE，Windows 实机验证尚未完成。

## 功能

- 全屏覆盖完整显示器，关闭全屏圆角和边框，恢复原窗口位置。
- 音频感知后台节能，正确处理挂起失败与恢复竞态，一次快照遍历子进程。
- 默认原声模式，不接管媒体，提供 Windows 音效设置入口，便于用户配置系统空间音效。
- 可选对白、轻柔、标准和影院 DSP；音箱保留左右声道；按需建立 HRTF/混响节点。
- 音量与 EQ 平滑调整，软件动态压缩保护；全部效果关闭时旁路处理。
- 借鉴 XQL-MUSIC：EQ 预衰减、补偿式交叉馈送、齿音抑制和夜间模式。
- 播放诊断显示增强状态、视频尺寸和丢帧统计。
- 原有地址搜索、缩放、元素隐藏、请求拦截及 DNS 设置。

本项目未集成授权 Dolby 解码器或专有 DSP，不保证 Atmos 播放、对象输出或比特流直通。
系统空间音效需用户在 Windows/Dolby Access 中单独配置；建议同时使用浏览器原声模式。
没有经过测量的速度、功耗和内存收益不会作为保证。

## 构建

要求 Windows、Visual Studio 2022 C++ 桌面工具、NuGet、CMake 3.25+ 和 WebView2 Runtime。

```powershell
.\build-windows.ps1
```

输出 `dist\Release\UltraLightBrowser.exe`。详见完整版说明。

## 快捷键

F11 全屏；Esc 退出全屏；Ctrl+L 地址栏；Ctrl+R/F5 刷新；
Ctrl+加号/减号缩放；Ctrl+0 重置缩放；Alt+左右方向键前进后退；
Ctrl+Shift+H 元素隐藏。

## 测试

参见 [tests/README.md](tests/README.md)。

## 许可证

MIT，见 [LICENSE](LICENSE)。Dolby 和 Dolby Atmos 名称属于相应权利人，
源码包不包含这些技术的分发授权或认证。
