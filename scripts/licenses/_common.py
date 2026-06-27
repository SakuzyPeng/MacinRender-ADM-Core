"""第三方许可清单的共享加载与小工具。纯标准库，零 pip 依赖。"""

from __future__ import annotations

import json
from pathlib import Path

SCHEMA = "mradm.third-party-manifest.v1"


def repo_root() -> Path:
    return Path(__file__).resolve().parents[2]


def manifest_path() -> Path:
    return repo_root() / "third_party" / "manifest.json"


def sbom_path() -> Path:
    return repo_root() / "third_party" / "sbom.cyclonedx.json"


def licenses_dir() -> Path:
    return repo_root() / "third_party" / "licenses"


def notices_doc_path() -> Path:
    return repo_root() / "docs" / "THIRD_PARTY_LICENSES.md"


def load_components() -> list[dict]:
    data = json.loads(manifest_path().read_text(encoding="utf-8"))
    if data.get("schema") != SCHEMA:
        raise SystemExit(f"[ERROR] manifest schema 不符，期望 {SCHEMA}，实际 {data.get('schema')!r}")
    components = data.get("components")
    if not isinstance(components, list) or not components:
        raise SystemExit("[ERROR] manifest.components 为空或非数组")
    return components


def is_spdx_expression(license_str: str) -> bool:
    return any(tok in license_str for tok in (" OR ", " AND ", "(", ")"))


def cyclonedx_license_entry(license_str: str) -> dict:
    """把 manifest 的 license 字符串映射为 CycloneDX licenses[] 元素。"""
    if is_spdx_expression(license_str):
        return {"expression": license_str}
    if license_str.startswith("LicenseRef-"):
        return {"license": {"name": license_str}}
    return {"license": {"id": license_str}}
