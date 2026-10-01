# GLOAI 车机投屏 · WinCE 接收端（ARM WinCE 6.0）

车机端程序 `GLOAI.exe`，接收安卓手机投来的屏幕画面并渲染，同时把车机触摸回传给手机（参考 scrcpy 架构，详见 `../docs/DESIGN.md`，协议见 `../proto/protocol.md`）。

## 编译（CI 自动：GitHub Actions + enlyze CeGCC，无需 Windows）

WinCE 程序**不再需要 Windows / VS2008**。仓库 `.github/workflows/build.yml` 的 `wince-build` job 在 `ghcr.io/enlyze/windows-ce-build-environment-arm` 容器里用 `arm-mingw32ce-g++`（CeGCC GCC 9.3）交叉编译，产出自包含（静态链接 libgcc/libstdc++）的 ARM WinCE 可执行文件 `GLOAI.exe`，并已校验 PE `machine=0x01C0` + `subsystem=9`。

本地如需手动编译（需先取得该容器）：

```bash
docker run --rm -v "$PWD":/work -w /work ghcr.io/enlyze/windows-ce-build-environment-arm \
  sh -c 'cd wince && chmod +x build_wince.sh && ./build_wince.sh'
```

`build_wince.sh` 直接用绝对路径调用 `/opt/cegcc-arm/bin/arm-mingw32ce-g++` 编译 `src/*.cpp`，链接 `ws2`（Winsock2），不传 `--subsystem`（arm-mingw32ce 默认即 Windows CE GUI 子系统，且默认入口为 `WinMain`）。

## 解码器集成

- **MJPEG（已实现，V1 默认）**：`src/nanojpeg.c` 是 vendor 进来的 **NanoJPEG**（Martin J. Fiedler, MIT，微型基线 JPEG 解码器，与 tinyjpeg 同类）。`decoder.cpp` 直接 `#include "nanojpeg.c"`，`decodeMJPEG()` 解出 24-bit RGB 后扩成 RGB32 供渲染器。每帧独立完整 JPEG，老车机也能解。
- **H264 / ffmpegce（明确延后）**：`decodeH264()` 为占位（返回 false）。ffmpeg 的 WinCE 移植体积与交叉编译复杂度较高，V1 不实装；如需更省带宽再接。

## 部署（产物已在 SD 卡）

CI 产物（APK + EXE）下载后放到 SD 卡，**零配置、无需手填 IP**：

1. `wince/GLOAI.exe` → 车机 SD 卡 `\SDMEMORY2\GLOAI\wince\`；
2. 车机用 Total Commander 进 `GLOAI\wince\` 双击 `GLOAI.exe`，或点根目录 `GLOAI.lnk`；
3. 手机装 `android/GLOAI-Mirror-v1.0.apk`，开权限后点“启动投屏服务”；
4. 车机端**自动发现**手机 IP 并连接（UDP 信标，详见 `../proto/protocol.md` §1.1）；USB 直连时走固定共享 IP `192.168.42.129`/`192.168.43.1`。

`config.txt` 仅作**可选显式覆盖**（一行 `host port`），留空则纯自动发现。

详见 SD 卡根目录 `README.md`。

## 运行

- 车机标题显示「自动发现手机…」→「已连接，镜像中」；
- 车机显示手机画面 → 触摸车机屏即可操控手机（单指点击/滑动）；
- **断线自动重连**：车机检测到连接断开即回到自动发现循环，无需重启。

## 模块

| 文件 | 职责 |
|---|---|
| `main.cpp` | WinMain、窗口、消息循环、触摸捕获、线程编排 |
| `net.cpp/.h` | TCP 客户端 + GLOA 协议（握手/视频/触摸/心跳）；含 UDP 自动发现（`:8687` 信标）+ `connectTimeout` 候选试探 |
| `renderer.cpp/.h` | GDI 渲染（CreateDIBSection + StretchBlt） |
| `decoder.cpp/.h` | 解码接口：`decodeMJPEG()` 已接 NanoJPEG；`decodeH264()` 占位 |
| `nanojpeg.c` | vendor 的 NanoJPEG 解码器（含 rordenlab lossless 16-bit 扩展，V1 不用） |
| `build_wince.sh` | CeGCC 交叉编译脚本（CI 调用） |

## 已知限制

- V1 仅单指；双指缩放为 V1.1；
- MJPEG 模式带宽较大（~3–6Mbps @ 800×480），H264 更省（待接）；
- 不支持音频（V2 计划）。
