#!/bin/bash
# 在 CE7 模拟器里打开 cmd 并运行指定程序（headless 脚本化操作）。
# 用法: ./tools/ce-run.sh [命令行]      默认: navit.exe -d 4 > nlog.txt
#
# 为什么不用 GUI 双击：BSP 没有自启动机制，且 explorer 里导航要靠盲点；
# 走 Run -> cmd -> 命令行最可靠，还能重定向 stdout 拿日志。
set -u
P=/Users/k/.workbuddy/binaries/python/versions/3.13.12/bin/python3
QMP=${QMP:-14445}
cd "$(dirname "$0")/.."
CMD="${1:-navit.exe -d 4 > nlog.txt}"

echo "[1/3] 打开 Run 并启动 cmd.exe"
TEXT_DELAY=0.1 "$P" tools/qmp_input.py --port "$QMP" \
    combo,ctrl-esc wait,2 click,60,534 wait,3 text,cmd key,ret wait,12 >/dev/null

echo "[2/3] cd 到数据盘 navit 目录（故意用小写：CE 上 shift-大写字母会丢键）"
TEXT_DELAY=0.12 "$P" tools/qmp_input.py --port "$QMP" \
    'text,cd "\mounted volume\navit"' key,ret wait,3 >/dev/null

echo "[3/3] 执行: $CMD"
TEXT_DELAY=0.1 "$P" tools/qmp_input.py --port "$QMP" "text,$CMD" key,ret >/dev/null
echo "[*] 已发送"
