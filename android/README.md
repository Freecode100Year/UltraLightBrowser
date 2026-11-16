# UltraLightBrowser for Android（测试版）

和电脑版一样：所有网页经网络线路访问（Cloudflare WARP、内置公共线路或你自己的线路），线路不通时不会直接联网；关闭浏览器时清除 Cookie、网站数据和缓存。界面仿照 iOS Safari：地址栏和按钮在屏幕下方，标签页以卡片网格显示。

- 网络组件与电脑版相同（`../warp`，Go），以 `lib/<abi>/libulbwarp.so` 打包，由 Android 解压到应用的原生库目录后运行；不需要 VPN 权限，只有这个浏览器走线路。
- WebView 通过 AndroidX `ProxyController` 走组件的本机 SOCKS5 代理；系统 WebView 太旧、不支持代理时**拒绝打开网页**，而不是直接联网。
- WebRTC 被禁用（它可能绕过代理暴露真实 IP）；Safe Browsing、WebView 使用统计已关闭。
- 自定义线路支持 VLESS、Trojan、VMess、Shadowsocks、Hysteria2（`hysteria2://` / `hy2://`）分享链接；不支持 TUIC。gRPC 是 VLESS / VMess 等线路的传输方式，不是独立协议。
- VLESS / Trojan / VMess / Shadowsocks 支持项目当前解析器提供的 TCP、WebSocket、HTTPUpgrade、XHTTP 和 gRPC 传输；TLS / REALITY 按协议与线路参数启用。证书校验保持开启。
- Hysteria2 由 Xray 自带的实现运行（`warp/quic_line.go`），支持密码及可选 Salamander 混淆；未知线路参数及不安全 TLS 选项会被拒绝。
- WireGuard 可从文件选择器导入单 Peer 的 wg-quick `.conf`，文件需包含 `[Interface]` 的 `PrivateKey`、`Address`、IP 格式 `DNS` 和 `[Peer]` 的 `PublicKey`、`Endpoint`、`AllowedIPs`；DNS 地址必须在 `AllowedIPs` 覆盖范围内。支持可选 `PresharedKey`、`PersistentKeepalive` 和 `MTU`；多 Peer 和其他未支持选项会被拒绝。网页 DNS 由 Xray 标记后通过 WireGuard outbound 发送。Hysteria2 和 WireGuard 的服务器域名会通过 Android 设备 DNS 解析。
- 订阅仅支持通用分享链接列表，不支持 Clash YAML，也不从订阅导入 WireGuard 文件；WireGuard 配置需逐个导入。
- 用户脚本：从菜单「用户脚本」导入 `.user.js` 或 `.js` 文件；支持按 `@match` / `@include` 限定网页、启停和删除。常用 `GM_info`、`GM_getValue`、`GM_setValue`、`GM_deleteValue`、`GM_addStyle` 可用；`@require`、菜单命令及跨域 GM 请求等高级 Tampermonkey API 暂不支持。普通 `.js` 导入时需要指定网址匹配规则。
- 暂不支持：Chrome / Edge 扩展、下载文件（系统下载器会绕过代理，目前只复制下载链接）、摄像头 / 麦克风 / 定位。

## 构建

```bash
GRADLE=/path/to/gradle android/build-apk.sh release   # 或 debug
```

需要 Go（版本见 `warp/go.mod`）、JDK 17+、Android SDK（`ANDROID_HOME`）、Gradle 9。网络组件按 arm64-v8a、armeabi-v7a、x86_64 分别编译为 Linux 静态程序（不需要 NDK）。正式签名使用 `android/keystore.properties`（不提交到仓库）。

输出：每种 CPU 一个 APK（正式版约 13 MB），另有一个通用 APK。大多数手机用 `arm64-v8a`。

安卓 App 里不能使用 netlink（Android 11 起禁止普通应用），网络组件不得依赖它：wireguard-go 的路由监听已绕开（`warp/tunnel.go` 的 `plainBind`），Hysteria2 不用 sing-box（其网络监视器依赖 netlink，在 App 里发不出数据）。改动网络组件后，请在模拟器里用 `run-as net.freedom8964.ulb` 以 App 身份运行测试，并检查 `adb logcat | grep avc` 没有 netlink 拒绝记录。
