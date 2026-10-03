#!/usr/bin/env bash
#
# tuptup.top 车机投屏 · 一次性 ADB 固化脚本（日常机版）
#
# 用途：在手机上“打开一次开发者模式”，把 tuptup.top 投屏 App 的配置一次性固化好，
#       之后插上 car 机线即可自动投屏，无需再进 App。
#
# 前置：
#   1. 手机已开启「开发者选项 → USB 调试」（首次连电脑会弹“允许调试”，勾选一律允许）。
#   2. 电脑已安装 adb，且手机通过 USB 连到本机。
#   3. 手机已安装 tuptup.top 投屏 App（见 CI 产物 tuptup-mirror-android-apk）。
#
# 运行：
#   adb devices            # 确认手机已列出（authorized）
#   bash scripts/bootstrap_adb.sh
#
# 说明（与“专用机全自动”方案的区别）：
#   本脚本【不】自动点掉系统“开始录屏”弹窗——那一步需你每次手机重启后手动点一次
#   （Android 安全限制，无法绕过）。除此之外（无障碍/权限/电池/网络共享）全部固化。
#   其中开启无障碍服务是为了“触摸回注”（车机点手机），并非自动点击弹窗。

set -u

PKG="top.tuptup.mirror"
A11Y="$PKG/.MirrorAccessibilityService"

echo "==> 检查 adb 与设备"
if ! command -v adb >/dev/null 2>&1; then
  echo "错误：找不到 adb，请先安装 Android platform-tools 并加入 PATH" >&2
  exit 1
fi

DEVS=$(adb devices | awk 'NR>1 && $2=="device"{print $1}')
if [ -z "$DEVS" ]; then
  echo "错误：没有已授权(authorized)的 adb 设备，请检查 USB 调试与授权弹窗" >&2
  exit 1
fi
echo "已连接设备：$DEVS"

echo "==> 1) 开启开发者选项（global development_settings_enabled=1）"
adb shell settings put global development_settings_enabled 1 || true

echo "==> 2) 授权通知权限（Android 13+，低版本忽略）"
adb shell pm grant "$PKG" android.permission.POST_NOTIFICATIONS 2>/dev/null || true

echo "==> 3) 允许悬浮窗（录屏/状态提示用）"
adb shell appops set "$PKG" SYSTEM_ALERT_WINDOW allow 2>/dev/null || true

echo "==> 4) 加入电池优化白名单（防后台冻结导致 8686 失活、车机连不上）"
adb shell "dumpsys deviceidle whitelist +$PKG" 2>/dev/null || true

echo "==> 5) 开启无障碍服务（用于车机→手机 触摸回注；非自动点击弹窗）"
adb shell settings put secure enabled_accessibility_services "$A11Y" || true
adb shell settings put secure accessibility_enabled 1 || true

echo ""
echo "==> 完成。建议随后在手机上手动确认："
echo "   - 设置 → 已安装应用 → tuptup.top 车机投屏 → 无障碍：应显示“已开启”"
echo "   - 设置 → 电池/应用启动管理：允许后台运行（部分国产 ROM 仍需手动放开自启动/后台)"
echo ""
echo "剩余唯一手动步骤：手机重启后，插上 car 机线，在手机弹出的系统“开始录屏”对话框点一次「开始」。"
echo "（若没自动弹出，点车机端常驻通知“点此授权录屏以开始车机投屏”即可。）"
