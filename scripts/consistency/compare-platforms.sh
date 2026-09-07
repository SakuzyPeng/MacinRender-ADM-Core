#!/usr/bin/env bash
# Compare the numerical baseline outputs produced by several platforms.
#
# usage: compare-platforms.sh <pcm-bits-tool> <dir1> <dir2> [dir3 ...]
#
# Each <dir> is one platform's output from render-matrix.sh. Directory names are used as the
# platform labels in the report.
#
# Gate policy (docs/architecture/RUST_SAF_REPLACEMENT_ROADMAP.md §5.1): phase 0 measures the
# current state, so known divergence must not turn this into a permanently red job. The full
# table is always printed. The exit status is driven only by:
#   - input fixtures differing between platforms, which invalidates the whole comparison, and
#   - cases listed in expected-identical.txt, which are capabilities already shown to hold and
#     must therefore keep holding.
# As cases are fixed they get added to that list and become gated from then on.

set -euo pipefail

if [ "$#" -lt 3 ]; then
    echo "usage: $0 <pcm-bits-tool> <dir1> <dir2> [dir3 ...]" >&2
    exit 2
fi

pcm_bits="$1"
shift
dirs=("$@")
script_dir="$(cd "$(dirname "$0")" && pwd)"
expected_list="${script_dir}/expected-identical.txt"

label_of() { basename "$1"; }

echo "=============================================="
echo " Cross-platform numerical baseline"
echo "=============================================="
echo "platforms:"
for d in "${dirs[@]}"; do
    echo "  - $(label_of "$d")"
done
echo

# ---- gate 1: inputs must match, or nothing downstream means anything ----
echo "== input fixtures =="
input_fail=0
ref="${dirs[0]}"
for f in "${ref}/fixtures/"*.wav; do
    name="$(basename "$f")"
    status="identical"
    for d in "${dirs[@]:1}"; do
        other="${d}/fixtures/${name}"
        if [ ! -f "$other" ]; then
            status="MISSING on $(label_of "$d")"
            input_fail=1
            break
        fi
        if ! cmp -s "$f" "$other"; then
            status="DIFFERS on $(label_of "$d")"
            input_fail=1
            break
        fi
    done
    printf '  %-28s %s\n' "$name" "$status"
done
echo

if [ "$input_fail" -ne 0 ]; then
    echo "error: input fixtures differ between platforms; output comparison would be meaningless." >&2
    echo "       the generator is integer-only by construction, so this indicates a real bug." >&2
    exit 1
fi

# ---- the report ----
declare -a differing=()
declare -a identical=()

echo "== rendered PCM =="
while read -r case_name; do
    [ -n "$case_name" ] || continue
    ref_bits="${ref}/pcm/${case_name}.pcmbits"
    if [ ! -f "$ref_bits" ]; then
        printf '  %-32s %s\n' "$case_name" "MISSING on $(label_of "$ref")"
        differing+=("$case_name")
        continue
    fi

    case_status="identical"
    for d in "${dirs[@]:1}"; do
        other="${d}/pcm/${case_name}.pcmbits"
        if [ ! -f "$other" ]; then
            case_status="MISSING on $(label_of "$d")"
            break
        fi
        if ! cmp -s "$ref_bits" "$other"; then
            case_status="differs: $(label_of "$ref") vs $(label_of "$d")"
            break
        fi
    done

    printf '  %-32s %s\n' "$case_name" "$case_status"
    if [ "$case_status" = "identical" ]; then
        identical+=("$case_name")
    else
        differing+=("$case_name")
    fi
done <"${ref}/cases.txt"
echo

# ---- localisation for whatever differs ----
if [ "${#differing[@]}" -ne 0 ]; then
    echo "== first difference per diverging case =="
    for case_name in "${differing[@]}"; do
        for d in "${dirs[@]:1}"; do
            a="${ref}/pcm/${case_name}.pcmbits"
            b="${d}/pcm/${case_name}.pcmbits"
            if [ -f "$a" ] && [ -f "$b" ] && ! cmp -s "$a" "$b"; then
                echo "-- ${case_name}: $(label_of "$ref") vs $(label_of "$d")"
                "$pcm_bits" compare "$a" "$b" || true
                break
            fi
        done
    done
    echo
fi

echo "== summary =="
echo "  identical: ${#identical[@]}"
echo "  differing: ${#differing[@]}"
echo

# ---- gate 2: previously-passing cases must keep passing ----
gate_fail=0
if [ -f "$expected_list" ]; then
    echo "== gated cases (expected-identical.txt) =="
    gated=0
    while read -r line; do
        line="${line%%#*}"
        line="$(echo "$line" | tr -d '[:space:]')"
        [ -n "$line" ] || continue
        gated=$((gated + 1))
        if printf '%s\n' "${identical[@]:-}" | grep -qx "$line"; then
            printf '  %-32s ok\n' "$line"
        else
            printf '  %-32s REGRESSED\n' "$line"
            gate_fail=1
        fi
    done <"$expected_list"
    if [ "$gated" -eq 0 ]; then
        echo "  (none yet — populate from the first full three-platform run)"
    fi
    echo
fi

if [ "$gate_fail" -ne 0 ]; then
    echo "error: a case listed in expected-identical.txt is no longer bit-identical." >&2
    exit 1
fi

echo "baseline recorded. divergence above is measurement, not a gate;"
echo "add a case to scripts/consistency/expected-identical.txt once it is fixed."
