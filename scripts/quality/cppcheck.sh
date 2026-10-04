#!/usr/bin/env bash
set -euo pipefail

repo_root="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
_build_dir="${1:-$repo_root/build/debug}"

if ! command -v cppcheck >/dev/null 2>&1; then
    echo "[ERROR] cppcheck not found. Install it, for example: brew install cppcheck" >&2
    exit 127
fi

# -i src/adm_windows: Windows-only sink (windows.h / spatialaudioclient.h) — cppcheck can't parse the
# Windows COM TUs on the macOS quality host; the windows-debug CI build verifies it instead.
reference_excludes=()
if ! grep -q '^MR_ADM_BUILD_SAF_REFERENCE_TESTS:BOOL=ON$' "$_build_dir/CMakeCache.txt"; then
    for file in tests/reference/saf_reference_test.cpp tests/reference/spreader_mr.c tests/reference/spreader_mr.h tests/reference/spreader_mr_internal.h tests/tools/export_saf_hoa_matrix.cpp tests/tools/numeric_probe.cpp tests/tools/vbap_probe.cpp tests/tools/vbap_probe_backend.c tests/tools/fft_lifecycle_probe.cpp; do
        reference_excludes+=(-i "$repo_root/$file")
    done
fi
if ! grep -q '^MR_ADM_BUILD_SAMPLERATE_REFERENCE_TESTS:BOOL=ON$' "$_build_dir/CMakeCache.txt"; then
    reference_excludes+=(-i "$repo_root/tests/reference/resampler_reference_test.cpp")
fi
if ! grep -q '^MR_ADM_BUILD_EBUR128_REFERENCE_TESTS:BOOL=ON$' "$_build_dir/CMakeCache.txt"; then
    reference_excludes+=(-i "$repo_root/tests/reference/ebur128_reference_test.cpp")
fi
cppcheck \
    --enable=warning,style,performance,portability \
    --std=c++20 \
    --inline-suppr \
    --error-exitcode=2 \
    --suppressions-list="$repo_root/CppcheckSuppressions.txt" \
    -I "$repo_root/include" \
    -i "$repo_root/src/adm_windows" \
    "${reference_excludes[@]}" \
    "$repo_root/include" "$repo_root/src" "$repo_root/tests"
