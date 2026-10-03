#!/bin/sh
# 把本地目录打包成 CE 可挂载的数据盘（MBR + FAT32），供模拟器使用。
#
# 用法:
#   ./make-datadisk.sh <源目录> [输出img] [卷标] [大小MB]
# 例:
#   ./make-datadisk.sh ~/myapps images/data.img MYAPPS 900
set -e
cd "$(dirname "$0")"

SRC="$1"
OUT_REL="${2:-images/data.img}"
LABEL="${3:-WINCEAPPS}"
MB="${4:-900}"
IMG="${IMAGE:-wince-ce7-arm:v1}"

if [ -z "$SRC" ] || [ ! -d "$SRC" ]; then
  echo "用法: $0 <源目录> [输出img] [卷标] [大小MB]" >&2
  exit 1
fi
OUT_ABS="$(cd "$(dirname "$OUT_REL")" && pwd)/$(basename "$OUT_REL")"
SRC_ABS="$(cd "$SRC" && pwd)"

echo "[*] 源: $SRC_ABS"
echo "[*] 目标: $OUT_ABS  (卷标=$LABEL, ${MB}MB)"

# 1) 建空镜像
docker run --rm --entrypoint sh \
  -v "$SRC_ABS":/src:ro -v "$(dirname "$OUT_ABS")":/out \
  "$IMG" -c "dd if=/dev/zero of=/out/$(basename "$OUT_ABS") bs=1M count=$MB 2>/dev/null; echo '[*] 空镜像已建'"

# 2) 写 MBR（mkmbr.py 与本脚本同目录；脚本开头已 cd 到本目录）
PY=/Users/k/.workbuddy/binaries/python/versions/3.13.12/bin/python3
if [ -x "$PY" ]; then
  "$PY" ./mkmbr.py "$OUT_ABS" 2048
else
  python3 ./mkmbr.py "$OUT_ABS" 2048
fi

# 3) 格式化 + 拷贝
docker run --rm --entrypoint sh \
  -v "$SRC_ABS":/src:ro -v "$(dirname "$OUT_ABS")":/out \
  "$IMG" -c "
    set -e
    O=/out/$(basename "$OUT_ABS")@@2048s
    mformat -i \$O -F -v $LABEL ::
    for d in /src/*/; do
      name=\$(basename \"\$d\")
      mmd -i \$O ::/\$name
      mcopy -s -i \$O \"\$d\"* ::/\$name/ 2>/dev/null || mcopy -s -i \$O \"\$d\". ::/\$name/
      echo \"  + \$name\"
    done
    for f in /src/*; do
      [ -f \"\$f\" ] && { mcopy -i \$O \"\$f\" ::/ ; echo \"  + \$(basename \$f)\"; }
    done
    echo '--- 根目录 ---'
    mdir -i \$O ::
  "

echo "[*] 完成: $OUT_ABS"
echo "    重启模拟器生效: DISK_IMAGE=/wince/images/$(basename "$OUT_ABS") ./start.sh"
