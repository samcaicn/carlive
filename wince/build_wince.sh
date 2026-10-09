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
# R34：信标线程 Winsock 窗口二分电池（test9-16）。
# 真车日志钉死的死亡窗口 = DiscoveryThread entered 之后、bind 结果日志之前
# （这段只有 socket()/setsockopt()/bind() 几个网络栈调用）；test8 证明与 ConnThread 无关
# （延迟 3s 未建 ConnThread 也死），test6 证明 usb_net 扫描线程死在 GetAdaptersInfo。
#   test9  = 真·无信标线程（对照：无任何后台发现）                → 元凶在不在 DiscoveryThread？
#   test10 = 信标线程纯空转（无任何 Winsock 调用）               → 网络栈调用有罪？
#   test11 = 线程内仅 socket()+closesocket 即退出                → 单"线程内 socket()"有罪？
#   test12 = 线程内 socket，无 bind，select 空转                 → 单"线程内 bind()"有罪？
#   test13 = 主线程 socket，线程 bind+select                     → socket 的线程亲和性
#   test14 = 主线程 socket+bind，线程 select/recvfrom            → bind 的线程亲和性
#   test15 = 线程内自行 WSAStartup + 全套正常                    → 每线程 Winsock 初始化
#   test16 = R13 原版重建对照（源码 be6ff77，CI 单独步骤构建）
# 各 exe 日志名按 EXE 名派生，互不覆盖。
VARIANT="${VARIANT:-test9}"
case "$VARIANT" in
  test9)   # ADB, 真·无信标线程（对照）
    EXTRA_FLAGS="-DTLTP_TEST9 -DTLTP_DISC_NO_THREAD"
    EXTRA_SRC=( "$SCRIPT_DIR/src/adb.cpp" "$SCRIPT_DIR/src/rsa.cpp" )
    OUT="$SCRIPT_DIR/test9.exe" ;;
  test10)  # ADB, 信标线程纯空转（无 Winsock）
    EXTRA_FLAGS="-DTLTP_TEST10 -DTLTP_DISC_NO_SOCKET"
    EXTRA_SRC=( "$SCRIPT_DIR/src/adb.cpp" "$SCRIPT_DIR/src/rsa.cpp" )
    OUT="$SCRIPT_DIR/test10.exe" ;;
  test11)  # ADB, 线程内仅 socket() 即退出
    EXTRA_FLAGS="-DTLTP_TEST11 -DTLTP_DISC_SOCK_ONLY"
    EXTRA_SRC=( "$SCRIPT_DIR/src/adb.cpp" "$SCRIPT_DIR/src/rsa.cpp" )
    OUT="$SCRIPT_DIR/test11.exe" ;;
  test12)  # ADB, 线程内 socket, 无 bind, select 空转
    EXTRA_FLAGS="-DTLTP_TEST12 -DTLTP_DISC_NO_BIND"
    EXTRA_SRC=( "$SCRIPT_DIR/src/adb.cpp" "$SCRIPT_DIR/src/rsa.cpp" )
    OUT="$SCRIPT_DIR/test12.exe" ;;
  test13)  # ADB, 主线程 socket, 线程 bind+select
    EXTRA_FLAGS="-DTLTP_TEST13 -DTLTP_DISC_SOCK_MAIN"
    EXTRA_SRC=( "$SCRIPT_DIR/src/adb.cpp" "$SCRIPT_DIR/src/rsa.cpp" )
    OUT="$SCRIPT_DIR/test13.exe" ;;
  test14)  # ADB, 主线程 socket+bind, 线程 select/recvfrom
    EXTRA_FLAGS="-DTLTP_TEST14 -DTLTP_DISC_SOCK_MAIN -DTLTP_DISC_BIND_MAIN"
    EXTRA_SRC=( "$SCRIPT_DIR/src/adb.cpp" "$SCRIPT_DIR/src/rsa.cpp" )
    OUT="$SCRIPT_DIR/test14.exe" ;;
  test15)  # ADB, 线程内自 WSAStartup + 全套正常
    EXTRA_FLAGS="-DTLTP_TEST15 -DTLTP_DISC_WSA_SELF"
    EXTRA_SRC=( "$SCRIPT_DIR/src/adb.cpp" "$SCRIPT_DIR/src/rsa.cpp" )
    OUT="$SCRIPT_DIR/test15.exe" ;;
  *) echo "ERROR: 未知 VARIANT=$VARIANT（支持: test9-15；test16 由 CI 单独从 wince-legacy/r13 构建）" >&2; exit 2 ;;
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
