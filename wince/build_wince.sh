#!/usr/bin/env bash
# 用 enlyze CeGCC（arm-mingw32ce，GCC 9.3）交叉编译 tuptup.top 车机端为 ARM WinCE 可执行文件。
# 依赖：ghcr.io/enlyze/windows-ce-build-environment-arm 容器（已含工具链与 WinCE 头/库）。
# 产物 tuptup.exe 为自包含二进制（静态链接 libgcc/libstdc++），可直接拷到车机 SD 卡运行。
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
cd "$SCRIPT_DIR"

CEGCC_BIN="${CEGCC_BIN:-/opt/cegcc-arm/bin}"
CC="$CEGCC_BIN/arm-mingw32ce-g++"

echo ">> CWD = $SCRIPT_DIR"
echo ">> CC  = $CC"
ls -la src || true

OUT="$SCRIPT_DIR/tuptup.exe"
echo ">> building $OUT"
"$CC" -O2 -Wall -Wno-unused-function \
  "$SCRIPT_DIR/src/main.cpp" \
  "$SCRIPT_DIR/src/net.cpp" \
  "$SCRIPT_DIR/src/adb.cpp" \
  "$SCRIPT_DIR/src/rsa.cpp" \
  "$SCRIPT_DIR/src/tcptransport.cpp" \
  "$SCRIPT_DIR/src/renderer.cpp" \
  "$SCRIPT_DIR/src/decoder.cpp" \
  "$SCRIPT_DIR/src/log.cpp" \
  "$SCRIPT_DIR/src/crashlog.cpp" \
  -o "$OUT" \
  -lws2 -liphlpapi \
  -static-libgcc -static-libstdc++
# 注意：-liphlpapi 必须带上（net.cpp 用 GetAdaptersInfo 枚举网卡找网关），
# 缺了会 undefined reference to `GetAdaptersInfo` 链接失败。
# 注意：arm-mingw32ce 工具链默认子系统即 Windows CE（PE Subsystem=9, WINDOWS_CE_GUI），
# 且默认入口为 WinMain（见 cegcc 文档）。显式写 --subsystem,windowsce 反而被 ld 拒绝，
# 故不传该 flag，由工具链默认值产出 CE 可执行文件。

echo ">> done: $(ls -la "$OUT" | awk '{print $5, $9}')"
