#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-only
"""
Compare the CRCs of symbols our rebuilt modules export against the ones the
device already ships.

Why this exists: the VM manager ships a rebuilt gh_rm_drv.ko in place of the
vendor one, because gh_rm_vmmgr.o has to live in that module to reach
gh_rm_call(). msm_drm, qrtr_gunyah and gh_virtio_backend all import symbols
from gh_rm_drv, so if any of those CRCs moved, they would refuse to load and
the device would come up without a display.

MODVERSIONS CRCs are derived from the symbol's declared type in the headers,
not from the implementation, so identical headers give identical CRCs even
when the .c files differ. This script turns that expectation into a check.

Usage:
    compare_module_crcs.py <device-Module.symvers> <built-Module.symvers>

Exit 0 = every shared symbol agrees (or there is nothing to compare).
Exit 1 = at least one symbol has a different CRC.
"""

import sys


def load(path):
    """Return {symbol: crc} from a Module.symvers file."""
    out = {}
    with open(path, "r", encoding="utf-8", errors="replace") as fh:
        for line in fh:
            parts = line.split()
            if len(parts) < 2:
                continue
            crc, name = parts[0], parts[1]
            if crc.startswith("0x"):
                out[name] = crc.lower()
    return out


def main():
    if len(sys.argv) != 3:
        print(__doc__)
        return 2

    device = load(sys.argv[1])
    built = load(sys.argv[2])

    shared = sorted(set(device) & set(built))
    if not shared:
        print("compare_module_crcs: no shared exported symbols; "
              "nothing to compare")
        return 0

    mismatches = [(s, device[s], built[s]) for s in shared
                  if device[s] != built[s]]

    print("compare_module_crcs: device=%d built=%d shared=%d mismatches=%d"
          % (len(device), len(built), len(shared), len(mismatches)))

    if mismatches:
        print()
        print("CRC mismatch. These symbols would make the vendor modules "
              "that import them refuse to load:")
        for name, d, b in mismatches:
            print("  %-48s device=%s built=%s" % (name, d, b))
        print()
        print("Do NOT flash or install the rebuilt module until this is "
              "understood.")
        return 1

    return 0


if __name__ == "__main__":
    sys.exit(main())
