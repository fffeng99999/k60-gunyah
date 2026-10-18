#!/usr/bin/env bash
# SPDX-License-Identifier: GPL-2.0-only
#
# Apply the K60 in-tree Gunyah VM manager into a kernel source tree.
#
# Usage:
#   KERNEL_SRC=/path/to/kernel ./scripts/apply_intree.sh
#   KERNEL_SRC=/path/to/kernel ./scripts/apply_intree.sh --revert
#
# The script is idempotent: running it twice leaves the tree in the same
# state, and --revert removes exactly what it added.
set -euo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
SRC="$ROOT/kernel-intree"
KERNEL_SRC="${KERNEL_SRC:-$ROOT/.work/kernel}"
REVERT=0

for arg in "$@"; do
    case "$arg" in
        --revert) REVERT=1 ;;
        *) echo "unknown argument: $arg" >&2; exit 2 ;;
    esac
done

if [[ ! -f "$KERNEL_SRC/Makefile" ]]; then
    echo "错误：KERNEL_SRC 不是内核源码树：$KERNEL_SRC" >&2
    exit 2
fi
if ! grep -q '^SUBLEVEL = 252$' "$KERNEL_SRC/Makefile"; then
    echo "错误：内核源码不是 Linux 5.10.252" >&2
    exit 1
fi

GH_DIR="$KERNEL_SRC/drivers/virt/gunyah"

install_files() {
    install -m 0644 "$SRC/vm_mgr.c"            "$GH_DIR/vm_mgr.c"
    install -m 0644 "$SRC/vm_mgr.h"            "$GH_DIR/vm_mgr.h"
    install -m 0644 "$SRC/gunyah_vcpu.c"       "$GH_DIR/gunyah_vcpu.c"
    install -m 0644 "$SRC/gunyah_irqfd.c"      "$GH_DIR/gunyah_irqfd.c"
    install -m 0644 "$SRC/gunyah_ioeventfd.c"  "$GH_DIR/gunyah_ioeventfd.c"
    install -m 0644 "$SRC/gh_rm_vmmgr.c"       "$GH_DIR/gh_rm_vmmgr.c"
    install -m 0644 "$SRC/gh_rm_vmmgr.h"       "$GH_DIR/gh_rm_vmmgr.h"
    install -m 0644 "$SRC/gh_rm_msgid_ext.h"   "$GH_DIR/gh_rm_msgid_ext.h"
    install -m 0644 "$SRC/gunyah_hvc_compat.h" "$GH_DIR/gunyah_hvc_compat.h"

    install -m 0644 "$SRC/uapi_linux_gunyah.h" \
        "$KERNEL_SRC/include/uapi/linux/gunyah.h"

    # Append the Kconfig block inside the "if GUNYAH_DRIVERS ... endif" body.
    if ! grep -q '^config GH_VM_MGR$' "$GH_DIR/Kconfig"; then
        awk '
            /^endif$/ && !done { print; print ""; while ((getline line < frag) > 0) print line; done=1; next }
            { print }
        ' frag="$SRC/Kconfig.fragment" "$GH_DIR/Kconfig" > "$GH_DIR/Kconfig.tmp"
        mv "$GH_DIR/Kconfig.tmp" "$GH_DIR/Kconfig"
    fi

    # Append the Makefile block.
    if ! grep -q '^obj-\$(CONFIG_GH_VM_MGR)' "$GH_DIR/Makefile"; then
        printf '\n' >> "$GH_DIR/Makefile"
        cat "$SRC/Makefile.fragment" >> "$GH_DIR/Makefile"
    fi

    echo "installed VM manager into $GH_DIR"
}

revert_files() {
    rm -f "$GH_DIR/vm_mgr.c" "$GH_DIR/vm_mgr.h" \
          "$GH_DIR/gunyah_vcpu.c" "$GH_DIR/gunyah_irqfd.c" \
          "$GH_DIR/gunyah_ioeventfd.c" "$GH_DIR/gh_rm_vmmgr.c" \
          "$GH_DIR/gh_rm_vmmgr.h" "$GH_DIR/gh_rm_msgid_ext.h" \
          "$GH_DIR/gunyah_hvc_compat.h" \
          "$KERNEL_SRC/include/uapi/linux/gunyah.h"

    # Strip the Kconfig block we appended.
    if grep -q '^config GH_VM_MGR$' "$GH_DIR/Kconfig"; then
        awk '
            /^config GH_VM_MGR$/ { skip=1 }
            skip && /^endif$/ { skip=0; next }
            !skip { print }
        ' "$GH_DIR/Kconfig" > "$GH_DIR/Kconfig.tmp"
        mv "$GH_DIR/Kconfig.tmp" "$GH_DIR/Kconfig"
    fi

    # Strip the Makefile block.
    if grep -q '^obj-\$(CONFIG_GH_VM_MGR)' "$GH_DIR/Makefile"; then
        awk '
            /^# K60 \(mondrian, SM8475\) in-tree Gunyah VM manager\.$/ { skip=1 }
            skip && /^gh_vm_mgr-y/ { skip=0; next }
            !skip { print }
        ' "$GH_DIR/Makefile" > "$GH_DIR/Makefile.tmp"
        mv "$GH_DIR/Makefile.tmp" "$GH_DIR/Makefile"
    fi

    echo "reverted VM manager from $GH_DIR"
}

if [[ $REVERT -eq 1 ]]; then
    revert_files
else
    install_files
fi
