# tuptup.top · WinCE 车机投屏镜像 —— 架构设计

> 参考 [scrcpy](https://github.com/Genymobile/scrcpy) 的「设备端采集编码 + 主机端解码渲染 + 反向控制」模型，针对 **ARM WinCE 6.0 车机** 做工程化适配。
> 目标：把安卓手机屏幕实时投到车机大屏，并能在车机上触摸点击反向操控手机。

---

## 1. scrcpy 模型回顾

scrcpy 把安卓设备当「源」，PC 当「显示/控制端」：

```
[Android 设备]                         [PC 主机]
 MediaProjection 捕获屏幕     ──H264──▶  FFmpeg 解码 → SDL 渲染
 InputManager 注入控制  ◀──键鼠事件──   捕获键鼠
 (scrcpy-server.jar)                   (scrcpy client)
```

它之所以轻、延迟低（35~70ms），关键是：
1. **设备端用 MediaCodec 硬件编码** H.264，不走 `screenrecord`（那是文件级、高延迟）；
2. **主机端用 FFmpeg 软/硬解 + SDL 渲染**；
3. **控制权走同一条 TCP/USB 隧道**，双向复用。

我们完全复用这套「采集/编码 → 网络 → 解码/渲染 → 捕获/回注」的分层，只把「PC 主机」换成「WinCE 车机」。

---

## 2. 本系统角色映射

| scrcpy 组件 | 本系统对应 | 平台 |
|---|---|---|
| scrcpy-server（采集编码+回注） | `android/` ScreenMirrorSender | Android (Kotlin) |
| scrcpy client（解码渲染+捕获） | `wince/` WinCEClient | ARM WinCE (C++) |
| 传输隧道（adb/usb/tcp） | **WiFi TCP**（端口 8686） | — |

**为什么不用 USB/ADB 隧道（像亿连那样）？**
scrcpy 虽支持 USB，但那依赖 WinCE 扮演「USB 主机 + ADB 协议栈」角色——WinCE 6.0 没有现成 ADB 主机端，自研成本极高且不稳。车机普遍带 WiFi 或可用 USB-WiFi 网卡，**WiFi TCP 是工程上最务实、最稳的传输层**，延迟在局域网内可接受（1080p@30fps 约 40~80ms）。

---

## 3. 系统架构图

```
        ┌─────────────────────────── 安卓手机 (Android 7+) ───────────────────────────┐
        │                                                                             │
        │   MediaProjection                                                          │
        │        │ (Surface)                                                         │
        │        ▼                                                                   │
        │   MediaCodec (H.264 Baseline, 低延迟)                                       │
        │        │                                                                   │
        │        ├──▶ [NET] TCP 8686 ───────────────────┐                           │
        │        │                                      │                           │
        │   TouchInjector ◀── 触摸事件 ◀────────────────┘ (回注到手机)                │
        │   (AccessibilityService / InputManager)                                      │
        └──────────────────────────────────────────────┬────────────────────────────┘
                                                        │  WiFi (同网/热点)
        ┌───────────────────────────────────────────────▼───────────────────────────┐
        │                 ARM WinCE 6.0 车机  (WinCEClient.exe)                      │
        │                                                                             │
        │   [NET] TCP 8686 接收 H.264                                                │
        │        │                                                                   │
        │        ▼                                                                   │
        │   Decoder (ffmpeg 软解 / MJPEG 降级)                                        │
        │        │  (RGB 帧)                                                         │
        │        ▼                                                                   │
        │   Renderer (GDI CreateDIBSection + BitBlt) ──▶ 车机 LCD                    │
        │                                                                             │
        │   TouchCapture (鼠标/触摸消息) ──▶ 编码 ──▶ [NET] 发送触摸事件             │
        └─────────────────────────────────────────────────────────────────────────────┘
```

---

## 4. 关键模块设计

### 4.1 传输层（`proto/` 协议见 `protocol.md`）
- 传输：**TCP**，端口 `8686`，字节序大端。
- 握手：车机端主动连手机（或手机开服务、车机连）；交换分辨率、解码能力（H264/MJPEG）。
- 复用同一条 socket 做「视频下行 + 触摸上行 + 心跳」，避免多连接竞争。
- 每个消息：`MAGIC(4) + TYPE(1) + LEN(4) + PAYLOAD(LEN)`，见 `protocol.md`。

### 4.2 视频链路
- **编码（手机端）**：`MediaCodec` 配 `KEY_PROFILE = Baseline`、`KEY_BITRATE ~2~4Mbps`、`KEY_FRAME_RATE=30`、`KEY_I_FRAME_INTERVAL=1`（每秒一个关键帧，断线重连快恢复）。输出 Annex-B（SPS/PPS + slice），直塞 socket。
- **解码（车机端）**：
  - **首选 ffmpeg（ARM 移植，如 ffmpeg 2.x/3.x 的 WinCE 构建）** 解 H.264 → YUV → RGB；
  - **降级 MJPEG**：若车机无 ffmpeg/性能不足，手机端改 `MediaCodec` 编码 JPEG 序列（质量 60~70），车机用轻量 JPEG 解码器（如 `tinyjpeg` / `libjpeg WinCE 移植`）。带宽更大但解码极简，**保证老车机也能跑**。
- **渲染（车机端）**：`CreateDIBSection` 建 RGB 位图 → `BitBlt` 到窗口 DC。比 DirectDraw 更稳、兼容所有 WinCE 6.0 屏。分辨率自适应车机屏（常见 800×480）。

### 4.3 控制（触摸）链路
- **捕获（车机端）**：WinCE 触摸屏产生 `WM_LBUTTONDOWN/MOVE/UP`（或 `WM_POINTER`）；用 `GetCursorPos`/消息参数取坐标，按车机分辨率归一化到手机分辨率。
- **回注（手机端）**：收到坐标 → `MotionEvent`（`ACTION_DOWN/MOVE/UP`）→ 经 **AccessibilityService** 的 `dispatchGesture` 或 `InputManager.injectInputEvent` 回注。**走 WiFi 非 ADB，用无障碍服务最稳、无需 root。**
- 多点/双指缩放：V1 仅单指点击/滑动；双指缩放留接口（后续用两路 TOUCH 事件对）。

### 4.4 音频（可选，V2）
- 手机端 `AudioPlaybackCapture` → Opus/PCM → 车机 `waveOut` 播放。V1 默认关闭，避免车机音频延迟/兼容问题。

---

## 5. 选型理由（为什么是「最佳方案」）

| 决策点 | 选型 | 理由 |
|---|---|---|
| 传输 | WiFi TCP | 避开 WinCE 缺失的 ADB 主机栈；局域网延迟可接受 |
| 编码 | MediaCodec H264 Baseline | 硬件编码、零延迟、scrcpy 验证过 |
| 车机解码 | ffmpeg 软解 + MJPEG 降级 | 兼顾性能与老车机兼容性 |
| 车机渲染 | GDI BitBlt | 稳、全 WinCE 6.0 通用，不依赖 GPU |
| 触摸回注 | AccessibilityService | 非 ADB、非 root，WiFi 直连可用 |
| 协议 | 自研轻量二进制 | 比 JSON 省带宽、解析快，适合嵌入式 |

---

## 6. 编译与部署

### 手机端（Android）
- 环境：Android SDK + NDK（CI 用 `actions/setup-java` + `android-actions`）。
- 产物：`app-release.apk`，装到手机，开「开发者选项→USB调试」+ 无障碍服务授权。
- 详见 `android/README.md`。

### 车机端（WinCE ARM）
- 环境：**Windows + Visual Studio 2008 + Windows CE 6.0 SDK + ARMv4I 交叉工具链**（本机 macOS 无此环境，无法本地编译）。
- 产物：`WinCEClient.exe` + `ffmpegce.dll`（或 MJPEG 解码器），拷到 SD 卡，导航路径指向它。
- CI 现状：GitHub Actions **仅能构建 Android 端**；WinCE 端做 CMake 配置校验 + 静态检查，真编译需在 Windows 环境（见 `wince/README.md` 与 CI 注释）。

### 连接步骤
1. 手机开热点 / 车机连同一 WiFi；
2. 车机运行 `WinCEClient.exe`，输入手机 IP:8686；
3. 手机端 App 启动镜像服务；
4. 车机显示手机画面，触摸即可操控。

---

## 7. 局限与路线图

- **V1 局限**：单指触摸、无音频、H264 解码依赖车机 ffmpeg；老车机降级 MJPEG 时清晰度/帧率下降。
- **路线图**：
  - V1.1：双指缩放、Android 11+ 适配（MediaProjection 前台服务）；
  - V1.2：音频透传；
  - V2：可选 USB 隧道（若车机有 ADB 主机能力）；
  - V2+：车机端 H264 硬解（若 WinCE 有 DXVA/DirectDraw 加速）。

---

## 8. 与现成方案的定位

- 本方案是 **scrcpy 思路在 WinCE 上的开源落地**，补上「WinCE 无开源投屏客户端」的空白；
- 不替代亿连（亿连是闭源商业、已升 4.6.14），而是提供一个**可审计、可改、免费**的替代；
- 若手机安卓 ≤10，亿连仍是最省事的现成选择；本方案面向「想自己掌控、或手机较新」的场景。
