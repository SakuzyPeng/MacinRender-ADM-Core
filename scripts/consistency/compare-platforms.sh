#!/usr/bin/env bash
# Compare complete, validated platform baselines. Numerical divergence is measurement unless
# gated by the gate list; missing inputs/outputs and tool/IO errors always fail.
# usage: compare-platforms.sh [--expected <file>] <pcm-bits-tool> <dir1> <dir2> [dir3 ...]
#
# --expected selects the gate list, defaulting to expected-identical.txt next to this script.
# The default and controlled builds (roadmap §5.1 configs A and B) converge on different sets of
# cases, so they keep separate lists: gating the default build on something only the controlled
# build achieves would make it permanently red for a result it never claimed.
set -euo pipefail
export LC_ALL=C
shopt -s nullglob

fail() { echo "error: $*" >&2; exit 2; }
label_of() { basename "$1"; }

script_dir="$(cd "$(dirname "$0")" && pwd)"
expected_list="${script_dir}/expected-identical.txt"
while [ "$#" -gt 0 ]; do
    case "$1" in
        --expected)
            [ "$#" -ge 2 ] || fail "--expected needs a file"
            expected_list="$2"
            shift 2
            ;;
        --) shift; break ;;
        *) break ;;
    esac
done

if [ "$#" -lt 3 ]; then
    fail "usage: $0 [--expected <file>] <pcm-bits-tool> <dir1> <dir2> [dir3 ...]"
fi
pcm_bits="$1"
shift
dirs=("$@")
[ -x "$pcm_bits" ] || fail "missing or non-executable comparison tool: $pcm_bits"
[ -f "$expected_list" ] || fail "missing gate configuration: $expected_list"
validation_dir="$(mktemp -d)"
trap 'rm -rf "$validation_dir"' EXIT

# Validate the inventories before interpreting any difference as a numerical result. Keep
# sorted lists in private temporary files so order is irrelevant and producer errors propagate.
for ((i = 0; i < ${#dirs[@]}; i++)); do
    d="${dirs[i]}"
    [ -f "$d/cases.txt" ] || fail "missing cases.txt on $d"
    [ -d "$d/fixtures" ] && [ -d "$d/pcm" ] || fail "missing fixture/PCM directory on $d"
    : >"$validation_dir/cases-$i"
    while IFS= read -r name || [ -n "$name" ]; do
        name="${name%$'\r'}"
        [[ "$name" =~ ^[a-zA-Z0-9][a-zA-Z0-9_.-]*$ ]] || fail "invalid case name on $d: $name"
        printf '%s\n' "$name" >>"$validation_dir/cases-$i"
    done <"$d/cases.txt"
    [ -s "$validation_dir/cases-$i" ] || fail "empty case inventory on $d"
    sort -o "$validation_dir/cases-$i" "$validation_dir/cases-$i"
    duplicates="$(uniq -d "$validation_dir/cases-$i")"
    [ -z "$duplicates" ] || fail "duplicate cases on $d: $duplicates"

    fixtures=("$d/fixtures/"*.wav)
    [ "${#fixtures[@]}" -gt 0 ] || fail "empty fixture inventory on $d"
    : >"$validation_dir/fixtures-$i"
    for f in "${fixtures[@]}"; do
        [ -f "$f" ] || fail "fixture is not a file: $f"
        basename "$f" >>"$validation_dir/fixtures-$i"
    done
    sort -o "$validation_dir/fixtures-$i" "$validation_dir/fixtures-$i"
    if [ "$i" -ne 0 ]; then
        cmp -s "$validation_dir/cases-0" "$validation_dir/cases-$i" || fail "case inventories differ on $d"
        cmp -s "$validation_dir/fixtures-0" "$validation_dir/fixtures-$i" || fail "fixture inventories differ on $d"
    fi
    while IFS= read -r name; do
        bits="$d/pcm/$name.pcmbits"
        [ -f "$bits" ] || fail "missing PCM output: $bits"
        "$pcm_bits" validate "$bits" || fail "invalid PCM image or failed validator: $bits"
    done <"$validation_dir/cases-$i"
done

# A missing or misspelled gate must not silently disable an assertion.
: >"$validation_dir/gates"
while IFS= read -r line || [ -n "$line" ]; do
    line="${line%%#*}"
    line="$(printf '%s' "$line" | sed 's/^[[:space:]]*//;s/[[:space:]]*$//')"
    [ -n "$line" ] || continue
    [[ "$line" =~ ^[a-zA-Z0-9][a-zA-Z0-9_.-]*$ ]] || fail "invalid gated case: $line"
    grep -Fxq -- "$line" "$validation_dir/cases-0" || fail "gated case absent from inventory: $line"
    printf '%s\n' "$line" >>"$validation_dir/gates"
done <"$expected_list"

printf 'Cross-platform numerical baseline\nplatforms:\n'
for d in "${dirs[@]}"; do
    echo "  - $(label_of "$d")"
done

echo "== input fixtures =="
ref="${dirs[0]}"
while IFS= read -r name; do
    for d in "${dirs[@]:1}"; do
        if ! cmp -s "$ref/fixtures/$name" "$d/fixtures/$name"; then
            echo "error: input fixture differs or is unreadable: $name on $d" >&2
            exit 1
        fi
    done
    printf '  %-28s identical\n' "$name"
done <"$validation_dir/fixtures-0"

echo "== rendered PCM (all platform pairs) =="
identical=()
differing=()
while IFS= read -r name; do
    case_identical=1
    for ((i = 0; i < ${#dirs[@]} - 1; i++)); do
        for ((j = i + 1; j < ${#dirs[@]}; j++)); do
            a="${dirs[i]}/pcm/$name.pcmbits"
            b="${dirs[j]}/pcm/$name.pcmbits"
            printf '  %s: %s vs %s: ' "$name" "$(label_of "${dirs[i]}")" "$(label_of "${dirs[j]}")"
            if "$pcm_bits" compare "$a" "$b" >"$validation_dir/comparison" 2>&1; then
                echo "identical"
            else
                status=$?
                cat "$validation_dir/comparison"
                [ "$status" -eq 1 ] || fail "comparison tool failed with status $status for $name"
                case_identical=0
            fi
        done
    done
    if [ "$case_identical" -eq 1 ]; then
        identical+=("$name")
    else
        differing+=("$name")
    fi
done <"$validation_dir/cases-0"

printf '\n== summary ==\n  identical: %s\n  differing: %s\n' "${#identical[@]}" "${#differing[@]}"
printf '%s\n' "${identical[@]:-}" >"$validation_dir/identical"
echo "== gated cases ($(basename "$expected_list")) =="
gate_fail=0
while IFS= read -r name; do
    if grep -Fxq -- "$name" "$validation_dir/identical"; then
        printf '  %-32s ok\n' "$name"
    else
        printf '  %-32s REGRESSED\n' "$name"
        gate_fail=1
    fi
done <"$validation_dir/gates"
if [ ! -s "$validation_dir/gates" ]; then
    echo "  (none yet — populate from the first full three-platform run)"
fi
if [ "$gate_fail" -ne 0 ]; then
    echo "error: a gated case is no longer bit-identical." >&2
    exit 1
fi
echo "baseline recorded; ungated numerical divergence is measurement, not a gate."
