#!/usr/bin/env bash
# SPDX-License-Identifier: GPL-2.0-only
set -euo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
KERNEL_SRC="${KERNEL_SRC:-$ROOT/.work/kernel}"
OUT_DIR="${OUT_DIR:-$ROOT/out}"
TARGET_RELEASE="5.10.252-gki-g19f54b03cc87"
TARGET_VERMAGIC="$TARGET_RELEASE SMP preempt mod_unload modversions aarch64"
JOBS="${JOBS:-$(nproc)}"
MAKE_ARGS=(ARCH=arm64 LLVM=1 LLVM_IAS=1)

if [[ ! -f "$KERNEL_SRC/Makefile" ]]; then
    echo "错误：KERNEL_SRC 不是内核源码树：$KERNEL_SRC" >&2
    exit 2
fi
if [[ ! -f "$ROOT/config/config.gz" ]]; then
    echo "错误：缺少 config/config.gz" >&2
    exit 2
fi
if [[ ! -f "$ROOT/config/Module.symvers" ]]; then
    echo "错误：缺少 config/Module.symvers" >&2
    exit 2
fi

mkdir -p "$OUT_DIR"

# 内核源码由 CI 固定到 Linux 5.10.252；这里再次确认源码版本，
# 避免误用其他内核树生成错误 ABI。
grep -q '^SUBLEVEL = 252$' "$KERNEL_SRC/Makefile" || {
    echo "错误：内核源码不是 5.10.252" >&2
    exit 1
}

gzip -dc "$ROOT/config/config.gz" > "$KERNEL_SRC/.config"
"$KERNEL_SRC/scripts/config" --file "$KERNEL_SRC/.config" \
    --disable LOCALVERSION_AUTO \
    --set-str LOCALVERSION "-gki-g19f54b03cc87" \
    --enable MODULES \
    --enable MODVERSIONS \
    --disable MODULE_SIG \
    --enable MODULE_SCMVERSION

printf '%s\n' 'g19f54b03cc87' > "$KERNEL_SRC/.scmversion"

# setlocalversion 在 LOCALVERSION 未设置时会追加 "+"；显式导出空值。
export LOCALVERSION=

make -C "$KERNEL_SRC" "${MAKE_ARGS[@]}" olddefconfig
RELEASE="$(make -s -C "$KERNEL_SRC" "${MAKE_ARGS[@]}" kernelrelease)"
if [[ "$RELEASE" != "$TARGET_RELEASE" ]]; then
    echo "错误：内核 release 为 $RELEASE，预期 $TARGET_RELEASE" >&2
    exit 1
fi

make -C "$KERNEL_SRC" "${MAKE_ARGS[@]}" -j"$JOBS" modules_prepare
install -m 0644 "$ROOT/config/Module.symvers" "$KERNEL_SRC/Module.symvers"
make -C "$KERNEL_SRC" "${MAKE_ARGS[@]}" -j"$JOBS" M="$ROOT/kernel" modules

install -m 0644 "$ROOT/kernel/gunyah_probe.ko" "$OUT_DIR/gunyah_probe.ko"

MODULE_VERMAGIC="$(modinfo -F vermagic "$OUT_DIR/gunyah_probe.ko" 2>/dev/null || true)"
if [[ -n "$MODULE_VERMAGIC" && "$MODULE_VERMAGIC" != "$TARGET_VERMAGIC" ]]; then
    echo "错误：产物 vermagic 不匹配：$MODULE_VERMAGIC" >&2
    exit 1
fi

echo "已生成 $OUT_DIR/gunyah_probe.ko"
if [[ -n "$MODULE_VERMAGIC" ]]; then
    echo "vermagic: $MODULE_VERMAGIC"
fi
