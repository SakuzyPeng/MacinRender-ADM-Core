#!/usr/bin/env bash
# Render the cross-platform numerical baseline matrix and emit canonical PCM bit images.
#
# One script for all three runners: only the binary directory and executable suffix differ.
# Windows CI needs cmd + vcvars for the *build*, but running the built binaries needs no MSVC
# environment, so the render step uses bash there too.
#
# usage: render-matrix.sh <bin-dir> <out-dir> [exe-suffix]
#
# Outputs into <out-dir>:
#   fixtures/<kind>.wav        deterministic inputs (byte-compared across runners first)
#   pcm/<case>.pcmbits         canonical little-endian float32 bit image of the rendered PCM
#   cases.txt                  the case list actually run, for the compare job to iterate
#
# Design notes tied to the determinism contract (docs/architecture/RUST_SAF_REPLACEMENT_ROADMAP.md §6.1):
#   - Output is always float32. Integer quantisation and encoder output are validated separately.
#   - Rendered containers are never hashed: every output embeds the current UTC time, so two
#     identical renders differ as files. Only the extracted PCM is comparable.
#   - Post-processing is off by default here. Loudness/True Peak feed a measured gain back into
#     the PCM, so a divergence there would otherwise be indistinguishable from a renderer
#     divergence. One case deliberately leaves the default post-processing on to cover it.
#   - Binaural 'auto' selects the cloud path, not the SAF spreader. The spreader case sets
#     --binaural-spread-mode saf-spreader explicitly and uses an extent fixture, because objects
#     below the 1.0 deg extent gate bypass the spreader entirely.

set -euo pipefail

if [ "$#" -lt 2 ]; then
    echo "usage: $0 <bin-dir> <out-dir> [exe-suffix]" >&2
    exit 2
fi

bin_dir="$1"
out_dir="$2"
sfx="${3:-}"

mradm="${bin_dir}/mradm${sfx}"
pcm_bits="${bin_dir}/mr_adm_pcm_bits${sfx}"
make_fixture="${bin_dir}/mr_adm_make_fixture${sfx}"

for tool in "$mradm" "$pcm_bits" "$make_fixture"; do
    if [ ! -x "$tool" ]; then
        echo "error: missing or non-executable: $tool" >&2
        exit 2
    fi
done

fixture_dir="${out_dir}/fixtures"
pcm_dir="${out_dir}/pcm"
mkdir -p "$fixture_dir" "$pcm_dir"

echo "== generating fixtures =="
for kind in objects-point objects-extent directspeakers hoa; do
    "$make_fixture" "$kind" "${fixture_dir}/${kind}.wav"
done

# case-name | fixture-kind | extra mradm render arguments
cases=$(
    cat <<'EOF'
ear-5_1-point|objects-point|--renderer ear --output-layout 5.1 --no-peak-limit
ear-5_1-extent|objects-extent|--renderer ear --output-layout 5.1 --no-peak-limit
ear-5_1-directspeakers|directspeakers|--renderer ear --output-layout 5.1 --no-peak-limit
ear-5_1-hoa-input|hoa|--renderer ear --output-layout 5.1 --no-peak-limit
ear-5_1-point-postproc|objects-point|--renderer ear --output-layout 5.1
saf-5_1-point|objects-point|--renderer saf --output-layout 5.1 --no-peak-limit
saf-5_1-extent|objects-extent|--renderer saf --output-layout 5.1 --no-peak-limit
hoa-hoa3-point|objects-point|--renderer hoa --output-layout hoa3 --no-peak-limit
binaural-point|objects-point|--renderer saf-binaural --output-layout binaural --no-peak-limit
binaural-extent-cloud|objects-extent|--renderer saf-binaural --output-layout binaural --no-peak-limit --binaural-spread-mode cloud
binaural-extent-spreader|objects-extent|--renderer saf-binaural --output-layout binaural --no-peak-limit --binaural-spread-mode saf-spreader
EOF
)

: >"${out_dir}/cases.txt"
failures=0

echo "== rendering matrix =="
while IFS='|' read -r name kind args; do
    [ -n "$name" ] || continue
    wav="${out_dir}/${name}.wav"
    # shellcheck disable=SC2086 -- args is a deliberate word-split argument list
    if ! "$mradm" render -i "${fixture_dir}/${kind}.wav" -o "$wav" --output-bit-depth f32 $args >/dev/null 2>&1; then
        echo "FAIL render: $name" >&2
        failures=$((failures + 1))
        continue
    fi
    if ! "$pcm_bits" extract "$wav" "${pcm_dir}/${name}.pcmbits"; then
        echo "FAIL extract: $name" >&2
        failures=$((failures + 1))
        continue
    fi
    rm -f "$wav"
    echo "$name" >>"${out_dir}/cases.txt"
    echo "ok: $name"
done <<<"$cases"

if [ "$failures" -ne 0 ]; then
    echo "error: $failures case(s) failed to render or extract" >&2
    exit 1
fi

echo "== done: $(wc -l <"${out_dir}/cases.txt") case(s) =="
