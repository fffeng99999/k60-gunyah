#!/usr/bin/env python3
"""用新内核重建 K60 boot v4 镜像，保留原 ramdisk 与 header 参数。"""

from __future__ import annotations

import argparse
import hashlib
import struct
from pathlib import Path

PAGE_SIZE = 4096
BOOT_MAGIC = b"ANDROID!"
HEADER_SIZE_OFFSET = 20
HEADER_VERSION_OFFSET = 40
KERNEL_SIZE_OFFSET = 8
RAMDISK_SIZE_OFFSET = 12


def sha256(data: bytes) -> str:
    return hashlib.sha256(data).hexdigest()


def parse_stock(data: bytes) -> tuple[bytes, bytes, bytes, int, int, int]:
    if data[:8] != BOOT_MAGIC:
        raise ValueError("不是 Android boot image")
    kernel_size, ramdisk_size = struct.unpack_from("<II", data, KERNEL_SIZE_OFFSET)
    header_size = struct.unpack_from("<I", data, HEADER_SIZE_OFFSET)[0]
    header_version = struct.unpack_from("<I", data, HEADER_VERSION_OFFSET)[0]
    if header_version != 4:
        raise ValueError(f"仅支持 boot v4，当前为 v{header_version}")
    if header_size > PAGE_SIZE:
        raise ValueError(f"异常 header_size: {header_size}")
    signature_size = struct.unpack_from("<I", data, PAGE_SIZE - 4)[0]
    if signature_size:
        raise ValueError("当前设备 boot v4 带有 boot signature，本脚本拒绝破坏签名")

    kernel_offset = PAGE_SIZE
    ramdisk_offset = kernel_offset + ((kernel_size + PAGE_SIZE - 1) // PAGE_SIZE) * PAGE_SIZE
    kernel = data[kernel_offset : kernel_offset + kernel_size]
    ramdisk = data[ramdisk_offset : ramdisk_offset + ramdisk_size]
    if len(kernel) != kernel_size or len(ramdisk) != ramdisk_size:
        raise ValueError("原始 boot 数据截断")
    return data[:PAGE_SIZE], kernel, ramdisk, kernel_size, ramdisk_size, signature_size


def align(data: bytes) -> bytes:
    remainder = len(data) % PAGE_SIZE
    return data if remainder == 0 else data + b"\0" * (PAGE_SIZE - remainder)


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--stock-boot", required=True, type=Path)
    parser.add_argument("--kernel", required=True, type=Path)
    parser.add_argument("--output", required=True, type=Path)
    args = parser.parse_args()

    stock = args.stock_boot.read_bytes()
    header, stock_kernel, ramdisk, stock_kernel_size, ramdisk_size, _ = parse_stock(stock)
    kernel = args.kernel.read_bytes()
    if not kernel:
        raise ValueError("新内核为空")

    new_header = bytearray(header)
    struct.pack_into("<I", new_header, KERNEL_SIZE_OFFSET, len(kernel))
    struct.pack_into("<I", new_header, RAMDISK_SIZE_OFFSET, len(ramdisk))

    image = bytes(new_header) + align(kernel) + align(ramdisk)
    args.output.parent.mkdir(parents=True, exist_ok=True)
    args.output.write_bytes(image)

    print(f"stock kernel: {stock_kernel_size} bytes sha256={sha256(stock_kernel)}")
    print(f"new kernel:   {len(kernel)} bytes sha256={sha256(kernel)}")
    print(f"ramdisk:      {ramdisk_size} bytes sha256={sha256(ramdisk)}")
    print(f"boot image:   {len(image)} bytes sha256={sha256(image)}")
    print(f"output:       {args.output}")
    print("注意：输出是未签名 boot payload；解锁状态优先用 fastboot boot 测试，不要直接刷写。")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
