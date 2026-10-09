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

# R27：VARIANT=usbnet 构建「单 USB 共享网络」变体。
# 该变体编译带 -DUSB_NET_ONLY：不含 ADB 隧道（adb.cpp/rsa.cpp 不进链接），
# 模式恒为 usb_net。用于真车 A/B 对比，排除 ADB 模块与 exe 体积的影响。
# 产物名随 variants 变化，使得日志文件名（由 EXE 名派生）也各自独立。
VARIANT="${VARIANT:-full}"
case "$VARIANT" in
  full)
    EXTRA_FLAGS=""
    EXTRA_SRC=( "$SCRIPT_DIR/src/adb.cpp" "$SCRIPT_DIR/src/rsa.cpp" )
    OUT="$SCRIPT_DIR/tuptup.exe"
    ;;
  usbnet)
    EXTRA_FLAGS="-DUSB_NET_ONLY"
    EXTRA_SRC=()
    OUT="$SCRIPT_DIR/tuptup-usbnet.exe"
    ;;
  # R30 二分定位：一轮 CI 出 3 个变体，每个只改一个嫌疑变量。
  #   bisect-a (ba) = CrashSetStage 空操作      → 验 crashlog 模块（无锁并发写）
  #   bisect-b (bb) = R13 原样线程创建          → 验 R23 栈 reservation 标志
  #   bisect-c (bc) = 显式 256KB 栈不带标志     → bb 存活时的修复候选
  bisect-a)
    EXTRA_FLAGS="-DTLTP_BISECT_NO_CRASHLOG"
    EXTRA_SRC=( "$SCRIPT_DIR/src/adb.cpp" "$SCRIPT_DIR/src/rsa.cpp" )
    OUT="$SCRIPT_DIR/tuptup-ba.exe"
    ;;
  bisect-b)
    EXTRA_FLAGS="-DTLTP_BISECT_PLAIN_THREAD"
    EXTRA_SRC=( "$SCRIPT_DIR/src/adb.cpp" "$SCRIPT_DIR/src/rsa.cpp" )
    OUT="$SCRIPT_DIR/tuptup-bb.exe"
    ;;
  bisect-c)
    EXTRA_FLAGS="-DTLTP_BISECT_BIGSTACK"
    EXTRA_SRC=( "$SCRIPT_DIR/src/adb.cpp" "$SCRIPT_DIR/src/rsa.cpp" )
    OUT="$SCRIPT_DIR/tuptup-bc.exe"
    ;;
  # R31 ADB 模式 5 连测：每个只改一个变量，test1-5.exe 逐个上车定位闪退根因。
  test1)   # crashlog 模块关闭 → 验 CrashSetStage 无锁并发写是否有罪
    EXTRA_FLAGS="-DTLTP_TEST1 -DTLTP_BISECT_NO_CRASHLOG"
    EXTRA_SRC=( "$SCRIPT_DIR/src/adb.cpp" "$SCRIPT_DIR/src/rsa.cpp" )
    OUT="$SCRIPT_DIR/test1.exe" ;;
  test2)   # 线程创建回退 R13 原样 → 验 R23 栈 reservation 标志是否有罪
    EXTRA_FLAGS="-DTLTP_TEST2 -DTLTP_BISECT_PLAIN_THREAD"
    EXTRA_SRC=( "$SCRIPT_DIR/src/adb.cpp" "$SCRIPT_DIR/src/rsa.cpp" )
    OUT="$SCRIPT_DIR/test2.exe" ;;
  test3)   # 256KB 大栈不带标志 → test2 存活时的修复候选
    EXTRA_FLAGS="-DTLTP_TEST3 -DTLTP_BISECT_BIGSTACK"
    EXTRA_SRC=( "$SCRIPT_DIR/src/adb.cpp" "$SCRIPT_DIR/src/rsa.cpp" )
    OUT="$SCRIPT_DIR/test3.exe" ;;
  test4)   # readConfig 内部行级打点 → 若仍闪退，crash.log 直接给出死点行
    EXTRA_FLAGS="-DTLTP_TEST4 -DTLTP_TEST_FINE_STAGE"
    EXTRA_SRC=( "$SCRIPT_DIR/src/adb.cpp" "$SCRIPT_DIR/src/rsa.cpp" )
    OUT="$SCRIPT_DIR/test4.exe" ;;
  test5)   # 不 spawn 任何发现线程（单线程探针，连不上手机）→ 验跨线程交互
    EXTRA_FLAGS="-DTLTP_TEST5 -DTLTP_TEST_NO_DISCOVERY"
    EXTRA_SRC=( "$SCRIPT_DIR/src/adb.cpp" "$SCRIPT_DIR/src/rsa.cpp" )
    OUT="$SCRIPT_DIR/test5.exe" ;;
  *) echo "ERROR: 未知 VARIANT=$VARIANT（支持: full | usbnet | bisect-a/b/c | test1-5）" >&2; exit 2 ;;
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
