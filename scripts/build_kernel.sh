#!/usr/bin/env bash
# SPDX-License-Identifier: GPL-2.0-only
set -euo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
KERNEL_SRC="${KERNEL_SRC:-$ROOT/.work/kernel}"
OUT_DIR="${OUT_DIR:-$ROOT/out/kernel}"
TARGET_RELEASE="5.10.252-gki-g19f54b03cc87"
JOBS="${JOBS:-$(nproc)}"
MAKE_ARGS=(ARCH=arm64 LLVM=1 LLVM_IAS=1)

# ccache cuts repeat build time from ~20 minutes to a couple of minutes,
# which matters because each iteration on the VM manager needs a fresh
# kernel Image. CI restores the cache; a local run just benefits if ccache
# happens to be installed.
if command -v ccache >/dev/null 2>&1; then
    export CCACHE_DIR="${CCACHE_DIR:-$HOME/.ccache}"
    export CCACHE_MAXSIZE="${CCACHE_MAXSIZE:-5G}"
    export CCACHE_COMPRESS=1
    MAKE_ARGS+=(CC="ccache clang" HOSTCC="ccache gcc")
    echo "ccache enabled (dir=$CCACHE_DIR max=$CCACHE_MAXSIZE)"
    ccache --show-stats | head -5 || true
fi

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
    --module GH_VM_MGR

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

# 3. Sanity checks.
#
# GH_VM_MGR must be =m: it depends on GH_RM_DRV, which the device config
# sets to =m, so =y is not reachable without dropping the vendor
# gh_rm_drv.ko that DRM, qrtr and the virtio backends load.
if ! grep -q '^CONFIG_GH_VM_MGR=m$' "$KERNEL_SRC/.config"; then
    echo "错误：CONFIG_GH_VM_MGR 不是 m：$(grep '^CONFIG_GH_VM_MGR=' "$KERNEL_SRC/.config")" >&2
    exit 1
fi
if ! grep -q '^CONFIG_GH_RM_DRV=m$' "$KERNEL_SRC/.config"; then
    echo "错误：CONFIG_GH_RM_DRV 被改动，厂商模块会被替换" >&2
    exit 1
fi

# Build the kernel Image first. This is the step that has been proven to
# work; it does not compile any modules.
echo "=== $(date -u +%H:%M:%S) building Image ==="
make -C "$KERNEL_SRC" "${MAKE_ARGS[@]}" -j"$JOBS" Image
echo "=== $(date -u +%H:%M:%S) Image done ==="

# modules_prepare is cheap and guarantees the module build scaffolding is
# in place even if a future kernel version stops doing it as a side effect
# of building Image.
echo "=== $(date -u +%H:%M:%S) modules_prepare ==="
make -C "$KERNEL_SRC" "${MAKE_ARGS[@]}" modules_prepare
echo "=== $(date -u +%H:%M:%S) modules_prepare done ==="

# Use the DEVICE's Module.symvers as the symbol source for the module build,
# the same thing scripts/build_module.sh does.
#
# Why: gh_rm_drv.ko imports gh_get_irq/gh_put_irq/gh_get_virq/gh_put_virq
# from gh_arm_drv.ko, which lives in arch/arm64/gunyah and is not part of
# this M= build. Without a symvers that already knows those symbols,
# modpost would emit them with CRC 0 and every load would fail MODVERSIONS
# with "disagrees about version of symbol".
#
# config/Module.symvers was extracted from the running device and its CRCs
# were cross-checked against a kernel built from this exact source: 2738
# overlapping symbols, 0 mismatches. See CURRENT_RESULTS.md section 5.
install -m 0644 "$ROOT/config/Module.symvers" "$KERNEL_SRC/Module.symvers"

