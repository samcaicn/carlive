# GLOAI 车机投屏 · 安卓发送端

把安卓手机屏幕通过 **WiFi TCP** 实时投到 WinCE 车机，并支持车机端触摸反向操控手机（参考 scrcpy 架构）。

## 编译

环境：Android SDK + NDK（已验证 GitHub Actions 可构建；本地需 `gradle 8.6` 或 `./gradlew`）。

```bash
cd android
gradle wrapper --gradle-version 8.6   # 首次生成本地 wrapper（可选）
gradle assembleRelease               # 产物 app/build/outputs/apk/release/app-release.apk
```

> CI 见仓库根 `.github/workflows/build.yml`，用 `gradle/gradle-build-action` 自动构建 APK。

## 安装与权限

1. `adb install app-release.apk`；
2. 打开 App，在系统「设置 → 无障碍」中开启 **GLOAI 车机投屏**（触摸回注必需，无需 root）；
3. 首次「开始镜像」会弹 **屏幕录制授权**，允许即可（MediaProjection）。

## 使用

1. 手机与车机连同一 WiFi（或手机开热点，车机连接）；
2. 车机端先运行 `WinCEClient.exe` 并监听/连接手机 IP:8686（见 `../wince/README.md`）；
3. 手机 App 输入车机 IP:端口（默认 `8686`），点「开始镜像」；
4. 车机显示手机画面，触摸车机屏即可操控手机。

## 模块

| 文件 | 职责 |
|---|---|
| `MainActivity.kt` | UI、权限请求、连接编排 |
| `MirrorState.kt` | 同进程共享 MediaProjection / NetClient |
| `MirrorForegroundService.kt` | 前台服务（Android 10+ 录屏要求） |
| `ScreenSender.kt` | MediaProjection + MediaCodec H264 编码 |
| `NetClient.kt` | TCP 通信 + 协议封装/解析（`../proto/protocol.md`） |
| `Protocol.kt` | 协议常量 + avcc→Annex-B 转换 |
| `MirrorAccessibilityService.kt` | 触摸回注（GestureDescription） |

## 已知限制

- 单指点击/滑动；双指缩放为 V1.1 计划；
- 触摸回注走无障碍服务，连续轨迹平滑度弱于 InputManager（root 设备可替换）；
- 音频透传为 V2 计划（AudioPlaybackCapture）。
