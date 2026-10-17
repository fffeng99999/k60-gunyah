#!/usr/bin/env bash
# SPDX-License-Identifier: GPL-2.0-only
set -euo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
KO="${1:-$ROOT/out/gunyah_probe.ko}"
KO2="${KO2:-$ROOT/out/gh_vmmgr.ko}"
OUT_ZIP="${2:-$ROOT/out/k60-gunyah-probe-magisk.zip}"
STAGE="$ROOT/out/magisk-stage"

if [[ ! -f "$KO" ]]; then
    echo "错误：找不到 $KO" >&2
    exit 2
fi

mkdir -p "$(dirname "$OUT_ZIP")"
OUT_ZIP="$(cd "$(dirname "$OUT_ZIP")" && pwd)/$(basename "$OUT_ZIP")"

rm -rf "$STAGE"
mkdir -p "$STAGE"
cp -a "$ROOT/magisk/." "$STAGE/"
install -m 0644 "$KO" "$STAGE/gunyah_probe.ko"
install -m 0644 "$KO2" "$STAGE/gh_vmmgr.ko"

(
    cd "$STAGE"
    rm -f "$OUT_ZIP"
    zip -9 -r "$OUT_ZIP" . -x '*.DS_Store'
)

echo "已生成 $OUT_ZIP"
