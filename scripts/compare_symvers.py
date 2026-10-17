#!/usr/bin/env python3
"""比较设备模块 CRC 与新内核 Module.symvers 的重叠符号。"""

from __future__ import annotations

import argparse
from pathlib import Path


def load_symvers(path: Path) -> dict[str, int]:
    result: dict[str, int] = {}
    for line_no, line in enumerate(path.read_text(encoding="ascii").splitlines(), 1):
        if not line:
            continue
        fields = line.split("\t")
        if len(fields) < 4:
            raise ValueError(f"{path}:{line_no}: 字段不足")
        result[fields[1]] = int(fields[0], 16)
    return result


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--device", required=True, type=Path)
    parser.add_argument("--built", required=True, type=Path)
    parser.add_argument("--show", type=int, default=80)
    parser.add_argument("--require-match", action="store_true")
    args = parser.parse_args()

    device = load_symvers(args.device)
    built = load_symvers(args.built)
    common = sorted(set(device) & set(built))
    mismatches = [(name, device[name], built[name]) for name in common if device[name] != built[name]]

    print(f"device symbols: {len(device)}")
    print(f"built symbols:  {len(built)}")
    print(f"overlap:        {len(common)}")
    print(f"mismatches:     {len(mismatches)}")
    for name, dev_crc, built_crc in mismatches[: args.show]:
        print(f"{name}: device={dev_crc:#010x} built={built_crc:#010x}")

    if args.require_match and mismatches:
        return 1
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
