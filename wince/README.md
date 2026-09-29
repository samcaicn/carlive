# GLOAI 车机投屏 · WinCE 接收端（ARM WinCE 6.0）

车机端程序 `WinCEClient.exe`，接收安卓手机投来的屏幕画面并渲染，同时把车机触摸回传给手机（参考 scrcpy 架构，详见 `../docs/DESIGN.md`，协议见 `../proto/protocol.md`）。

## 编译（必需：Windows + VS2008 + CE6 SDK）

GitHub Actions / macOS **无法编译 WinCE 程序**（无 ARM CE 交叉工具链）。请在 Windows 上：

1. 安装 **Visual Studio 2008 + Windows Embedded Compact 6.0 SDK + ARMv4I 设备 SDK**；
2. 新建 **Win32 (Windows CE) Smart Device** 工程，导入 `src/*.cpp/.h`；
3. 链接 `ws2.lib`（Winsock2）、`coredll.lib`（系统）；
4. 平台选 **ARMv4I / WinCE 6.0**；
5. 编译出 `WinCEClient.exe`。

> 也可把 `CMakeLists.txt` 导入 CMake-GUI，指定 CE 工具链文件（toolchain 指向 VS2008 + CE6 SDK）。

## 解码器集成（二选一或都接）

- **MJPEG（默认，V1 推荐）**：把 `tinyjpeg` / `tjpgd`（轻量 JPEG 解码，几百行 C）加入工程，实现 `decoder.cpp` 的 `decodeMJPEG()`，输出 RGB32。无需 ffmpeg，老车机也能跑。
- **H264（可选增强）**：集成 **ffmpeg 的 WinCE 移植**（ffmpegce / 0.6–2.x 的 CE 构建），实现 `decodeH264()`：`av_parser_parse2` 切 NALU → `avcodec_decode_video2` → `sws_scale` 转 RGB32。画质/带宽更优。

## 部署

1. 把 `WinCEClient.exe` 拷到车机 SD 卡（如 `\SDMEMORY2\GLOAI\`）；
2. 同目录放 `config.txt`，内容填手机 IP 与端口，例如：
   ```
   192.168.1.100 8686
   ```
   （也支持 `host:port` 或仅 `host`）；
3. 在善领 DSA 一机多图 / 车机导航路径里，把「导航路径」指向 `WinCEClient.exe`；
4. 车机连上手机热点或同一 WiFi；手机端打开 GLOAI App 开始镜像。

## 运行

- 车机显示手机画面 → 触摸车机屏即可操控手机（单指点击/滑动）；
- 断线自动退出；重连需重启程序。

## 模块

| 文件 | 职责 |
|---|---|
| `main.cpp` | WinMain、窗口、消息循环、触摸捕获、线程编排 |
| `net.cpp/.h` | TCP 客户端 + GLOA 协议（握手/视频/触摸/心跳） |
| `renderer.cpp/.h` | GDI 渲染（CreateDIBSection + StretchBlt） |
| `decoder.cpp/.h` | 解码接口（MJPEG / H264 框架，需集成第三方库） |

## 已知限制

- V1 仅单指；双指缩放为 V1.1；
- MJPEG 模式带宽较大（~3–6Mbps @ 800×480），H264 更省；
- 不支持音频（V2 计划）。
