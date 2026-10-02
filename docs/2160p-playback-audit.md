# 2160p 播放代码审计（2026-10-02）

审计基线：GitHub main 99ed43f9b54d931d05afc895866bf2e94f8cec76（1.6.8）。
本次版本：1.6.9。当前 Linux 环境无法运行 Windows WebView2、采集 GPU 解码数据或实测视频。
结论：尚不能证明存在前台 2160p 卡顿，也不能保证无卡顿；以下是代码事实和待测风险。

## 已确认的行为

| 代码路径 | 发现 | 对播放的意义 |
|---|---|---|
| WebViewManager 初始化 | 前台 NORMAL 内存预算；hardwareAcceleration=false 添加 --disable-gpu | 未发现前台主动降低预算；关闭 GPU 可能影响硬解，应在实机记录 |
| MainWindow WM_ACTIVATE / WM_TIMER + PowerManager | 失焦 300000ms 后，仅按 IsDocumentPlayingAudio 判断保护；无音频时隐藏、启用 EcoQoS、LOW 预算并尝试挂起 | 可见但失焦的静音视频也会走节能路径，不是单纯解码慢；挂起可能被引擎拒绝，但隐藏/低预算仍已发生 |
| PowerManager 恢复 | 恢复可见、NORMAL 内存预算，并解除子进程 EcoQoS | 回归测试通过；不代表恢复时零丢帧 |
| 音频路由 | 默认原声不创建 AudioContext；增强才分频、压缩、HRTF/混响 | 默认无这部分 DSP 负担；增强开销需测，不能据源码给出 CPU 百分比 |
| NativeRequestFilter | 未注册 MEDIA 类型；注册 FETCH/XHR/OTHER 等并同步检查域名/路径 | 没有直接重写视频帧；MSE 分片仍可能走 fetch/XHR，请求拦截需按站点做开关对照 |
| 启动参数 | 强制 zero-copy、hardware overlays、GPU buffers 和多个节能 feature | 参数支持随 Runtime/驱动变化；参数存在不能证明已硬解或更流畅。没有实测依据，不随意增加/删除 |
| 播放诊断 | 旧版仅累计丢帧，依赖音频脚本 | 不足以归因；本次改为独立视频采样，保留音频状态并增加区间统计和缓冲余量 |

## 本次实际更改

- 分享菜单增加“关于 UltraLightBrowser”，使用原生对话框显示版本和可点击 GitHub 项目链接。
- 音频菜单的“播放诊断”读取视频分辨率、播放状态、readyState、当前播放点缓冲秒数。
- 第一次点击建立基线；连续播放约 30 秒后再点，报告两次之间总帧、丢帧、丢帧率和平均显示帧/秒。
- 重载导致计数回退、源/分辨率切换、倒退跳转、当前 seeking 或采样端点暂停时重建基线。
- 帧数不足不判定为流畅。只统计主文档；iframe/硬解/codec/停顿原因没有虚构。
- 仅手动采样，无持续定时器、逐帧回调或视频帧拷贝。
- 未改变节能、拦截或解码策略，没有把未验证的风险写成已修复卡顿。

## Windows 实测步骤

使用 Release 构建，记录 WebView2 Runtime、显卡驱动、电源模式、显示刷新率、硬件加速设置、视频 URL。
同一素材分别测试 2160p30 和 2160p60；编码格式分别记录 H.264/VP9/AV1/HEVC（以实际站点/DevTools 信息为准）。
在你的 Alienware 16X 上，分别接电与电池测试；GPU 型号本身不能保证站点流畅。

1. 保持默认 Windows UA、原声、硬件加速开启，前台播放。确认诊断实际高度是 2160，而非播放器只是选了 4K。
2. 等初始缓冲稳定后打开一次播放诊断；关闭对话框，连续播放 30 秒，再开一次。重复 3 次；记录实际区间时长（含对话框关闭耗时）。
3. 在 YouTube 使用“详细统计信息”记录 codec、dropped frames、buffer health；通过 Edge/WebView2 调试工具或任务管理器的 GPU Video Decode 核实硬解。GPU 利用率低不等于硬解失败。
4. 与相同 Runtime 主版本的 Edge，用同素材/编码/清晰度/网络做对照。再分别测试关闭拦截、开启音频增强、macOS UA、F11 全屏，每次只改变一项。
5. 失焦但窗口可见超过五分钟，分别测试有声与静音。静音进入节能是已确认策略；不得误记成正常前台解码卡顿。
6. 最小化/恢复后重新建立基线；不要把切换清晰度、seek 或暂停计入稳定播放区间。

判读：持续增加的丢帧是解码/呈现压力的线索；缓冲耗尽更偏网络/供片问题，但快照不能单独确定根因。
平均显示帧/秒低也可能是暂停、缓冲等待或源帧率低。零丢帧仍可能有网络冻结。
建议每次有效区间至少 30 秒且至少数百帧；1% 丢帧只能作为排查信号，不是所有视频通用的合格线。

## 验证与限制

本地通过：视频诊断（实际脚本 VM 测试）、音频路由、滤波器响应、脚本注册、节能策略、窗口几何和 UA 测试。
Windows 编译、可点击对话框及真实 2160p 播放待实机或 GitHub Windows CI 验证；本地测试不是视频基准测试。

参考：
- https://w3c.github.io/media-playback-quality/ （总帧含已显示及丢弃帧；加载媒体会重置计数）
- https://learn.microsoft.com/en-us/microsoft-edge/webview2/reference/win32/icorewebview2_19 （低内存目标可能影响性能）
- https://learn.microsoft.com/en-us/microsoft-edge/webview2/reference/win32/icorewebview2_3 （挂起条件及效果）
