#!/usr/bin/env bash
set -euo pipefail

repo_root="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
build_dir="${1:-$repo_root/build/debug}"

"$repo_root/scripts/quality/format.sh" --check
"$repo_root/scripts/quality/clang-tidy.sh" "$build_dir"
"$repo_root/scripts/quality/cppcheck.sh" "$build_dir"
