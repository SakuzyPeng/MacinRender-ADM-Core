#!/usr/bin/env bash
set -euo pipefail

usage() {
    cat <<'EOF'
Usage: scripts/release/smoke-macos-gui-release.sh <dist/MacinRender-Gui-...tar.gz>

Verifies the GUI package checksum, extracts it, checks macOS signing and
dependency policy, then runs the app's headless --selftest entry point. For
.NET single-file packages, self-extracted native libraries are also scanned.
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
    shasum -a 256 -c "$checksum_name"
)

tar -xzf "$dist_dir/$archive_name" -C "$work_dir"
package_root="$(find "$work_dir" -mindepth 1 -maxdepth 1 -type d -print -quit)"
if [[ -z "$package_root" ]]; then
    echo "package archive did not contain a top-level directory" >&2
    exit 1
fi

app_root="$package_root/MacinRender ADM.app"
exe="$app_root/Contents/MacOS/MacinRender.Gui"
deps="$package_root/DEPENDENCIES.txt"

for required in "$app_root" "$exe" "$app_root/Contents/Info.plist" "$app_root/Contents/Resources/AppIcon.icns" \
    "$deps" "$package_root/LICENSE" "$package_root/THIRD_PARTY_NOTICES.md" "$package_root/BUILD_INFO.txt" \
    "$package_root/licenses/INDEX.md" "$package_root/sbom.cyclonedx.json" \
    "$app_root/Contents/Resources/Legal/LICENSE" \
    "$app_root/Contents/Resources/Legal/THIRD_PARTY_NOTICES.md" \
    "$app_root/Contents/Resources/Legal/licenses/INDEX.md" \
    "$app_root/Contents/Resources/Legal/sbom.cyclonedx.json"; do
    if [[ ! -e "$required" ]]; then
        echo "package is missing ${required#$package_root/}" >&2
        exit 1
    fi
done

if [[ ! -x "$exe" ]]; then
    echo "package app executable is not executable: $exe" >&2
    exit 1
fi

if grep -E '^[[:space:]]+(/opt/homebrew|/usr/local|/Users/)' "$deps" >/dev/null; then
    echo "GUI package dependency manifest contains user/Homebrew/local libraries:" >&2
    grep -E '^[[:space:]]+(/opt/homebrew|/usr/local|/Users/)' "$deps" >&2
    exit 1
fi

while IFS= read -r dep; do
    [[ "$dep" =~ ^[[:space:]] ]] || continue
    dep_path="$(awk '{print $1}' <<<"$dep")"
    case "$dep_path" in
        @rpath/*|@executable_path/*) ;;
        /usr/lib/*) ;;
        /System/Library/Frameworks/*) ;;
        *)
            echo "GUI package links unexpected dependency: $dep_path" >&2
            exit 1
            ;;
    esac
done < "$deps"

codesign --verify --deep --strict "$app_root"

extract_dir="$work_dir/dotnet-bundle-extract"
mkdir -p "$extract_dir"
DOTNET_BUNDLE_EXTRACT_BASE_DIR="$extract_dir" "$exe" --selftest

extracted_deps="$work_dir/EXTRACTED_DEPENDENCIES.txt"
: > "$extracted_deps"
while IFS= read -r binary; do
    rel_binary="dotnet-bundle-extract/${binary#$extract_dir/}"
    echo "== $rel_binary" >> "$extracted_deps"
    otool -L "$binary" | sed "s|^$binary|$rel_binary|" >> "$extracted_deps"
done < <(find "$extract_dir" -type f -name '*.dylib' -print | sort)

if [[ -s "$extracted_deps" ]]; then
    if grep -E '^[[:space:]]+(/opt/homebrew|/usr/local|/Users/)' "$extracted_deps" >/dev/null; then
        echo "GUI single-file extraction dependency manifest contains user/Homebrew/local libraries:" >&2
        grep -E '^[[:space:]]+(/opt/homebrew|/usr/local|/Users/)' "$extracted_deps" >&2
        exit 1
    fi

    while IFS= read -r dep; do
        [[ "$dep" =~ ^[[:space:]] ]] || continue
        dep_path="$(awk '{print $1}' <<<"$dep")"
        case "$dep_path" in
            @rpath/*|@executable_path/*) ;;
            /usr/lib/*) ;;
            /System/Library/Frameworks/*) ;;
            *)
                echo "GUI single-file extraction links unexpected dependency: $dep_path" >&2
                exit 1
                ;;
        esac
    done < "$extracted_deps"
fi
