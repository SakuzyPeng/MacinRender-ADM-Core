#!/usr/bin/env bash
set -euo pipefail

usage() {
    cat <<'EOF'
Usage: scripts/release/smoke-package.sh <dist/mradm-...-macos-....tar.gz>

Verifies the macOS CLI package checksum, extracts it to a temporary directory, and runs basic CLI smoke checks.
EOF
}

if [[ $# -ne 1 || "${1:-}" == "-h" || "${1:-}" == "--help" ]]; then
    usage
    exit 2
fi

archive="$1"
checksum="$archive.sha256"

if [[ ! -f "$archive" ]]; then
    echo "package archive is missing: $archive" >&2
    exit 1
fi

if [[ ! -f "$checksum" ]]; then
    echo "package checksum is missing: $checksum" >&2
    exit 1
fi

dist_dir="$(cd "$(dirname "$archive")" && pwd)"
archive_name="$(basename "$archive")"
checksum_name="$(basename "$checksum")"
work_dir="$(mktemp -d)"
trap 'rm -rf "$work_dir"' EXIT

(
    cd "$dist_dir"
    if command -v shasum >/dev/null 2>&1; then
        shasum -a 256 -c "$checksum_name"
    else
        sha256sum -c "$checksum_name"
    fi
)

tar -xzf "$dist_dir/$archive_name" -C "$work_dir"
package_root="$(find "$work_dir" -mindepth 1 -maxdepth 1 -type d -print -quit)"
if [[ -z "$package_root" ]]; then
    echo "package archive did not contain a top-level directory" >&2
    exit 1
fi

for required in bin/mradm LICENSE THIRD_PARTY_NOTICES.md BUILD_INFO.txt DEPENDENCIES.txt; do
    if [[ ! -e "$package_root/$required" ]]; then
        echo "package is missing $required" >&2
        exit 1
    fi
done

if [[ ! -x "$package_root/bin/mradm" ]]; then
    echo "package binary is not executable: $package_root/bin/mradm" >&2
    exit 1
fi

"$package_root/bin/mradm" --version
"$package_root/bin/mradm" backends

if grep -F 'not found' "$package_root/DEPENDENCIES.txt" >/dev/null; then
    echo "package dependency manifest contains unresolved libraries:" >&2
    grep -F 'not found' "$package_root/DEPENDENCIES.txt" >&2
    exit 1
fi
