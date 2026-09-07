#!/usr/bin/env bash
# Record how one platform actually built the consistency baseline.
#
# usage: build-info.sh <build-dir> <out-file>
#
# The roadmap's phase 0 exit condition asks for a per-platform build and dependency record
# (docs/architecture/RUST_SAF_REPLACEMENT_ROADMAP.md §5.1). Two things make it necessary rather
# than decorative:
#   - The controlled build (config B) only means something if the flags actually reached the
#     compiler. compile_commands.json is the ground truth for that; the CMake option is only an
#     intent. This script counts the flags in the real command lines.
#   - System prebuilt libraries (macOS Accelerate, Windows/Linux OpenBLAS) are outside every
#     project compile option, so which backend SAF picked has to be recorded, not assumed.
#
# Output is a stable key=value text file, sorted where order is not meaningful, so two
# platforms' records diff cleanly.

set -euo pipefail

if [ "$#" -ne 2 ]; then
    echo "usage: $0 <build-dir> <out-file>" >&2
    exit 2
fi

build_dir="$1"
out_file="$2"
cache="${build_dir}/CMakeCache.txt"
ccjson="${build_dir}/compile_commands.json"

if [ ! -f "$cache" ]; then
    echo "error: no CMakeCache.txt in ${build_dir}" >&2
    exit 2
fi

mkdir -p "$(dirname "$out_file")"

# CMakeCache.txt lines are KEY:TYPE=VALUE.
cache_get() {
    sed -n "s/^$1:[^=]*=\(.*\)$/\1/p" "$cache" | head -n 1
}

{
    echo "# MacinRender ADM Core — consistency build record"
    echo "record.version=1"
    echo "record.build_dir=$(basename "$build_dir")"

    echo "platform.system=$(uname -s 2>/dev/null || echo unknown)"
    echo "platform.machine=$(uname -m 2>/dev/null || echo unknown)"
    echo "platform.release=$(uname -r 2>/dev/null || echo unknown)"

    # CMAKE_<lang>_COMPILER_ID/VERSION are configure-time variables, not cache entries, so they
    # have to come from the generated compiler info files instead of CMakeCache.txt.
    for lang in C CXX; do
        info="$(ls "${build_dir}"/CMakeFiles/*/CMake${lang}Compiler.cmake 2>/dev/null | head -n 1)"
        for prop in COMPILER_ID COMPILER_VERSION; do
            value=""
            if [ -n "$info" ]; then
                value="$(sed -n "s/^set(CMAKE_${lang}_${prop} \"\(.*\)\")$/\1/p" "$info" | head -n 1)"
            fi
            printf 'compiler.%s.%s=%s\n' "$lang" "$prop" "$value"
        done
    done

    for key in \
        CMAKE_BUILD_TYPE CMAKE_CXX_COMPILER CMAKE_C_COMPILER \
        CMAKE_C_FLAGS CMAKE_CXX_FLAGS CMAKE_CXX_FLAGS_RELEASE CMAKE_OSX_ARCHITECTURES \
        MR_ADM_STRICT_FP MR_ADM_EAR_SCALAR_REFERENCE MR_ADM_HAVE_MSVC_FP_CONTRACT_OFF \
        MR_ADM_CORE_USE_INSTALLED_DEPS MR_ADM_FLAC_PROVIDER MR_ADM_OPUS_PROVIDER \
        MR_ADM_ENABLE_SOFA MR_ADM_ENABLE_IAMF \
        EAR_SIMD SAF_PERFORMANCE_LIB SAF_ENABLE_SIMD SAF_USE_FAST_MATH_FLAG; do
        printf 'cmake.%s=%s\n' "$key" "$(cache_get "$key")"
    done

    # ---- did the flags actually reach the compiler? ----
    if [ -f "$ccjson" ]; then
        count_flag() { grep -c -F -e "$1" "$ccjson" || true; }
        echo "compile.entries=$(grep -c '"file"' "$ccjson" || true)"
        echo "compile.ffp_contract_off=$(count_flag '-ffp-contract=off')"
        echo "compile.fp_precise=$(count_flag '/fp:precise')"
        echo "compile.fp_contract_off_msvc=$(count_flag '/fp:contract-')"
        # Known and expected to be non-zero: libopus builds its own encoder with fast math.
        # The baseline matrix writes float32 WAV, so that path carries no baseline PCM.
        echo "compile.fast_math=$(count_flag '-ffast-math')"
        echo "compile.fp_fast=$(count_flag '/fp:fast')"

        # One full command line for the record. compile_commands.json emits the keys in the
        # order directory, command, file — so the command for a given file can be pulled out
        # without a JSON parser (none is guaranteed on all three runners).
        sample="$(
            tr -d '\n' <"$ccjson" |
                sed 's/},[[:space:]]*{/\n/g' |
                grep -F 'ear_renderer.cpp' |
                sed -n 's/.*"command"[[:space:]]*:[[:space:]]*"\(.*\)"[[:space:]]*,[[:space:]]*"file".*/\1/p' |
                head -n 1
        )"
        if [ -n "$sample" ]; then
            echo "compile.sample.ear_renderer=${sample}"
        else
            echo "compile.sample.ear_renderer=unavailable"
        fi
    else
        echo "compile.entries=unavailable"
    fi

    # ---- dependency provenance ----
    if [ -d "${build_dir}/_deps" ]; then
        for src in "${build_dir}"/_deps/*-src; do
            [ -d "${src}/.git" ] || continue
            name="$(basename "$src")"
            name="${name%-src}"
            sha="$(git -C "$src" rev-parse HEAD 2>/dev/null || echo unknown)"
            echo "dep.${name}=${sha}"
        done | sort
    fi
} >"$out_file"

echo "wrote ${out_file}"
