#!/usr/bin/env bash
set -euo pipefail

repo_root="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
mode="fix"

usage() {
    cat <<'USAGE'
Usage: scripts/quality/format.sh [--check]

Formats C/C++ sources with clang-format. Use --check in CI.
USAGE
}

if [[ "${1:-}" == "--help" || "${1:-}" == "-h" ]]; then
    usage
    exit 0
fi

if [[ "${1:-}" == "--check" ]]; then
    mode="check"
elif [[ $# -gt 0 ]]; then
    usage
    exit 2
fi

clang_format_bin="${CLANG_FORMAT:-}"
if [[ -z "$clang_format_bin" ]]; then
    if command -v clang-format >/dev/null 2>&1; then
        clang_format_bin="$(command -v clang-format)"
    elif [[ -x /opt/homebrew/opt/llvm/bin/clang-format ]]; then
        clang_format_bin="/opt/homebrew/opt/llvm/bin/clang-format"
    elif [[ -x /usr/local/opt/llvm/bin/clang-format ]]; then
        clang_format_bin="/usr/local/opt/llvm/bin/clang-format"
    fi
fi

if [[ -z "$clang_format_bin" || ! -x "$clang_format_bin" ]]; then
    echo "[ERROR] clang-format not found. Install LLVM tools, for example: brew install llvm" >&2
    exit 127
fi

files=()
while IFS= read -r -d '' file; do
    files+=("$file")
done < <(
    find "$repo_root/include" "$repo_root/src" "$repo_root/tests" \
        -type f \( -name '*.h' -o -name '*.hpp' -o -name '*.c' -o -name '*.cc' -o -name '*.cpp' -o -name '*.cxx' \) \
        -print0
)

if [[ ${#files[@]} -eq 0 ]]; then
    echo "[INFO] No C/C++ source files found"
    exit 0
fi

if [[ "$mode" == "check" ]]; then
    "$clang_format_bin" --dry-run --Werror "${files[@]}"
else
    "$clang_format_bin" -i "${files[@]}"
fi
