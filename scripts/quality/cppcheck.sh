#!/usr/bin/env bash
set -euo pipefail

repo_root="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
_build_dir="${1:-$repo_root/build/debug}"

if ! command -v cppcheck >/dev/null 2>&1; then
    echo "[ERROR] cppcheck not found. Install it, for example: brew install cppcheck" >&2
    exit 127
fi

cppcheck \
    --enable=warning,style,performance,portability \
    --std=c++20 \
    --inline-suppr \
    --error-exitcode=2 \
    --suppressions-list="$repo_root/CppcheckSuppressions.txt" \
    -I "$repo_root/include" \
    "$repo_root/include" "$repo_root/src" "$repo_root/tests"
