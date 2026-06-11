#!/usr/bin/env bash
set -euo pipefail

repo_root="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
base_ref="origin/main"
build_dir="$repo_root/build/debug"

usage() {
    cat <<'USAGE'
Usage: scripts/quality/check-changed.sh [--base REF] [--build-dir DIR]

Runs clang-format, clang-tidy, and cppcheck only on changed C/C++ files.
Changed files include:
  - commits since merge-base with REF
  - staged changes
  - unstaged changes

Defaults:
  --base origin/main
  --build-dir build/debug
USAGE
}

while [[ $# -gt 0 ]]; do
    case "$1" in
        --base)
            base_ref="${2:?missing value for --base}"
            shift 2
            ;;
        --build-dir)
            build_dir="${2:?missing value for --build-dir}"
            shift 2
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

cd "$repo_root"

if ! git rev-parse --verify --quiet "$base_ref" >/dev/null; then
    echo "[WARN] Base ref '$base_ref' not found; using HEAD" >&2
    base_ref="HEAD"
fi

merge_base="$(git merge-base "$base_ref" HEAD 2>/dev/null || git rev-parse HEAD)"

changed_files=()
while IFS= read -r file; do
    [[ -n "$file" ]] && changed_files+=("$file")
done < <(
    {
        git diff --name-only --diff-filter=ACMRT "$merge_base"...HEAD
        git diff --name-only --diff-filter=ACMRT --cached
        git diff --name-only --diff-filter=ACMRT
    } | sort -u
)

source_files=()
tidy_files=()
for file in "${changed_files[@]}"; do
    [[ -f "$file" ]] || continue
    case "$file" in
        include/*|src/*|tests/*)
            case "$file" in
                *.h|*.hpp|*.c|*.cc|*.cpp|*.cxx)
                    source_files+=("$repo_root/$file")
                    ;;
            esac
            case "$file" in
                *.cc|*.cpp|*.cxx)
                    tidy_files+=("$repo_root/$file")
                    ;;
            esac
            ;;
    esac
done

if [[ ${#source_files[@]} -eq 0 ]]; then
    echo "[INFO] No changed C/C++ files under include/, src/, or tests/"
    exit 0
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

echo "[INFO] clang-format: ${#source_files[@]} changed file(s)"
"$clang_format_bin" --dry-run --Werror "${source_files[@]}"

if [[ ${#tidy_files[@]} -gt 0 ]]; then
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
        sdk_path="$(xcrun --show-sdk-path 2>/dev/null || true)"
        if [[ -n "$sdk_path" ]]; then
            extra_args+=(--extra-arg=-isysroot --extra-arg="$sdk_path")
        fi
        # Homebrew clang-tidy parses Apple SDK headers with upstream Clang, not
        # Apple Clang. Keep the compile database target/arch intact and only add
        # SDK compatibility flags for diagnostics/macros that otherwise fail in
        # system headers before project code is checked.
        extra_args+=(--extra-arg=-Wno-elaborated-enum-base)
        extra_args+=('--extra-arg=-DINFINITY=__builtin_huge_valf()')
    fi

    echo "[INFO] clang-tidy: ${#tidy_files[@]} changed implementation file(s)"
    "$clang_tidy_bin" --quiet -p "$build_dir" --config-file "$repo_root/.clang-tidy" \
        "${extra_args[@]}" "${tidy_files[@]}" \
        2> >(grep -vE '^[0-9]+ warnings generated\.$' >&2)
else
    echo "[INFO] clang-tidy: no changed implementation files"
fi

if ! command -v cppcheck >/dev/null 2>&1; then
    echo "[ERROR] cppcheck not found. Install it, for example: brew install cppcheck" >&2
    exit 127
fi

echo "[INFO] cppcheck: ${#source_files[@]} changed file(s)"
cppcheck \
    --enable=warning,style,performance,portability \
    --language=c++ \
    --std=c++20 \
    --inline-suppr \
    --error-exitcode=2 \
    --suppressions-list="$repo_root/CppcheckSuppressions.txt" \
    -I "$repo_root/include" \
    "${source_files[@]}"
