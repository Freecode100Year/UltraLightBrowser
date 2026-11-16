# UltraLightBrowser for Android（测试版）

和电脑版一样：所有网页经网络线路访问（Cloudflare WARP、内置公共线路或你自己的线路），线路不通时不会直接联网；关闭浏览器时清除 Cookie、网站数据和缓存。界面仿照 iOS Safari：地址栏和按钮在屏幕下方，标签页以卡片网格显示。

- 网络组件与电脑版相同（`../warp`，Go），以 `lib/<abi>/libulbwarp.so` 打包，由 Android 解压到应用的原生库目录后运行；不需要 VPN 权限，只有这个浏览器走线路。
- WebView 通过 AndroidX `ProxyController` 走组件的本机 SOCKS5 代理；系统 WebView 太旧、不支持代理时**拒绝打开网页**，而不是直接联网。
- WebRTC 被禁用（它可能绕过代理暴露真实 IP）；Safe Browsing、WebView 使用统计已关闭。
- 暂不支持：下载文件（系统下载器会绕过代理，目前只复制下载链接）、摄像头 / 麦克风 / 定位、用户脚本和扩展。

## 构建

```bash
GRADLE=/path/to/gradle android/build-apk.sh release   # 或 debug
```

需要 Go（版本见 `warp/go.mod`）、JDK 17+、Android SDK（`ANDROID_HOME`）、Gradle 9。网络组件按 arm64-v8a、armeabi-v7a、x86_64 分别编译为 Linux 静态程序（不需要 NDK）。正式签名使用 `android/keystore.properties`（不提交到仓库）。

输出：每种 CPU 一个 APK（约 9–10 MB），另有一个通用 APK。大多数手机用 `arm64-v8a`。
