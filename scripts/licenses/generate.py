#!/usr/bin/env python3
"""从 third_party/manifest.json 派生 CycloneDX SBOM 与 docs 依赖表。

唯一事实源是 manifest.json。本脚本幂等：
  - 写模式（默认）：生成 third_party/sbom.cyclonedx.json，并把依赖表注入
    docs/THIRD_PARTY_LICENSES.md 的 <!-- BEGIN GENERATED DEPS --> / <!-- END GENERATED DEPS --> 之间。
  - --check：只比对，不写盘；任何不一致以非零退出（用于 CI 防生成物 stale）。

不引入任何第三方库——CycloneDX 只是一份 JSON schema，直接用标准库拼。
"""

from __future__ import annotations

import argparse
import json
import sys

import _common

BEGIN_MARK = "<!-- BEGIN GENERATED DEPS -->"
END_MARK = "<!-- END GENERATED DEPS -->"

DIST_LABEL = {
    "available": "可用",
    "macos-only": "macOS only",
    "opt-in": "opt-in（默认关闭）",
    "system": "平台 SDK（不再分发）",
    "gpl-gated-off": "默认禁用",
}


def build_sbom(components: list[dict]) -> dict:
    """构造确定性的 CycloneDX 1.5 文档（不含 timestamp/serialNumber 以保证幂等）。"""
    sbom_components = []
    for c in components:
        entry = {
            "type": "library",
            "name": c["name"],
            "version": c["version"],
            "description": c.get("purpose", ""),
            "licenses": [_common.cyclonedx_license_entry(c["license"])],
            "purl": c["purl"],
            "properties": [
                {"name": "mradm:distribution", "value": c.get("distribution", "")},
                {"name": "mradm:source-type", "value": c.get("source", {}).get("type", "")},
            ],
        }
        sbom_components.append(entry)
    return {
        "bomFormat": "CycloneDX",
        "specVersion": "1.5",
        "version": 1,
        "metadata": {
            "component": {
                "type": "application",
                "name": "MacinRender ADM Core",
                "description": "跨平台 ADM 空间音频渲染核心（mradm CLI + 稳定 C ABI 库）",
                "licenses": [{"license": {"id": "MIT"}}],
            }
        },
        "components": sbom_components,
    }


def render_sbom(components: list[dict]) -> str:
    return json.dumps(build_sbom(components), ensure_ascii=False, indent=2) + "\n"


def render_deps_table(components: list[dict]) -> str:
    lines = [
        BEGIN_MARK,
        "<!-- 本块由 scripts/licenses/generate.py 从 third_party/manifest.json 生成，请勿手改。 -->",
        "",
        "| 依赖 | 用途 | 许可证 (SPDX) | 默认发行可用性 | 备注 |",
        "|---|---|---|---|---|",
    ]
    for c in components:
        dist = DIST_LABEL.get(c.get("distribution", ""), c.get("distribution", ""))
        notes = c.get("notes", "").replace("|", "\\|")
        lines.append(
            f"| `{c['name']}` @ {c['version']} | {c.get('purpose', '')} | "
            f"{c['license']} | {dist} | {notes} |"
        )
    lines.append("")
    lines.append(END_MARK)
    return "\n".join(lines)


def inject_table(doc_text: str, table_block: str) -> str:
    if BEGIN_MARK not in doc_text or END_MARK not in doc_text:
        raise SystemExit(
            f"[ERROR] {_common.notices_doc_path()} 缺少 {BEGIN_MARK} / {END_MARK} 标记"
        )
    pre = doc_text.split(BEGIN_MARK, 1)[0]
    post = doc_text.split(END_MARK, 1)[1]
    return pre + table_block + post


def main() -> int:
    ap = argparse.ArgumentParser(description="生成 CycloneDX SBOM 与 docs 依赖表")
    ap.add_argument("--check", action="store_true", help="只校验生成物是否最新，不写盘")
    args = ap.parse_args()

    components = _common.load_components()
    sbom_text = render_sbom(components)
    table_block = render_deps_table(components)

    sbom_p = _common.sbom_path()
    doc_p = _common.notices_doc_path()
    doc_text = doc_p.read_text(encoding="utf-8")
    new_doc_text = inject_table(doc_text, table_block)

    if args.check:
        stale = []
        if not sbom_p.exists() or sbom_p.read_text(encoding="utf-8") != sbom_text:
            stale.append(str(sbom_p.relative_to(_common.repo_root())))
        if doc_text != new_doc_text:
            stale.append(str(doc_p.relative_to(_common.repo_root())))
        if stale:
            print("[ERROR] 生成物已过期，请运行 python3 scripts/licenses/generate.py：", file=sys.stderr)
            for s in stale:
                print(f"  - {s}", file=sys.stderr)
            return 1
        print("[INFO] SBOM 与 docs 依赖表与 manifest 一致")
        return 0

    sbom_p.parent.mkdir(parents=True, exist_ok=True)
    sbom_p.write_text(sbom_text, encoding="utf-8")
    doc_p.write_text(new_doc_text, encoding="utf-8")
    print(f"[INFO] 已写 {sbom_p.relative_to(_common.repo_root())}")
    print(f"[INFO] 已更新 {doc_p.relative_to(_common.repo_root())} 依赖表块")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
