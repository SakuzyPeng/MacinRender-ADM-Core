#!/usr/bin/env python3
"""Measure the C/C++ surface and ratchet it down (ADR 0021).

Lines are newline counts (as `wc -l`). Production C/C++ is `src/` plus `include/`, excluding the
platform islands; islands are ratcheted separately. Tests, frozen references and Rust are reported
only. `--check` fails when a ratcheted area grows past the committed baseline; after deleting C++,
run `--update` in the same commit to tighten it. See docs/architecture/RUST_INVERSION_ROADMAP.md.
"""

from __future__ import annotations

import argparse
import json
from pathlib import Path
import sys


REPO_ROOT = Path(__file__).resolve().parents[2]
BASELINE = Path(__file__).with_name("language-surface-baseline.json")
SCHEMA = "mradm.language-surface.v1"
CXX_SUFFIXES = {".c", ".h", ".cc", ".cpp", ".hpp", ".inl", ".m", ".mm"}
ISLANDS = ("src/adm_apple", "src/adm_windows")
RATCHETED = ("production", "islands")


def files(root: Path, relative: str, suffixes: set[str]):
    base = root / relative
    if not base.is_dir():
        return
    for path in sorted(base.rglob("*")):
        if path.is_file() and path.suffix in suffixes and "__pycache__" not in path.parts:
            yield path


def lines(path: Path) -> int:
    return path.read_bytes().count(b"\n")


def area_of(relative: str) -> str:
    if relative.startswith(tuple(island + "/" for island in ISLANDS)):
        return "islands"
    if relative.startswith(("src/", "include/")):
        return "production"
    if relative.startswith("tests/reference/"):
        return "reference"
    return "tests"


def measure(root: Path = REPO_ROOT) -> dict:
    areas = {name: {"files": 0, "lines": 0} for name in ("production", "islands", "tests", "reference", "rust", "rust-vendor")}
    modules: dict[str, int] = {}

    def add(area: str, module: str, path: Path) -> None:
        count = lines(path)
        areas[area]["files"] += 1
        areas[area]["lines"] += count
        modules[module] = modules.get(module, 0) + count

    for top in ("src", "include", "tests"):
        for path in files(root, top, CXX_SUFFIXES):
            relative = path.relative_to(root).as_posix()
            parts = relative.split("/")
            module = "/".join(parts[:2]) if top != "tests" or parts[1] != "reference" else "/".join(parts[:3])
            add(area_of(relative), module, path)
    for top, area in (("rust/crates", "rust"), ("rust/vendor", "rust-vendor")):
        for path in files(root, top, {".rs"}):
            add(area, "/".join(path.relative_to(root).as_posix().split("/")[:3]), path)
    return {"schema": SCHEMA, "areas": areas, "modules": dict(sorted(modules.items()))}


def check(current: dict, baseline: dict) -> list[str]:
    errors = []
    if baseline.get("schema") != SCHEMA:
        errors.append(f"基线 schema 必须为 {SCHEMA}")
        return errors
    for area in RATCHETED:
        now, before = current["areas"][area]["lines"], baseline["areas"][area]["lines"]
        if now > before:
            grown = sorted(((m, n - baseline["modules"].get(m, 0)) for m, n in current["modules"].items()
                            if area_of(m + "/") == area and n > baseline["modules"].get(m, 0)),
                           key=lambda item: -item[1])
            detail = "，".join(f"{m} +{d}" for m, d in grown[:5])
            errors.append(f"{area} C/C++ 由 {before} 行增至 {now} 行（{detail}）；ADR 0021 要求只减不增")
    return errors


def main(argv: list[str] | None = None) -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--root", type=Path, default=REPO_ROOT)
    parser.add_argument("--baseline", type=Path, default=BASELINE)
    mode = parser.add_mutually_exclusive_group()
    mode.add_argument("--check", action="store_true", help="fail when a ratcheted area grows past the baseline")
    mode.add_argument("--update", action="store_true", help="write the current measurement as the baseline")
    args = parser.parse_args(argv)
    current = measure(args.root)
    if args.update:
        args.baseline.write_text(json.dumps(current, ensure_ascii=False, indent=2) + "\n", encoding="utf-8")
        print(f"已写入 {args.baseline}")
        return 0
    if not args.check:
        print(json.dumps(current, ensure_ascii=False, indent=2))
        return 0
    baseline = json.loads(args.baseline.read_text(encoding="utf-8"))
    errors = check(current, baseline)
    for error in errors:
        print(f"错误：{error}", file=sys.stderr)
    if errors:
        return 1
    for area in RATCHETED:
        now, before = current["areas"][area]["lines"], baseline["areas"][area]["lines"]
        if now < before:
            print(f"{area} C/C++ 由 {before} 行减至 {now} 行；请在同一提交运行 --update 收紧基线")
    print("C/C++ 规模棘轮通过")
    return 0


if __name__ == "__main__":
    sys.exit(main())
