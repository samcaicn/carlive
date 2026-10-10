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
    EXTRA_FLAGS="-DTLTP_TEST9 -DTLTP_DISC_NO_THREAD -DTLTP_LOG_STAGES"
    EXTRA_SRC=( "$SCRIPT_DIR/src/adb.cpp" "$SCRIPT_DIR/src/rsa.cpp" )
    OUT="$SCRIPT_DIR/test9.exe" ;;
  test10)  # ADB, 信标线程纯空转（无 Winsock）
    EXTRA_FLAGS="-DTLTP_TEST10 -DTLTP_DISC_NO_SOCKET -DTLTP_LOG_STAGES"
    EXTRA_SRC=( "$SCRIPT_DIR/src/adb.cpp" "$SCRIPT_DIR/src/rsa.cpp" )
    OUT="$SCRIPT_DIR/test10.exe" ;;
  test11)  # ADB, 线程内仅 socket() 即退出
    EXTRA_FLAGS="-DTLTP_TEST11 -DTLTP_DISC_SOCK_ONLY -DTLTP_LOG_STAGES"
    EXTRA_SRC=( "$SCRIPT_DIR/src/adb.cpp" "$SCRIPT_DIR/src/rsa.cpp" )
    OUT="$SCRIPT_DIR/test11.exe" ;;
  test12)  # ADB, 线程内 socket, 无 bind, select 空转
    EXTRA_FLAGS="-DTLTP_TEST12 -DTLTP_DISC_NO_BIND -DTLTP_LOG_STAGES"
    EXTRA_SRC=( "$SCRIPT_DIR/src/adb.cpp" "$SCRIPT_DIR/src/rsa.cpp" )
    OUT="$SCRIPT_DIR/test12.exe" ;;
  test13)  # ADB, 主线程 socket, 线程 bind+select
    EXTRA_FLAGS="-DTLTP_TEST13 -DTLTP_DISC_SOCK_MAIN -DTLTP_LOG_STAGES"
    EXTRA_SRC=( "$SCRIPT_DIR/src/adb.cpp" "$SCRIPT_DIR/src/rsa.cpp" )
    OUT="$SCRIPT_DIR/test13.exe" ;;
  test14)  # ADB, 主线程 socket+bind, 线程 select/recvfrom
    EXTRA_FLAGS="-DTLTP_TEST14 -DTLTP_DISC_SOCK_MAIN -DTLTP_DISC_BIND_MAIN -DTLTP_LOG_STAGES"
    EXTRA_SRC=( "$SCRIPT_DIR/src/adb.cpp" "$SCRIPT_DIR/src/rsa.cpp" )
    OUT="$SCRIPT_DIR/test14.exe" ;;
  test15)  # ADB, 线程内自 WSAStartup + 全套正常
    EXTRA_FLAGS="-DTLTP_TEST15 -DTLTP_DISC_WSA_SELF -DTLTP_LOG_STAGES"
    EXTRA_SRC=( "$SCRIPT_DIR/src/adb.cpp" "$SCRIPT_DIR/src/rsa.cpp" )
    OUT="$SCRIPT_DIR/test15.exe" ;;
  test17)  # R36：test9 基础上跳过 LoadKnownPhones —— 隔离 InitCS(g_csKnown)+known_phones.cfg 路径
    EXTRA_FLAGS="-DTLTP_TEST17 -DTLTP_DISC_NO_THREAD -DTLTP_DISC_NO_LOADKNOWN -DTLTP_LOG_STAGES"
    EXTRA_SRC=( "$SCRIPT_DIR/src/adb.cpp" "$SCRIPT_DIR/src/rsa.cpp" )
    OUT="$SCRIPT_DIR/test17.exe" ;;
  test18)  # R36：StartDiscovery 入口即返回 —— 隔离 InitCS(g_csCand) 与全部发现逻辑
    EXTRA_FLAGS="-DTLTP_TEST18 -DTLTP_DISC_NO_THREAD -DTLTP_DISC_NONE -DTLTP_LOG_STAGES"
    EXTRA_SRC=( "$SCRIPT_DIR/src/adb.cpp" "$SCRIPT_DIR/src/rsa.cpp" )
    OUT="$SCRIPT_DIR/test18.exe" ;;
  test19)  # R37：接近成功候选 A —— 跳过 Renderer 构造 + StartDiscovery 仅置 flag（不 spawn/不 LoadKnownPhones），保留 ConnThread 连接逻辑
    EXTRA_FLAGS="-DTLTP_TEST19 -DTLTP_DISC_NO_THREAD -DTLTP_DISC_NONE -DTLTP_SAFE_RENDERER -DTLTP_LOG_STAGES"
    EXTRA_SRC=( "$SCRIPT_DIR/src/adb.cpp" "$SCRIPT_DIR/src/rsa.cpp" )
    OUT="$SCRIPT_DIR/test19.exe" ;;
  test20)  # R37：诊断 —— 跳过 NetClient 构造（ConnThread 判空直接退出），进程存活对照，验证网络栈路径是否踩堆
    EXTRA_FLAGS="-DTLTP_TEST20 -DTLTP_DISC_NO_THREAD -DTLTP_SAFE_NETCLIENT -DTLTP_LOG_STAGES"
    EXTRA_SRC=( "$SCRIPT_DIR/src/adb.cpp" "$SCRIPT_DIR/src/rsa.cpp" )
    OUT="$SCRIPT_DIR/test20.exe" ;;
  test24)  # R39 修复候选A：FULL 功能（Renderer+NetClient+Discovery+ConnThread）+ 统一文件锁（TLTP_UNIFY_FILELOCK）
    # 把 Log(gloai.log) 与 CrashSetStage(crash.log) 串行到同一把锁，消除跨锁并发写文件踩堆；保留全部诊断。
    EXTRA_FLAGS="-DTLTP_TEST24 -DTLTP_UNIFY_FILELOCK -DTLTP_LOG_STAGES"
    EXTRA_SRC=( "$SCRIPT_DIR/src/adb.cpp" "$SCRIPT_DIR/src/rsa.cpp" )
    OUT="$SCRIPT_DIR/test24.exe" ;;
  test25)  # R39 修复候选B：FULL 功能 + 去掉 TLTP_LOG_STAGES（去掉每行交叉 crash 写），crash.log 仅 stage 变化时写
    EXTRA_FLAGS="-DTLTP_TEST25"
    EXTRA_SRC=( "$SCRIPT_DIR/src/adb.cpp" "$SCRIPT_DIR/src/rsa.cpp" )
    OUT="$SCRIPT_DIR/test25.exe" ;;
  test26)  # R39 修复候选C：FULL 功能 + 彻底关掉 crash.log（TLTP_BISECT_NO_CRASHLOG）+ 去 TLTP_LOG_STAGES，单文件单锁最干净
    EXTRA_FLAGS="-DTLTP_TEST26 -DTLTP_BISECT_NO_CRASHLOG"
    EXTRA_SRC=( "$SCRIPT_DIR/src/adb.cpp" "$SCRIPT_DIR/src/rsa.cpp" )
    OUT="$SCRIPT_DIR/test26.exe" ;;
  test27)  # R40 根因修复验证：FULL 功能 + 统一文件锁 + 逐行诊断 + malloc_lock.cpp（newlib malloc 全局串行化）
    # 假设根因 = CeGCC/newlib 的 malloc 非线程安全，多线程并发 new/delete 损坏堆 → 任意后续堆操作崩。
    # malloc_lock.cpp 给全局堆分配加递归 CS；若 test27 不崩且能连手机，则假设成立、定稿。
    EXTRA_FLAGS="-DTLTP_TEST27 -DTLTP_UNIFY_FILELOCK -DTLTP_LOG_STAGES"
    EXTRA_SRC=( "$SCRIPT_DIR/src/adb.cpp" "$SCRIPT_DIR/src/rsa.cpp" "$SCRIPT_DIR/src/malloc_lock.cpp" )
    OUT="$SCRIPT_DIR/test27.exe" ;;
  *) echo "ERROR: 未知 VARIANT=$VARIANT（支持: test9-15,17-20,24-27；test16 由 CI 单独从 wince-legacy/r13 构建；test23 由 r13proto 构建）" >&2; exit 2 ;;
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
