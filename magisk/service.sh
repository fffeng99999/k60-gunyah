#!/system/bin/sh
# SPDX-License-Identifier: GPL-2.0-only
# 默认关闭自动加载；只有存在 autoload 标记时才在开机阶段 insmod。
MODDIR=${0%/*}
STATE=/data/adb/k60_gunyah_probe
KO="$MODDIR/gunyah_probe.ko"
MAX_FAILURES=2

mkdir -p "$STATE" 2>/dev/null
log_msg() {
    printf '%s %s\n' "$(date '+%Y-%m-%d %H:%M:%S')" "$*" >> "$STATE/service.log"
}

# 上一次开机成功则清零；boot_ok 只有在 sys.boot_completed=1 后才写入。
if [ -f "$STATE/boot_ok" ]; then
    rm -f "$STATE/boot_ok"
    printf '0\n' > "$STATE/fail_count"
fi

if [ -f "$MODDIR/disable" ] || [ -f "$MODDIR/remove" ]; then
    log_msg "module disabled; skip"
    exit 0
fi
if [ ! -f "$KO" ]; then
    log_msg "missing gunyah_probe.ko; skip"
    exit 0
fi
if [ ! -f "$MODDIR/autoload" ]; then
    log_msg "autoload not enabled; manual insmod only"
    exit 0
fi

for dep in gh_rm_drv gh_arm_drv gh_msgq gh_dbl; do
    if ! grep -q "^$dep " /proc/modules; then
        log_msg "dependency $dep not loaded; skip"
        exit 0
    fi
done

if insmod "$MODDIR/gh_vmmgr.ko"; then
    log_msg "gh_vmmgr loaded"
else
    log_msg "gh_vmmgr insmod failed (exit $?)"
    exit 0
fi

failures="$(cat "$STATE/fail_count" 2>/dev/null || printf '0')"
case "$failures" in
    ''|*[!0-9]*) failures=0 ;;
esac
failures=$((failures + 1))
printf '%s\n' "$failures" > "$STATE/fail_count"

if [ "$failures" -ge "$MAX_FAILURES" ]; then
    touch "$MODDIR/disable"
    log_msg "failure count $failures reached limit; module auto-disabled"
    exit 0
fi

rm -f "$STATE/boot_ok"
if insmod "$KO"; then
    log_msg "insmod succeeded; waiting for boot completion"
    (
        i=0
        while [ "$(getprop sys.boot_completed)" != "1" ] && [ "$i" -lt 240 ]; do
            sleep 2
            i=$((i + 1))
        done
        if [ "$(getprop sys.boot_completed)" = "1" ]; then
            date '+%s' > "$STATE/boot_ok"
            printf '0\n' > "$STATE/fail_count"
            log_msg "boot completed; module marked stable for this boot"
        else
            log_msg "boot completion timeout; stability not confirmed"
        fi
    ) >/dev/null 2>&1 &
else
    log_msg "insmod failed (exit $?)"
fi
