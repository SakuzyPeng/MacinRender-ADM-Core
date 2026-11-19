#!/usr/bin/env bash
# Builds the release products with Cargo (6.6): the shared libmradm_capi and the mradm executable of
# mradm-capi, with the islands and their C libraries built by mradm-islands-sys through the cmake crate
# (vendored libFLAC / libopus, no installed packages). On macOS mradm gets the full ad-hoc signature
# the APAC encoder needs. Prints the directory that holds the products.
set -euo pipefail

usage() {
    cat >&2 <<'EOF'
Usage: scripts/release/build-cargo-release.sh --target-dir <dir> [--target <triple>]

Environment: FC_CACHE_DIR (optional FetchContent cache; the islands' sources go to $FC_CACHE_DIR/cargo),
MR_ADM_IAMF_AOM_ROOT (optional prebuilt AOM iamf-tools bridge SDK; enables IAMF).
EOF
}

target_dir=""
target=""
while [[ $# -gt 0 ]]; do
    case "$1" in
        --target-dir) target_dir="${2:-}"; shift 2 ;;
        --target) target="${2:-}"; shift 2 ;;
        -h|--help) usage; exit 0 ;;
        *) echo "unknown argument: $1" >&2; usage; exit 2 ;;
    esac
done
if [[ -z "$target_dir" ]]; then
    usage
    exit 2
fi

repo_root="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
mkdir -p "$target_dir"
target_dir="$(cd "$target_dir" && pwd)"

cmake_args="MR_ADM_CORE_USE_INSTALLED_DEPS=OFF"
if [[ -n "${FC_CACHE_DIR:-}" ]]; then
    # A cache of its own: FetchContent also keeps each dependency's build tree in its base directory.
    mkdir -p "$FC_CACHE_DIR/cargo"
    cmake_args+=";FETCHCONTENT_BASE_DIR=$FC_CACHE_DIR/cargo"
fi
if command -v ccache >/dev/null 2>&1; then
    cmake_args+=";CMAKE_C_COMPILER_LAUNCHER=ccache;CMAKE_CXX_COMPILER_LAUNCHER=ccache"
fi
case "$target" in
    aarch64-apple-darwin) cmake_args+=";CMAKE_OSX_ARCHITECTURES=arm64" ;;
    x86_64-apple-darwin) cmake_args+=";CMAKE_OSX_ARCHITECTURES=x86_64" ;;
esac

target_args=()
out_dir="$target_dir/release"
if [[ -n "$target" ]]; then
    target_args=(--target "$target")
    out_dir="$target_dir/$target/release"
fi

(
    cd "$repo_root/rust"
    env -u MRADM_NATIVE_LINK_FILE MR_ADM_FLAC_PROVIDER=VENDORED MR_ADM_OPUS_PROVIDER=VENDORED \
        MRADM_NATIVE_CMAKE_ARGS="$cmake_args" \
        cargo build --release --locked -p mradm-capi --target-dir "$target_dir" ${target_args[@]+"${target_args[@]}"} >&2
)

product_version="$(python3 "$repo_root/scripts/release/version_metadata.py" --field product-version)"
major="${product_version%%.*}"
case "$(uname -s)" in
    Darwin)
        library="libmradm_capi.dylib"
        runtime_library="libmradm_capi.$major.dylib"
        # AudioToolbox's APAC encoder rejects ClientDataFormat when the helper only carries the
        # linker's transient signature.
        /usr/bin/codesign --force --sign - "$out_dir/mradm" >&2
        ;;
    *)
        library="libmradm_capi.so"
        runtime_library="libmradm_capi.so.$major"
        ;;
esac
python3 "$repo_root/scripts/quality/check-capi-exports.py" "$out_dir/$library" >&2
# Match the install name / SONAME recorded in consumers of the Cargo-built library.
ln -sfn "$library" "$out_dir/$runtime_library"
echo "$out_dir"
