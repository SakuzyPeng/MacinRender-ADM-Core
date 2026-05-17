#!/usr/bin/env bash
set -euo pipefail

repo_root="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
build_dir="${1:-$repo_root/build/debug}"

clang_tidy_bin="${CLANG_TIDY:-}"
if [[ -z "$clang_tidy_bin" ]]; then
    if command -v clang-tidy >/dev/null 2>&1; then
        clang_tidy_bin="$(command -v clang-tidy)"
    elif [[ -x /opt/homebrew/opt/llvm/bin/clang-tidy ]]; then
        clang_tidy_bin="/opt/homebrew/opt/llvm/bin/clang-tidy"
    elif [[ -x /usr/local/opt/llvm/bin/clang-tidy ]]; then
        clang_tidy_bin="/usr/local/opt/llvm/bin/clang-tidy"
    fi
fi

if [[ -z "$clang_tidy_bin" || ! -x "$clang_tidy_bin" ]]; then
    echo "[ERROR] clang-tidy not found. Install LLVM tools, for example: brew install llvm" >&2
    exit 127
fi

if [[ ! -f "$build_dir/compile_commands.json" ]]; then
    echo "[ERROR] compile_commands.json not found in $build_dir" >&2
    echo "[INFO] Run: cmake --preset debug" >&2
    exit 2
fi

extra_args=()
if [[ "$(uname -s)" == "Darwin" ]]; then
    extra_args+=(--extra-arg=-target --extra-arg="$(uname -m)-apple-macos")

    sdk_path="$(xcrun --show-sdk-path 2>/dev/null || true)"
    if [[ -n "$sdk_path" ]]; then
        extra_args+=(--extra-arg=-isysroot --extra-arg="$sdk_path")
    fi
fi

files=()
while IFS= read -r -d '' file; do
    files+=("$file")
done < <(
    find "$repo_root/src" "$repo_root/tests" \
        -type f \( -name '*.cc' -o -name '*.cpp' -o -name '*.cxx' \) \
        -print0
)

if [[ ${#files[@]} -eq 0 ]]; then
    echo "[INFO] No C++ implementation files found"
    exit 0
fi

"$clang_tidy_bin" --quiet -p "$build_dir" --config-file "$repo_root/.clang-tidy" "${extra_args[@]}" "${files[@]}" \
    2> >(grep -vE '^[0-9]+ warnings generated\.$' >&2)
