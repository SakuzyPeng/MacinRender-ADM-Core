#!/usr/bin/env python3
"""维护 checked-in 的 license 原文 bundle：third_party/licenses/<name>/。

bundle 是发行包的权威来源（打包时纯 cp 即可，与构建解耦），由本脚本从构建目录的
FetchContent _deps 树同步而来。vendored/system 组件的 NOTICE.txt 为手写，本脚本不覆盖。

模式：
  --refresh --build-dir DIR   从 <DIR>/_deps 把各依赖 license 原文同步进 bundle，并重写 INDEX.md
  --check                     （无需构建）校验 bundle 中每个组件声明的文件都存在
  --check --build-dir DIR     额外：对 _deps 中存在的依赖，校验 bundle 字节与新鲜 _deps 一致（防漂移）

退出码：0 通过；1 校验失败；2 用法错误。
"""

from __future__ import annotations

import argparse
import shutil
import sys
from pathlib import Path

import _common

# 这些 source.type 的 license 原文从 _deps 同步；其余（vendored/system）为手写 NOTICE。
HARVESTABLE = {"fetchcontent", "nested"}


def find_deps_root(build_dir: Path, deps_dir: str) -> Path | None:
    """定位某依赖的源码树。标准布局 <build>/_deps/<deps_dir>，兼容扁平 <build>/<deps_dir>。"""
    for cand in (build_dir / "_deps" / deps_dir, build_dir / deps_dir):
        if cand.is_dir():
            return cand
    return None


def bundle_files(component: dict) -> list[Path]:
    """该组件在 bundle 中应存在的目标文件路径列表。"""
    name = component["name"]
    dest_dir = _common.licenses_dir() / name
    return [dest_dir / Path(lf).name for lf in component["source"]["license_files"]]


def refresh(components: list[dict], build_dir: Path) -> int:
    missing_sources: list[str] = []
    for c in components:
        src = c["source"]
        if src["type"] not in HARVESTABLE:
            # vendored/system：仅确认手写 NOTICE 存在，不覆盖
            for dest in bundle_files(c):
                if not dest.exists():
                    missing_sources.append(f"{c['name']}: 缺手写 {dest.relative_to(_common.repo_root())}")
            continue
        deps_root = find_deps_root(build_dir, src["deps_dir"])
        if deps_root is None:
            missing_sources.append(f"{c['name']}: 构建目录无 _deps/{src['deps_dir']}（请用含全部依赖的构建）")
            continue
        dest_dir = _common.licenses_dir() / c["name"]
        dest_dir.mkdir(parents=True, exist_ok=True)
        for lf in src["license_files"]:
            src_file = deps_root / lf
            if not src_file.is_file():
                missing_sources.append(f"{c['name']}: _deps 中缺 {lf}")
                continue
            shutil.copyfile(src_file, dest_dir / Path(lf).name)

    if missing_sources:
        print("[ERROR] refresh 未完整：", file=sys.stderr)
        for m in missing_sources:
            print(f"  - {m}", file=sys.stderr)
        return 1

    write_index(components)
    print(f"[INFO] 已刷新 license bundle 至 {_common.licenses_dir().relative_to(_common.repo_root())}")
    return 0


def write_index(components: list[dict]) -> None:
    lines = [
        "# 第三方 License 原文 bundle",
        "",
        "> 本目录由 scripts/licenses/sync_license_texts.py 从构建 _deps 同步生成；",
        "> vendored/system 组件的 NOTICE.txt 为手写。发行包直接复制本目录。",
        "",
        "| 依赖 | 版本 | 许可证 | bundle 文件 |",
        "|---|---|---|---|",
    ]
    for c in sorted(components, key=lambda x: x["name"].lower()):
        files = ", ".join(f"`{Path(lf).name}`" for lf in c["source"]["license_files"])
        lines.append(f"| {c['name']} | {c['version']} | {c['license']} | {files} |")
    lines.append("")
    (_common.licenses_dir() / "INDEX.md").write_text("\n".join(lines), encoding="utf-8")


def check(components: list[dict], build_dir: Path | None, require_full: bool) -> int:
    errors: list[str] = []

    # 1) bundle 齐全性（无需构建）
    for c in components:
        for dest in bundle_files(c):
            if not dest.is_file():
                errors.append(f"{c['name']}: bundle 缺 {dest.relative_to(_common.repo_root())}")

    index_p = _common.licenses_dir() / "INDEX.md"
    if not index_p.is_file():
        errors.append("缺 third_party/licenses/INDEX.md（运行 --refresh 生成）")

    # 2) 漂移校验（需要构建目录）。默认对 _deps 不在该构建中的依赖优雅跳过；
    #    --require-full 时改为硬失败，用于发布前强制全量覆盖。
    if build_dir is not None:
        skipped = 0
        for c in components:
            src = c["source"]
            if src["type"] not in HARVESTABLE:
                continue
            deps_root = find_deps_root(build_dir, src["deps_dir"])
            if deps_root is None:
                if require_full:
                    errors.append(
                        f"{c['name']}: 构建目录无 _deps/{src['deps_dir']}"
                        f"（--require-full 要求全量覆盖；请用全 FetchContent 构建）"
                    )
                else:
                    skipped += 1
                continue
            dest_dir = _common.licenses_dir() / c["name"]
            for lf in src["license_files"]:
                src_file = deps_root / lf
                dest_file = dest_dir / Path(lf).name
                if not src_file.is_file():
                    errors.append(f"{c['name']}: _deps 声明文件缺失 {lf}（manifest 路径过期？）")
                    continue
                if not dest_file.is_file():
                    continue  # 已在齐全性检查里报过
                if src_file.read_bytes() != dest_file.read_bytes():
                    errors.append(
                        f"{c['name']}: bundle 与 _deps 漂移 {Path(lf).name}"
                        f"（依赖升级后请运行 --refresh）"
                    )
        if skipped:
            print(f"[INFO] 漂移校验跳过 {skipped} 个 _deps 不在此构建中的依赖（如 debug 不含 FLAC/Opus）")

    if errors:
        print("[ERROR] license bundle 校验失败：", file=sys.stderr)
        for e in errors:
            print(f"  - {e}", file=sys.stderr)
        return 1
    print("[INFO] license bundle 校验通过")
    return 0


def main() -> int:
    ap = argparse.ArgumentParser(description="同步/校验 license 原文 bundle")
    mode = ap.add_mutually_exclusive_group(required=True)
    mode.add_argument("--refresh", action="store_true", help="从 _deps 刷新 bundle（需 --build-dir）")
    mode.add_argument("--check", action="store_true", help="校验 bundle（可选 --build-dir 做漂移校验）")
    ap.add_argument("--build-dir", type=Path, help="含 _deps 的 CMake 构建目录")
    ap.add_argument(
        "--require-full",
        action="store_true",
        help="校验时要求所有依赖的 _deps 都在该构建中（缺即失败，用于发布前强制全量）",
    )
    args = ap.parse_args()

    components = _common.load_components()

    if args.refresh:
        if args.build_dir is None:
            print("[ERROR] --refresh 需要 --build-dir", file=sys.stderr)
            return 2
        return refresh(components, args.build_dir.resolve())

    if args.require_full and args.build_dir is None:
        print("[ERROR] --require-full 需要 --build-dir", file=sys.stderr)
        return 2
    return check(components, args.build_dir.resolve() if args.build_dir else None, args.require_full)


if __name__ == "__main__":
    raise SystemExit(main())
