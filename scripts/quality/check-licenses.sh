#!/usr/bin/env bash
set -euo pipefail

repo_root="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
build_dir=""
require_full=0

usage() {
    cat <<'USAGE'
Usage: scripts/quality/check-licenses.sh [--build-dir DIR] [--require-full]

校验第三方许可的单一事实源一致性：
  Tier 1（无需构建）：
    - manifest.json 的 fetchcontent 组件 vs cmake/MRDependencies.cmake GIT_TAG 一致
    - third_party/sbom.cyclonedx.json 与 docs 依赖表与 manifest 同步（生成物不 stale）
    - third_party/licenses/ bundle 每个组件声明的文件齐全
  Tier 2（给 --build-dir 且其 _deps 完整时）：
    - bundle 字节与新鲜 _deps 一致（依赖升级后未刷新会失败）

选项：
  --require-full   要求所有依赖的 _deps 都在该构建中（缺即失败）。本地质量门可不带；
                   发布流程应带上（配合全 FetchContent 构建），强制全量覆盖。

Defaults: 不带 --build-dir 时只跑 Tier 1。
USAGE
}

while [[ $# -gt 0 ]]; do
    case "$1" in
        --build-dir)
            build_dir="${2:?missing value for --build-dir}"
            shift 2
            ;;
        --require-full)
            require_full=1
            shift
            ;;
        -h|--help)
            usage
            exit 0
            ;;
        *)
            usage >&2
            exit 2
            ;;
    esac
done

python_bin="${PYTHON:-python3}"
if ! command -v "$python_bin" >/dev/null 2>&1; then
    echo "[ERROR] 未找到 python3" >&2
    exit 127
fi

lic_dir="$repo_root/scripts/licenses"

echo "[INFO] license: 校验 manifest 与 MRDependencies.cmake 一致"
"$python_bin" "$lic_dir/check_manifest.py"

echo "[INFO] license: 校验 SBOM 与 docs 依赖表未 stale"
"$python_bin" "$lic_dir/generate.py" --check

echo "[INFO] license: 校验 license 原文 bundle"
sync_args=(--check)
if [[ -n "$build_dir" ]]; then
    sync_args+=(--build-dir "$build_dir")
fi
if [[ "$require_full" -eq 1 ]]; then
    sync_args+=(--require-full)
fi
"$python_bin" "$lic_dir/sync_license_texts.py" "${sync_args[@]}"

echo "[INFO] license: 全部检查通过"
