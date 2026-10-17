#!/system/bin/sh
# SPDX-License-Identifier: GPL-2.0-only
# 安装前做最保守的机型/内核校验；不读取任何用户数据。
ARCH_OK=0
KERNEL_OK=0

case "$ARCH" in
    arm64|aarch64) ARCH_OK=1 ;;
esac
[ "$(uname -r)" = "5.10.252-gki-g19f54b03cc87" ] && KERNEL_OK=1

if [ "$ARCH_OK" -ne 1 ]; then
    abort "仅支持 arm64 设备"
fi
if [ "$KERNEL_OK" -ne 1 ]; then
    abort "内核版本不匹配，预期 5.10.252-gki-g19f54b03cc87"
fi

set_perm_recursive "$MODPATH" 0 0 0755 0644
set_perm "$MODPATH/service.sh" 0 0 0755
set_perm "$MODPATH/action.sh" 0 0 0755
set_perm "$MODPATH/uninstall.sh" 0 0 0755
set_perm "$MODPATH/gunyah_probe.ko" 0 0 0644
set_perm "$MODPATH/gh_vmmgr.ko" 0 0 0644
