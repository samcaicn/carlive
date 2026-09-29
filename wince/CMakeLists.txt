cmake_minimum_required(VERSION 3.10)
project(WinCEClient CXX)

# ============================================================================
# 注意：本 CMakeLists 仅是「源码组织 + 依赖声明」。
# WinCE 的官方/可用编译路径是 Windows + Visual Studio 2008 + Windows CE 6.0 SDK
# + ARMv4I 交叉工具链（见 wince/README.md）。GitHub Actions 没有 WinCE 工具链，
# 故 CI 只对 Android 端构建；WinCE 端在此处做静态检查/配置校验。
# ============================================================================

set(SOURCES
    src/net.cpp
    src/renderer.cpp
    src/decoder.cpp
    src/main.cpp
)

add_executable(WinCEClient WIN32 ${SOURCES})
target_include_directories(WinCEClient PRIVATE src)

# WinCE 链接 Winsock2（ws2.lib）；若启用 H264 需 avcodec/avutil/swscale；
# 若启用 MJPEG 需把 tinyjpeg.c 加入 SOURCES 并链接其依赖。
target_link_libraries(WinCEClient ws2)

# ---- 可选增强（按需开启）----
# option(USE_FFMPEG "Decode H264 via ffmpegce" OFF)
# option(USE_MJPEG  "Decode MJPEG via tinyjpeg" ON)
