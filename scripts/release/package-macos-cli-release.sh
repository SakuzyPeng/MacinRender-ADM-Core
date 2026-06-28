#!/usr/bin/env bash
set -euo pipefail

usage() {
    cat <<'EOF'
Usage: scripts/release/package-macos-cli-release.sh --binary <path> --platform macos --arch <arch> [--cmake-options <text>]

Creates dist/mradm-<version>-macos-<arch>.tar.gz and a matching .sha256 file.
Use scripts/release/package-linux-cli-appimage-release.sh for Linux release packages.
EOF
}

repo_root="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
binary=""
platform=""
arch=""
cmake_options=""

while [[ $# -gt 0 ]]; do
    case "$1" in
        --binary)
            binary="${2:-}"
            shift 2
            ;;
        --platform)
            platform="${2:-}"
            shift 2
            ;;
        --arch)
            arch="${2:-}"
            shift 2
            ;;
        --cmake-options)
            cmake_options="${2:-}"
            shift 2
            ;;
        -h|--help)
            usage
            exit 0
            ;;
        *)
            echo "unknown argument: $1" >&2
            usage >&2
            exit 2
            ;;
    esac
done

if [[ -z "$binary" || -z "$platform" || -z "$arch" ]]; then
    usage >&2
    exit 2
fi

if [[ "$platform" != "macos" ]]; then
    echo "unsupported platform '$platform'; expected macos. Use package-linux-cli-appimage-release.sh for Linux." >&2
    exit 2
fi

binary_path="$repo_root/$binary"
if [[ "$binary" = /* ]]; then
    binary_path="$binary"
fi
if [[ ! -x "$binary_path" ]]; then
    echo "binary is missing or not executable: $binary_path" >&2
    exit 1
fi

short_sha="$(git -C "$repo_root" rev-parse --short=12 HEAD)"
commit_sha="$(git -C "$repo_root" rev-parse HEAD)"
if [[ -n "${MRADM_VERSION:-}" ]]; then
    version="$MRADM_VERSION"
elif [[ "${GITHUB_REF_TYPE:-}" == "tag" && -n "${GITHUB_REF_NAME:-}" ]]; then
    version="$GITHUB_REF_NAME"
elif git -C "$repo_root" describe --tags --exact-match >/dev/null 2>&1; then
    version="$(git -C "$repo_root" describe --tags --exact-match)"
else
    version="0.0.0-dev.$short_sha"
fi

dist_dir="$repo_root/dist"
package_name="mradm-${version}-${platform}-${arch}"
package_root="$dist_dir/$package_name"
archive="$dist_dir/$package_name.tar.gz"
checksum="$archive.sha256"

rm -rf "$package_root" "$archive" "$checksum"
mkdir -p "$package_root/bin"
cp "$binary_path" "$package_root/bin/mradm"
cp "$repo_root/LICENSE" "$package_root/LICENSE"
cp "$repo_root/docs/THIRD_PARTY_LICENSES.md" "$package_root/THIRD_PARTY_NOTICES.md"
cp -R "$repo_root/third_party/licenses" "$package_root/licenses"
cp "$repo_root/third_party/sbom.cyclonedx.json" "$package_root/sbom.cyclonedx.json"

deps_file="$package_root/DEPENDENCIES.txt"
otool -L "$package_root/bin/mradm" > "$deps_file"
if grep -E '^[[:space:]]+(/opt/homebrew|/usr/local)' "$deps_file" >/dev/null; then
    echo "macOS release binary links non-system libraries:" >&2
    grep -E '^[[:space:]]+(/opt/homebrew|/usr/local)' "$deps_file" >&2
    exit 1
fi
while IFS= read -r dep; do
    [[ "$dep" =~ ^[[:space:]] ]] || continue
    dep_path="$(awk '{print $1}' <<<"$dep")"
    case "$dep_path" in
        /usr/lib/libSystem.B.dylib|/usr/lib/libc++.1.dylib) ;;
        # 任何 Apple 系统 framework 都不随发行物再分发（macOS 平台提供），一律放行；
        # 与 package-macos-gui-release.sh 的白名单口径一致，避免每次新增系统 framework 依赖就打地鼠。
        # 真正的再分发风险（Homebrew / /usr/local）已由上方独立检查拦截。
        /System/Library/Frameworks/*) ;;
        *)
            echo "macOS release binary links unexpected dependency: $dep_path" >&2
            exit 1
            ;;
    esac
done < "$deps_file"

cat > "$package_root/BUILD_INFO.txt" <<EOF
name: MacinRender ADM Core
binary: mradm
version: $version
commit: $commit_sha
platform: $platform
arch: $arch
built_at_utc: $(date -u +"%Y-%m-%dT%H:%M:%SZ")
cmake_options: ${cmake_options:-not recorded}
dependency_policy: macOS packages may only link Apple system libraries/frameworks. Use the AppImage release script for Linux packages.
EOF

(
    cd "$dist_dir"
    tar -czf "$archive" "$package_name"
)

if command -v shasum >/dev/null 2>&1; then
    (
        cd "$dist_dir"
        shasum -a 256 "$(basename "$archive")" > "$(basename "$checksum")"
    )
else
    (
        cd "$dist_dir"
        sha256sum "$(basename "$archive")" > "$(basename "$checksum")"
    )
fi

echo "$archive"
echo "$checksum"
