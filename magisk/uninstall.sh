#!/system/bin/sh
# SPDX-License-Identifier: GPL-2.0-only
if [ -d /sys/module/gunyah_probe ]; then
    rmmod gunyah_probe 2>/dev/null || true
fi
rm -rf /data/adb/k60_gunyah_probe
