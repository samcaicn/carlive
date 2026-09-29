# GLOAI · WinCE 车机投屏镜像

把安卓手机屏幕实时投到 **ARM WinCE 6.0 车机**大屏，并能在车机上触摸点击反向操控手机。
参考 [scrcpy](https://github.com/Genymobile/scrcpy) 的「采集编码 → 网络 → 解码渲染 → 反向控制」模型，针对 WinCE 适配。

> WinCE 平台**没有开源投屏客户端**（scrcpy 仅支持 PC 端），本项目填补这一空白，提供一个可审计、可改、免费的替代。

## 架构

```
安卓手机 (采集/编码/回注)  ──WiFi TCP:8686──▶  WinCE 车机 (解码/渲染/采触摸)
```

- 传输：WiFi TCP（避开 WinCE 缺失的 ADB 主机栈）
- 视频：安卓 `MediaCodec` H.264/MJPEG → 车机 `ffmpeg`/`tinyjpeg` 软解 → GDI 渲染
- 控制：车机触摸 → TCP → 手机 `AccessibilityService` 回注
- 协议：详见 [`proto/protocol.md`](proto/protocol.md)

完整设计见 [`docs/DESIGN.md`](docs/DESIGN.md)。

## 目录

| 路径 | 内容 |
|---|---|
| `android/` | 安卓投屏发送端（Kotlin，MediaProjection + MediaCodec） |
| `wince/` | 车机接收端（C++/ARM WinCE，GDI 渲染 + 触摸捕获） |
| `proto/` | 通信协议定义 |
| `docs/` | 架构设计 |
| `.github/workflows/` | CI（构建 Android APK / 源校验） |

## 构建

- **安卓**：见 `android/README.md`（GitHub Actions 已可构建 APK）。
- **WinCE**：见 `wince/README.md`（需 Windows + VS2008 + CE6 SDK，本地编译）。

## 使用

1. 手机与车机连同一 WiFi（或手机开热点）；
2. 车机运行 `WinCEClient.exe`（同目录 `config.txt` 填手机 IP:端口）；
3. 手机打开 GLOAI App，开启无障碍服务，输入车机 IP，开始镜像；
4. 车机显示手机画面，触摸即可操控。

## 状态

- V1：单指触摸、MJPEG 默认解码、无音频。
- 路线图：双指缩放、Android 11+ 适配、音频透传、H264 硬解。

## 协议

代码以 Apache-2.0 开源（见 `LICENSE`）。
