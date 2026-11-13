#!/usr/bin/env python3
"""Check that the self-contained C ABI bundle exports exactly the functions declared in c_api.h.

Private Rust (`mradm_*`), allocator, panic and C++ symbols must stay inside the shared library.
ELF/Mach-O exports come from `nm`; PE exports are read directly from the DLL export table so the
check does not need a Visual Studio environment. `--write-def` emits the Windows module-definition
file that the bundle links with; `--write-references` emits the C table that names every declared
function, so the linker takes the exports implemented in Rust from the Rust staticlib.
"""

from __future__ import annotations

import argparse
from pathlib import Path
import re
import struct
import subprocess
import sys


REPO_ROOT = Path(__file__).resolve().parents[2]
C_API_HEADER = REPO_ROOT / "include" / "adm" / "c_api.h"


def declared_functions(header: Path = C_API_HEADER) -> list[str]:
    text = re.sub(r"/\*.*?\*/|//[^\n]*", " ", header.read_text(encoding="utf-8"), flags=re.S)
    names = re.findall(r"^[A-Za-z_][\w\s\*]*?\b(adm_\w+)\s*\(", text, re.M)
    if len(names) != len(set(names)):
        sys.exit(f"{header}: 函数声明重复")
    return names


def write_if_changed(path: Path, content: str) -> None:
    path.parent.mkdir(parents=True, exist_ok=True)
    if not path.exists() or path.read_text(encoding="ascii") != content:
        path.write_text(content, encoding="ascii")


def references(declared: list[str]) -> str:
    entries = "".join(f"    (mradm_capi_entry){name},\n" for name in declared)
    return (
        "/* Generated from include/adm/c_api.h by scripts/quality/check-capi-exports.py. */\n"
        "/* Every declared export, whichever side defines it: a Rust export is otherwise not taken from\n"
        "   the Rust staticlib, since nothing inside the shared library calls it. */\n"
        "#if defined(__GNUC__) || defined(__clang__)\n"
        "#pragma GCC diagnostic ignored \"-Wdeprecated-declarations\"\n"
        "#elif defined(_MSC_VER)\n"
        "#pragma warning(disable : 4996)\n"
        "#endif\n"
        "#include \"adm/c_api.h\"\n\n"
        "typedef void (*mradm_capi_entry)(void);\n"
        "extern const mradm_capi_entry mradm_capi_entries[];\n"
        f"const mradm_capi_entry mradm_capi_entries[{len(declared)}] = {{\n{entries}}};\n"
    )


def pe_exports(path: Path) -> list[str]:
    data = path.read_bytes()
    pe = struct.unpack_from("<I", data, 0x3C)[0]
    if data[pe : pe + 4] != b"PE\0\0":
        sys.exit(f"{path}: 不是 PE 文件")
    sections = struct.unpack_from("<H", data, pe + 6)[0]
    optional_size = struct.unpack_from("<H", data, pe + 20)[0]
    optional = pe + 24
    magic = struct.unpack_from("<H", data, optional)[0]
    directories = optional + (96 if magic == 0x10B else 112)
    export_rva = struct.unpack_from("<I", data, directories)[0]
    if export_rva == 0:
        return []
    table = optional + optional_size

    def offset(rva: int) -> int:
        for index in range(sections):
            base = table + index * 40
            size, address, _, raw = struct.unpack_from("<IIII", data, base + 8)
            if address <= rva < address + max(size, 1):
                return rva - address + raw
        sys.exit(f"{path}: 无法定位 RVA {rva:#x}")

    export = offset(export_rva)
    count, names_rva = struct.unpack_from("<I", data, export + 24)[0], struct.unpack_from("<I", data, export + 32)[0]
    names = []
    for index in range(count):
        start = offset(struct.unpack_from("<I", data, offset(names_rva) + index * 4)[0])
        names.append(data[start : data.index(b"\0", start)].decode("ascii"))
    return names


def nm_exports(path: Path) -> list[str]:
    if path.suffix == ".dylib":
        command = ["nm", "-gU", "-j", str(path)]
    else:
        command = ["nm", "-D", "--defined-only", "--format=just-symbols", str(path)]
    output = subprocess.run(command, capture_output=True, text=True, check=True).stdout.split()
    if path.suffix == ".dylib":
        output = [name[1:] if name.startswith("_") else name for name in output]
    return output


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    group = parser.add_mutually_exclusive_group(required=True)
    group.add_argument("library", nargs="?", type=Path, help="mradm_capi 共享库（.so/.dylib/.dll）")
    group.add_argument("--write-def", type=Path, help="写出 Windows .def 导出文件")
    group.add_argument("--write-references", type=Path, help="写出引用全部导出函数的 C 表")
    args = parser.parse_args()

    declared = declared_functions()
    if args.write_def:
        write_if_changed(args.write_def, "EXPORTS\n" + "".join(f"    {name}\n" for name in declared))
        return 0
    if args.write_references:
        write_if_changed(args.write_references, references(declared))
        return 0

    library = args.library.resolve()
    exports = pe_exports(library) if library.suffix.lower() == ".dll" else nm_exports(library)
    # Linkers may add these marker symbols to ELF dynamic tables; they are not callable entry points.
    exports = [name for name in exports if name not in ("_init", "_fini")]
    missing = sorted(set(declared) - set(exports))
    extra = sorted(set(exports) - set(declared))
    for name in missing:
        print(f"error: {name} 在 c_api.h 中声明但未导出", file=sys.stderr)
    for name in extra[:50]:
        print(f"error: 导出了 c_api.h 之外的符号 {name}", file=sys.stderr)
    if len(extra) > 50:
        print(f"error: …以及另外 {len(extra) - 50} 个多余导出", file=sys.stderr)
    if missing or extra:
        print(f"C ABI 导出面校验失败：缺 {len(missing)} 个，多 {len(extra)} 个", file=sys.stderr)
        return 1
    print(f"C ABI 导出面校验通过：{library.name} 恰好导出 {len(declared)} 个 adm_* 函数")
    return 0


if __name__ == "__main__":
    # Windows consoles default to a legacy code page; the messages are Chinese.
    for stream in (sys.stdout, sys.stderr):
        stream.reconfigure(encoding="utf-8")
    sys.exit(main())
