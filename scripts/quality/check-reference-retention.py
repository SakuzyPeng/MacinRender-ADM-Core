#!/usr/bin/env python3
"""Validate tests/reference/retention.json against the frozen migration reference code.

Every file under tests/reference/ must belong to exactly one registered unit and still match its
recorded SHA-256, so frozen implementations cannot drift silently. Retired units keep their entry
(with the removal commit) but no files. See docs/architecture/RUST_REFERENCE_RETENTION.md.
"""

from __future__ import annotations

import hashlib
import json
from pathlib import Path
import re
import sys


REPO_ROOT = Path(__file__).resolve().parents[2]
REFERENCE_ROOT = REPO_ROOT / "tests" / "reference"
REGISTRY = REFERENCE_ROOT / "retention.json"
SCHEMA = "mradm.reference-retention.v1"
KINDS = {"frozen", "third-party"}
STATUSES = {"retained", "retired"}


def sha256(path: Path) -> str:
    return hashlib.sha256(path.read_bytes()).hexdigest()


def main() -> int:
    errors: list[str] = []
    registry = json.loads(REGISTRY.read_text(encoding="utf-8"))
    if registry.get("schema") != SCHEMA:
        errors.append(f"schema 必须为 {SCHEMA}")
    conditions = set(registry.get("conditions", {}))
    if not (REPO_ROOT / registry.get("policy", "")).is_file():
        errors.append("policy 指向的规则文档不存在")
    cmake = (REPO_ROOT / "CMakeLists.txt").read_text(encoding="utf-8")

    units = {}
    owner: dict[str, str] = {}
    for unit in registry.get("units", []):
        uid = unit.get("id", "?")
        where = f"retention.json: {uid}"
        if uid in units:
            errors.append(f"{where}: id 重复")
        units[uid] = unit
        if unit.get("kind") not in KINDS:
            errors.append(f"{where}: kind 必须是 {sorted(KINDS)}")
        if unit.get("status") not in STATUSES:
            errors.append(f"{where}: status 必须是 {sorted(STATUSES)}")
        if not re.fullmatch(r"\d{4}-\d{2}-\d{2}", unit.get("accepted") or ""):
            errors.append(f"{where}: accepted 必须是 YYYY-MM-DD")
        # Retired units keep provenance as a historical path in the removal commit's parent;
        # their registered reference files have been deleted, while migration evidence remains.
        required_paths = ("migration_doc", "provenance") if unit.get("status") == "retained" else ("migration_doc",)
        for key in required_paths:
            if unit.get(key) and not (REPO_ROOT / unit[key]).is_file():
                errors.append(f"{where}: {key} {unit[key]} 不存在")
        if not unit.get("migration_doc"):
            errors.append(f"{where}: 缺少 migration_doc")
        option = unit.get("cmake_option")
        if (unit.get("kind") == "third-party") != bool(option):
            errors.append(f"{where}: third-party 单元必须且只有它们登记 cmake_option")
        if unit.get("status") == "retained":
            if option and f"option({option}" not in cmake:
                errors.append(f"{where}: CMakeLists.txt 中没有选项 {option}")
            for test in unit.get("tests", []):
                if not re.search(rf"\bNAME {re.escape(test)}\b", cmake):
                    errors.append(f"{where}: CMakeLists.txt 中没有测试 {test}")
            unknown = set(unit.get("open_conditions", [])) - conditions
            if unknown:
                errors.append(f"{where}: 未定义的条件 {sorted(unknown)}")
            if not unit.get("files"):
                errors.append(f"{where}: 保留中的单元必须登记参考文件")
        else:
            if unit.get("files"):
                errors.append(f"{where}: 已退役单元不应再登记文件")
            if not re.fullmatch(r"[0-9a-f]{7,40}", unit.get("retired_in") or ""):
                errors.append(f"{where}: 已退役单元需要 retired_in 提交")
            if unit.get("open_conditions"):
                errors.append(f"{where}: 已退役单元不应有未满足条件")
        for relative, expected in unit.get("files", {}).items():
            if relative in owner:
                errors.append(f"{where}: {relative} 已登记在 {owner[relative]}")
            owner[relative] = uid
            path = REPO_ROOT / relative
            if not path.is_file():
                errors.append(f"{where}: 登记的 {relative} 不存在")
            elif sha256(path) != expected:
                errors.append(f"{where}: {relative} 内容与登记的 SHA-256 不一致（冻结参考被改动；确属必要请同步更新登记并在迁移文档说明）")

    for uid, unit in units.items():
        for dependency in unit.get("depends_on", []):
            if dependency not in units:
                errors.append(f"retention.json: {uid}: depends_on 未知单元 {dependency}")
            elif unit.get("status") == "retained" and units[dependency].get("status") == "retired":
                errors.append(f"retention.json: {uid} 仍保留，但依赖的 {dependency} 已退役")

    for path in sorted(REFERENCE_ROOT.rglob("*")):
        relative = path.relative_to(REPO_ROOT).as_posix()
        if path.is_file() and path != REGISTRY and relative not in owner:
            errors.append(f"{relative}: 未登记到 tests/reference/retention.json")

    if errors:
        for message in errors:
            print(f"error: {message}", file=sys.stderr)
        print(f"参考代码登记校验失败：{len(errors)} 处问题", file=sys.stderr)
        return 1
    retained = sum(1 for unit in units.values() if unit.get("status") == "retained")
    print(f"参考代码登记校验通过：{len(units)} 个单元（保留 {retained}），{len(owner)} 个文件哈希一致")
    return 0


if __name__ == "__main__":
    # Windows consoles default to a legacy code page; the messages are Chinese.
    for stream in (sys.stdout, sys.stderr):
        stream.reconfigure(encoding="utf-8")
    sys.exit(main())
