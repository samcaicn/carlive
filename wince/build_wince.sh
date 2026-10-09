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

# R33：构建「干净诊断矩阵」test1-6（见下方 case 注释）。
# 旧的 full/usbnet/bisect-a/b/c 变体已废弃，统一由 test1-6 取代。
# 产物名随 variants 变化，使得日志文件名（由 EXE 名派生）也各自独立。
# R33：干净的 test1-6 诊断矩阵（覆盖全部嫌疑维度，互相正交）。
#   test1 = ADB + 复现(ConnThread 内调 readConfig) + 行级打点       → 阴性对照/复现闪退
#   test2 = ADB + 修复(WinMain 读全局, ConnThread 复用)             → 修复候选(主)
#   test3 = ADB + 复现 + R13 原样线程(CreateThread 0,0)            → 栈 reservation 标志嫌疑
#   test4 = ADB + 复现 + 不 spawn 发现线程(单线程)                 → 跨线程交互嫌疑
#   test5 = ADB + 复现 + 关闭 crashlog                             → crashlog 无锁并发写嫌疑
#   test6 = WiFi(USB_NET_ONLY) + 修复(WinMain 读全局)              → 修复候选(另一模式)
# 各 exe 日志名按 EXE 名派生，互不覆盖。
VARIANT="${VARIANT:-test2}"
case "$VARIANT" in
  test1)   # ADB, 复现: ConnThread 内调 readConfig + 行级打点
    EXTRA_FLAGS="-DTLTP_TEST1 -DTLTP_REPRO_CONF -DTLTP_TEST_FINE_STAGE"
    EXTRA_SRC=( "$SCRIPT_DIR/src/adb.cpp" "$SCRIPT_DIR/src/rsa.cpp" )
    OUT="$SCRIPT_DIR/test1.exe" ;;
  test2)   # ADB, 修复: config 在 WinMain 读全局, ConnThread 复用
    EXTRA_FLAGS="-DTLTP_TEST2"
    EXTRA_SRC=( "$SCRIPT_DIR/src/adb.cpp" "$SCRIPT_DIR/src/rsa.cpp" )
    OUT="$SCRIPT_DIR/test2.exe" ;;
  test3)   # ADB, 复现 + R13 原样线程(CreateThread 0,0)
    EXTRA_FLAGS="-DTLTP_TEST3 -DTLTP_REPRO_CONF -DTLTP_TEST_FINE_STAGE -DTLTP_PLAIN_THREAD"
    EXTRA_SRC=( "$SCRIPT_DIR/src/adb.cpp" "$SCRIPT_DIR/src/rsa.cpp" )
    OUT="$SCRIPT_DIR/test3.exe" ;;
  test4)   # ADB, 复现 + 不 spawn 发现线程(单线程)
    EXTRA_FLAGS="-DTLTP_TEST4 -DTLTP_REPRO_CONF -DTLTP_TEST_FINE_STAGE -DTLTP_TEST_NO_DISCOVERY"
    EXTRA_SRC=( "$SCRIPT_DIR/src/adb.cpp" "$SCRIPT_DIR/src/rsa.cpp" )
    OUT="$SCRIPT_DIR/test4.exe" ;;
  test5)   # ADB, 复现 + 关闭 crashlog
    EXTRA_FLAGS="-DTLTP_TEST5 -DTLTP_REPRO_CONF -DTLTP_TEST_FINE_STAGE -DTLTP_BISECT_NO_CRASHLOG"
    EXTRA_SRC=( "$SCRIPT_DIR/src/adb.cpp" "$SCRIPT_DIR/src/rsa.cpp" )
    OUT="$SCRIPT_DIR/test5.exe" ;;
  test6)   # WiFi(USB_NET_ONLY) 修复: config 在 WinMain 读全局
    EXTRA_FLAGS="-DTLTP_TEST6 -DUSB_NET_ONLY"
    EXTRA_SRC=()
    OUT="$SCRIPT_DIR/test6.exe" ;;
  *) echo "ERROR: 未知 VARIANT=$VARIANT（支持: test1-6）" >&2; exit 2 ;;
esac

echo ">> building $OUT  (VARIANT=$VARIANT)"
"$CC" -O2 -Wall -Wno-unused-function $EXTRA_FLAGS \
  "$SCRIPT_DIR/src/main.cpp" \
  "$SCRIPT_DIR/src/net.cpp" \
  "${EXTRA_SRC[@]+"${EXTRA_SRC[@]}"}" \
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
