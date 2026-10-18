#!/usr/bin/env bash
# SPDX-License-Identifier: GPL-2.0-only
set -euo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
KERNEL_SRC="${KERNEL_SRC:-$ROOT/.work/kernel}"
OUT_DIR="${OUT_DIR:-$ROOT/out/kernel}"
TARGET_RELEASE="5.10.252-gki-g19f54b03cc87"
JOBS="${JOBS:-$(nproc)}"
MAKE_ARGS=(ARCH=arm64 LLVM=1 LLVM_IAS=1)

if [[ ! -f "$KERNEL_SRC/Makefile" ]]; then
    echo "错误：KERNEL_SRC 不是内核源码树：$KERNEL_SRC" >&2
    exit 2
fi
if ! grep -q '^SUBLEVEL = 252$' "$KERNEL_SRC/Makefile"; then
    echo "错误：内核源码不是 Linux 5.10.252" >&2
    exit 1
fi

mkdir -p "$OUT_DIR"

# 1. Lay down the in-tree VM manager sources and Kbuild hooks.
KERNEL_SRC="$KERNEL_SRC" "$ROOT/scripts/apply_intree.sh"

# 2. Start from the device config so every vendor driver stays enabled.
gzip -dc "$ROOT/config/config.gz" > "$KERNEL_SRC/.config"
"$KERNEL_SRC/scripts/config" --file "$KERNEL_SRC/.config" \
    --disable LOCALVERSION_AUTO \
    --set-str LOCALVERSION "-gki-g19f54b03cc87" \
    --enable MODULES \
    --enable MODVERSIONS \
    --disable MODULE_SIG \
    --enable MODULE_SCMVERSION \
    --disable LTO_CLANG_FULL \
    --enable LTO_CLANG_THIN \
    --enable GH_VM_MGR

printf '%s\n' 'g19f54b03cc87' > "$KERNEL_SRC/.scmversion"
export LOCALVERSION=
export KBUILD_BUILD_USER=keosh
export KBUILD_BUILD_HOST=ubuntu

make -C "$KERNEL_SRC" "${MAKE_ARGS[@]}" olddefconfig
RELEASE="$(make -s -C "$KERNEL_SRC" "${MAKE_ARGS[@]}" kernelrelease)"
if [[ "$RELEASE" != "$TARGET_RELEASE" ]]; then
    echo "错误：内核 release 为 $RELEASE，预期 $TARGET_RELEASE" >&2
    exit 1
fi

# 3. Sanity check: the VM manager must actually be compiled in.
if ! grep -q '^CONFIG_GH_VM_MGR=' "$KERNEL_SRC/.config"; then
    echo "错误：CONFIG_GH_VM_MGR 未出现在 .config" >&2
    exit 1
fi

make -C "$KERNEL_SRC" "${MAKE_ARGS[@]}" -j"$JOBS" Image

install -m 0644 "$KERNEL_SRC/arch/arm64/boot/Image" "$OUT_DIR/Image"
install -m 0644 "$KERNEL_SRC/System.map" "$OUT_DIR/System.map"
install -m 0644 "$KERNEL_SRC/.config" "$OUT_DIR/kernel.config"
for symvers in vmlinux.symvers Module.symvers; do
    if [[ -f "$KERNEL_SRC/$symvers" ]]; then
        install -m 0644 "$KERNEL_SRC/$symvers" "$OUT_DIR/$symvers"
    fi
done

echo "已生成 $OUT_DIR/Image"
file "$OUT_DIR/Image"
sha256sum "$OUT_DIR/Image"

# 4. Confirm the driver made it into the image.
if command -v nm >/dev/null 2>&1 && [[ -f "$KERNEL_SRC/vmlinux" ]]; then
    if nm "$KERNEL_SRC/vmlinux" 2>/dev/null | grep -q 'gh_vm_mgr_init'; then
        echo "OK: gh_vm_mgr_init present in vmlinux"
    else
        echo "警告：vmlinux 中未找到 gh_vm_mgr_init" >&2
    fi
fi
