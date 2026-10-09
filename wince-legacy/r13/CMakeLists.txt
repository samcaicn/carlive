cmake_minimum_required(VERSION 3.10)
project(WinCEClient CXX)

# ============================================================================
# 源码组织 + 依赖声明。
# WinCE 接收端的交叉编译由 wince/build_wince.sh 完成，使用 enlyze CeGCC
#（arm-mingw32ce, GCC 9.3，容器 ghcr.io/enlyze/windows-ce-build-environment:arm），
# 产出 ARM WinCE (subsystem 9) 的自包含可执行文件，可在 GitHub Actions 直接构建。
# 本 CMakeLists 仍保留为「Windows + VS2008 + CE6 SDK」人工编译路径的参考。
# ============================================================================

set(SOURCES
    src/net.cpp
    src/renderer.cpp
    src/decoder.cpp
    src/main.cpp
)

add_executable(WinCEClient WIN32 ${SOURCES})
target_include_directories(WinCEClient PRIVATE src)

# WinCE 链接 Winsock2（ws2.lib）；MJPEG 解码器 NanoJPEG 已 #include 进 decoder.cpp，
# 不需要额外链接；若启用 H264 需 avcodec/avutil/swscale（ffmpegce）。
target_link_libraries(WinCEClient ws2)

# ---- 可选增强（按需开启）----
# option(USE_FFMPEG "Decode H264 via ffmpegce" OFF)
# option(USE_MJPEG  "Decode MJPEG via tinyjpeg" ON)
