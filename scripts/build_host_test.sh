#!/usr/bin/env bash
# build_host_test.sh - 在宿主机（macOS / Linux，POSIX）编译 AdbTransport 端到端自测程序。
#
# 与 wince/src/adb.cpp 是【同一份实现】，仅通过平台宏切到 POSIX 分支（syssync.h / log_stub.cpp）。
# 真正交付到车机的 ARM WinCE 二进制由 CI（CeGCC）产出，这里只验证传输层“逻辑正确性”。
#
# 用法：
#   scripts/build_host_test.sh            # 产出 build/adbtest（Release，无调试桩）
#   scripts/build_host_test.sh --debug    # 额外产出 build/adbtest_dbg（带 ADB_DEBUG 跟踪）
#
# 然后：
#   python3 scripts/run_host_tests.py build/adbtest
set -e
cd "$(dirname "$0")/.."
mkdir -p build

SRC=(scripts/adbtest.cpp wince/src/adb.cpp wince/src/rsa.cpp scripts/log_stub.cpp)
INC=(-I wince/src)
CXX="${CXX:-c++}"
STD=-std=c++17

echo "[build] CXX=$CXX 编译 Release -> build/adbtest"
$CXX $STD -O2 "${INC[@]}" "${SRC[@]}" -o build/adbtest -lpthread

if [ "$1" = "--debug" ]; then
    echo "[build] CXX=$CXX 编译 Debug(ADB_DEBUG) -> build/adbtest_dbg"
    $CXX $STD -O0 -g -DADB_DEBUG "${INC[@]}" "${SRC[@]}" -o build/adbtest_dbg -lpthread
fi

echo "[build] done."