# Then build ONLY drivers/virt/gunyah, not the whole module tree.
#
# A full "make modules" fails in this vendor tree for reasons unrelated to
# Gunyah: drivers/clk/qcom/trace.h and drivers/soc/qcom/dcvs/trace-dcvs.h
# set TRACE_INCLUDE_PATH to ".", so include/trace/define_trace.h expands to
# #include "./trace.h", which resolves relative to include/trace/ and does
# not exist. Those modules are not ours and we do not ship them.
#
# Everything we need to build lives in drivers/virt/gunyah: gh_rm_drv (now
# also containing gh_rm_vmmgr.o), gh_msgq, gh_dbl and gh_vm_mgr itself.
# gh_arm_drv lives in arch/arm64/gunyah; it is not rebuilt and is resolved
# through the device Module.symvers installed above.
echo "=== $(date -u +%H:%M:%S) building gunyah modules ==="
make -C "$KERNEL_SRC" "${MAKE_ARGS[@]}" -j"$JOBS" \
    M=drivers/virt/gunyah modules
echo "=== $(date -u +%H:%M:%S) gunyah modules done ==="

# Hard gate: our rebuilt modules replace ones the device already ships.
# msm_drm, qrtr_gunyah and gh_virtio_backend import symbols from gh_rm_drv,
# so a changed CRC would make them refuse to load and the device would come
# up without a display. Compare before anything is packaged.
GV_SYMVERS="$KERNEL_SRC/drivers/virt/gunyah/Module.symvers"
if [[ -f "$GV_SYMVERS" ]]; then
    python3 "$ROOT/scripts/compare_module_crcs.py" \
        "$ROOT/config/Module.symvers" "$GV_SYMVERS"
else
    echo "错误：M= 构建没有生成 $GV_SYMVERS" >&2
    exit 1
fi

install -m 0644 "$KERNEL_SRC/arch/arm64/boot/Image" "$OUT_DIR/Image"
install -m 0644 "$KERNEL_SRC/System.map" "$OUT_DIR/System.map"
install -m 0644 "$KERNEL_SRC/.config" "$OUT_DIR/kernel.config"
for symvers in vmlinux.symvers Module.symvers; do
    if [[ -f "$KERNEL_SRC/$symvers" ]]; then
        install -m 0644 "$KERNEL_SRC/$symvers" "$OUT_DIR/$symvers"
    fi
done

# 4. Collect the two modules that matter: the VM manager, and gh_rm_drv
#    which now also carries gh_rm_vmmgr.o.
mkdir -p "$OUT_DIR/modules"
for ko in gh_vm_mgr.ko gh_rm_drv.ko; do
    src=$(find "$KERNEL_SRC/drivers/virt/gunyah" -maxdepth 1 -name "$ko" \
          -printf '%T@ %p\n' 2>/dev/null | sort -rn | head -1 | cut -d' ' -f2-)
    if [[ -z "${src:-}" ]]; then
        echo "错误：未找到 $ko" >&2
        exit 1
    fi
    install -m 0644 "$src" "$OUT_DIR/modules/$ko"
    echo "已生成 $OUT_DIR/modules/$ko ($(stat -c%s "$src") bytes)"
done

echo "已生成 $OUT_DIR/Image"
file "$OUT_DIR/Image"
sha256sum "$OUT_DIR/Image"

# 5. The VM manager module must carry the device vermagic or insmod refuses it.
VMC="$(modinfo -F vermagic "$OUT_DIR/modules/gh_vm_mgr.ko")"
if [[ "$VMC" != "5.10.252-gki-g19f54b03cc87 SMP preempt mod_unload modversions aarch64" ]]; then
    echo "错误：gh_vm_mgr.ko vermagic 不匹配：$VMC" >&2
    exit 1
fi
echo "OK: gh_vm_mgr.ko vermagic 正确"

# gh_rm_drv must now contain the RM extensions.
if ! strings "$OUT_DIR/modules/gh_rm_drv.ko" | grep -q 'gh_rm_vmmgr'; then
    echo "错误：gh_rm_drv.ko 中没有 gh_rm_vmmgr" >&2
    exit 1
fi
echo "OK: gh_rm_drv.ko 包含 gh_rm_vmmgr.o"
