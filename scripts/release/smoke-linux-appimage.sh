#!/usr/bin/env bash
set -euo pipefail

usage() {
    cat <<'EOF'
Usage: scripts/release/smoke-linux-appimage.sh <dist/mradm-...-linux-x86_64.AppImage>

Verifies the AppImage checksum, extracts metadata, and runs basic CLI smoke checks.
EOF
}

if [[ $# -ne 1 || "${1:-}" == "-h" || "${1:-}" == "--help" ]]; then
    usage
    exit 2
fi

appimage="$1"
checksum="$appimage.sha256"

if [[ ! -f "$appimage" ]]; then
    echo "AppImage is missing: $appimage" >&2
    exit 1
fi

if [[ ! -f "$checksum" ]]; then
    echo "AppImage checksum is missing: $checksum" >&2
    exit 1
fi

dist_dir="$(cd "$(dirname "$appimage")" && pwd)"
appimage_name="$(basename "$appimage")"
checksum_name="$(basename "$checksum")"
work_dir="$(mktemp -d)"
trap 'rm -rf "$work_dir"' EXIT

(
    cd "$dist_dir"
    sha256sum -c "$checksum_name"
)

chmod +x "$dist_dir/$appimage_name"

(
    cd "$work_dir"
    "$dist_dir/$appimage_name" --appimage-extract >/dev/null
)

extracted="$work_dir/squashfs-root"
for required in usr/bin/mradm usr/share/doc/macinrender-adm-core/LICENSE \
    usr/share/doc/macinrender-adm-core/THIRD_PARTY_NOTICES.md \
    usr/share/doc/macinrender-adm-core/BUILD_INFO.txt \
    usr/share/doc/macinrender-adm-core/DEPENDENCIES.txt; do
    if [[ ! -e "$extracted/$required" ]]; then
        echo "AppImage is missing $required" >&2
        exit 1
    fi
done

if grep -F 'not found' "$extracted/usr/share/doc/macinrender-adm-core/DEPENDENCIES.txt" >/dev/null; then
    echo "AppImage dependency manifest contains unresolved libraries:" >&2
    grep -F 'not found' "$extracted/usr/share/doc/macinrender-adm-core/DEPENDENCIES.txt" >&2
    exit 1
fi

if grep -E '=>[[:space:]]+/usr/local/' "$extracted/usr/share/doc/macinrender-adm-core/DEPENDENCIES.txt" >/dev/null; then
    echo "AppImage dependency manifest contains /usr/local libraries:" >&2
    grep -E '=>[[:space:]]+/usr/local/' "$extracted/usr/share/doc/macinrender-adm-core/DEPENDENCIES.txt" >&2
    exit 1
fi

export APPIMAGE_EXTRACT_AND_RUN=1
"$dist_dir/$appimage_name" --version
"$dist_dir/$appimage_name" backends
