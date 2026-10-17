#!/system/bin/sh
# SPDX-License-Identifier: GPL-2.0-only
MODDIR=${0%/*}
STATE=/data/adb/k60_gunyah_probe

mkdir -p "$STATE" 2>/dev/null
if [ -f "$MODDIR/autoload" ]; then
    rm -f "$MODDIR/autoload"
    printf '%s\n' "下一次开机：关闭 Gunyah 探测模块自动加载"
else
    touch "$MODDIR/autoload"
    rm -f "$STATE/fail_count" "$STATE/boot_ok" "$MODDIR/disable"
    printf '%s\n' "下一次开机：启用 Gunyah 探测模块自动加载（已重置失败计数）"
fi
printf '%s\n' "当前已加载状态：$(test -d /sys/module/gunyah_probe && echo yes || echo no)"
