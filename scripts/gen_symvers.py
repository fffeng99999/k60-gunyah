#!/usr/bin/env python3
"""从设备预编译模块重建 Module.symvers。

只读取 ELF 元数据（符号表、__versions、__ksymtab 和 .modinfo），
不读取任何用户数据。用于在没有 vmlinux 的情况下收集 MODVERSIONS CRC。
"""

from __future__ import annotations

import argparse
import re
import sys
from pathlib import Path

from elftools.elf.elffile import ELFFile

VERSION_ENTRY_SIZE = 64  # 64 位内核 MODULE_NAME_LEN=56，另有 8 字节 CRC
EXPORT_SECTIONS = {
    "__ksymtab": "EXPORT_SYMBOL",
    "__ksymtab_gpl": "EXPORT_SYMBOL_GPL",
    "__ksymtab_unused": "EXPORT_SYMBOL",
    "__ksymtab_unused_gpl": "EXPORT_SYMBOL_GPL",
}


def modinfo(elf: ELFFile) -> dict[str, str]:
    section = elf.get_section_by_name(".modinfo")
    if section is None:
        return {}
    result: dict[str, str] = {}
    for item in section.data().split(b"\0"):
        if b"=" not in item:
            continue
        key, value = item.split(b"=", 1)
        result[key.decode("ascii", "replace")] = value.decode("utf-8", "replace")
    return result


def read_versions(elf: ELFFile, path: Path) -> dict[str, int]:
    section = elf.get_section_by_name("__versions")
    if section is None:
        return {}
    data = section.data()
    if len(data) % VERSION_ENTRY_SIZE:
        raise ValueError(
            f"{path.name}: __versions 长度 {len(data)} 不是 {VERSION_ENTRY_SIZE} 的倍数"
        )

    versions: dict[str, int] = {}
    for offset in range(0, len(data), VERSION_ENTRY_SIZE):
        entry = data[offset : offset + VERSION_ENTRY_SIZE]
        crc = int.from_bytes(entry[:8], "little")
        name = entry[8:].split(b"\0", 1)[0].decode("ascii", "replace")
        if not name:
            continue
        previous = versions.setdefault(name, crc)
        if previous != crc:
            raise ValueError(f"{path.name}: 符号 {name} 存在多个 CRC")
    return versions


def read_exports(elf: ELFFile, path: Path) -> list[tuple[str, int, str]]:
    symtab = elf.get_section_by_name(".symtab")
    if symtab is None:
        return []

    crc_by_name: dict[str, int] = {}
    for symbol in symtab.iter_symbols():
        if symbol.name.startswith("__crc_") and symbol["st_value"]:
            crc_by_name[symbol.name[6:]] = int(symbol["st_value"])

    section_indexes = {section.name: index for index, section in enumerate(elf.iter_sections())}
    exports: list[tuple[str, int, str]] = []
    seen: set[str] = set()
    for section_name, export_type in EXPORT_SECTIONS.items():
        section_index = section_indexes.get(section_name)
        if section_index is None:
            continue
        for symbol in symtab.iter_symbols():
            if symbol["st_shndx"] != section_index:
                continue
            if not symbol.name.startswith("__ksymtab_"):
                continue
            name = symbol.name[len("__ksymtab_") :]
            if not name or name in seen:
                continue
            crc = crc_by_name.get(name)
            if crc is None:
                raise ValueError(f"{path.name}: 导出符号 {name} 缺少 __crc_")
            seen.add(name)
            exports.append((name, crc, export_type))
    return exports


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--mods", required=True, type=Path, help="设备 .ko 目录")
    parser.add_argument("--output", required=True, type=Path, help="输出 Module.symvers")
    parser.add_argument("--vermagic", help="要求匹配的 vermagic")
    args = parser.parse_args()

    modules = sorted(args.mods.glob("*.ko"))
    if not modules:
        print(f"错误：{args.mods} 中没有 .ko", file=sys.stderr)
        return 2

    versions: dict[str, int] = {}
    version_source: dict[str, str] = {}
    exports: dict[str, tuple[int, str, str]] = {}
    module_name_re = re.compile(r"^[A-Za-z0-9_]+$")
    checked = 0

    for path in modules:
        try:
            with path.open("rb") as handle:
                elf = ELFFile(handle)
                info = modinfo(elf)
                vermagic = info.get("vermagic", "")
                if args.vermagic and vermagic != args.vermagic:
                    raise ValueError(f"{path.name}: vermagic 不匹配: {vermagic!r}")

                module_name = info.get("name", path.stem)
                if not module_name_re.match(module_name):
                    raise ValueError(f"{path.name}: 非法模块名 {module_name!r}")

                for name, crc in read_versions(elf, path).items():
                    old = versions.setdefault(name, crc)
                    if old != crc:
                        raise ValueError(
                            f"符号 {name} CRC 冲突: {version_source[name]}={old:#x}, "
                            f"{path.name}={crc:#x}"
                        )
                    version_source.setdefault(name, path.name)

                for name, crc, export_type in read_exports(elf, path):
                    old = exports.get(name)
                    if old and old[0] != crc:
                        raise ValueError(
                            f"导出符号 {name} CRC 冲突: {old[1]}={old[0]:#x}, "
                            f"{path.name}={crc:#x}"
                        )
                    exports[name] = (crc, module_name, export_type)
                checked += 1
        except Exception as exc:  # noqa: BLE001 - 命令行工具需要报告文件级错误
            print(f"错误：{exc}", file=sys.stderr)
            return 1

    lines: list[str] = []
    for name in sorted(set(versions) | set(exports)):
        if name in exports:
            crc, module_name, export_type = exports[name]
        else:
            crc = versions[name]
            module_name = "vmlinux"
            export_type = "EXPORT_SYMBOL"
        lines.append(f"0x{crc:08x}\t{name}\t{module_name}\t{export_type}")

    args.output.parent.mkdir(parents=True, exist_ok=True)
    args.output.write_bytes(("\n".join(lines) + "\n").encode("ascii"))
    print(f"已处理 {checked} 个模块，{len(versions)} 个导入符号，{len(exports)} 个导出符号")
    print(f"已写入 {args.output}，共 {len(lines)} 条记录")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
