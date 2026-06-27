#!/usr/bin/env python3
"""校验 third_party/manifest.json 的 fetchcontent 组件与 cmake/MRDependencies.cmake 一致。

解析 MRDependencies.cmake 中每个 `package_name STREQUAL "<key>"` 块内的 `GIT_TAG <ver>`，
与 manifest 里 cmake_key 非空的组件逐项比对：缺项 / 多项 / 版本不符均以非零退出。

这是堵住「加了依赖却忘记登记许可」漂移的第一道防线（例如历史上 nlohmann_json 漏登记）。
"""

from __future__ import annotations

import re
import sys

import _common

KEY_RE = re.compile(r'package_name\s+STREQUAL\s+"([^"]+)"')
TAG_RE = re.compile(r"GIT_TAG\s+(\S+)")


def parse_cmake() -> dict[str, str]:
    text = (_common.repo_root() / "cmake" / "MRDependencies.cmake").read_text(encoding="utf-8")
    result: dict[str, str] = {}
    current: str | None = None
    for line in text.splitlines():
        m = KEY_RE.search(line)
        if m:
            current = m.group(1)
            continue
        if current is not None:
            t = TAG_RE.search(line)
            if t:
                # 去掉可能尾随的 ')'，并只取第一个 GIT_TAG
                result.setdefault(current, t.group(1).rstrip(")"))
    return result


def main() -> int:
    cmake = parse_cmake()
    components = _common.load_components()
    manifest = {c["cmake_key"]: c["version"] for c in components if c.get("cmake_key")}

    errors: list[str] = []

    for key, ver in cmake.items():
        if key not in manifest:
            errors.append(f"MRDependencies.cmake 有依赖 '{key}'@{ver}，但 manifest 未登记")
        elif manifest[key] != ver:
            errors.append(f"'{key}' 版本不符：cmake={ver} vs manifest={manifest[key]}")

    for key, ver in manifest.items():
        if key not in cmake:
            errors.append(f"manifest 登记 fetchcontent 依赖 '{key}'@{ver}，但 MRDependencies.cmake 未见")

    if errors:
        print("[ERROR] manifest 与 MRDependencies.cmake 不一致：", file=sys.stderr)
        for e in errors:
            print(f"  - {e}", file=sys.stderr)
        return 1

    print(f"[INFO] manifest 与 MRDependencies.cmake 一致（{len(cmake)} 个 fetchcontent 依赖）")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
