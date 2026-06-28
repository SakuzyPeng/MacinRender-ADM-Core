#!/usr/bin/env bash
set -euo pipefail

usage() {
    cat <<'EOF'
Usage: scripts/release/package-linux-cli-appimage-release.sh --binary <path> --arch x86_64 [--linuxdeploy <path>]

Creates dist/mradm-<version>-linux-<arch>.AppImage and a matching .sha256 file.

Policy: Linux CLI releases are AppImage/standalone packages. Non-core runtime
libraries must be bundled inside the AppImage; unresolved, /usr/local, and
build-tree dependencies are rejected.
EOF
}

repo_root="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
binary=""
arch=""
linuxdeploy=""

while [[ $# -gt 0 ]]; do
    case "$1" in
        --binary)
            binary="${2:-}"
            shift 2
            ;;
        --arch)
            arch="${2:-}"
            shift 2
            ;;
        --linuxdeploy)
            linuxdeploy="${2:-}"
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

if [[ -z "$binary" || -z "$arch" ]]; then
    usage >&2
    exit 2
fi

case "$arch" in
    x86_64) linuxdeploy_arch="x86_64" ;;
    *)
        echo "unsupported AppImage arch '$arch' (currently x86_64 only)" >&2
        exit 2
        ;;
esac

if [[ "$(uname -s)" != "Linux" ]]; then
    echo "Linux AppImage packaging must run on Linux" >&2
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
work_dir="$repo_root/build/linux-appimage/$arch"
tools_dir="$repo_root/build/release-tools"
appdir="$work_dir/MacinRenderADM.AppDir"
package_name="mradm-${version}-linux-${arch}"
appimage="$dist_dir/$package_name.AppImage"
checksum="$appimage.sha256"
deps_file="$appdir/usr/share/doc/macinrender-adm-core/DEPENDENCIES.txt"
raw_deps_file="$work_dir/DEPENDENCIES.raw.txt"

rm -rf "$work_dir" "$appimage" "$checksum"
mkdir -p "$dist_dir" "$tools_dir" "$appdir/usr/share/doc/macinrender-adm-core"

if [[ -z "$linuxdeploy" ]]; then
    linuxdeploy="$tools_dir/linuxdeploy-${linuxdeploy_arch}.AppImage"
    if [[ ! -x "$linuxdeploy" ]]; then
        curl --fail --location --show-error \
            "https://github.com/linuxdeploy/linuxdeploy/releases/download/continuous/linuxdeploy-${linuxdeploy_arch}.AppImage" \
            --output "$linuxdeploy"
        chmod +x "$linuxdeploy"
    fi
fi

plugin="$tools_dir/linuxdeploy-plugin-appimage-${linuxdeploy_arch}.AppImage"
if [[ ! -x "$plugin" ]]; then
    curl --fail --location --show-error \
        "https://github.com/linuxdeploy/linuxdeploy-plugin-appimage/releases/download/continuous/linuxdeploy-plugin-appimage-${linuxdeploy_arch}.AppImage" \
        --output "$plugin"
    chmod +x "$plugin"
fi

desktop_file="$work_dir/mradm.desktop"
cat > "$desktop_file" <<EOF
[Desktop Entry]
Type=Application
Name=MacinRender ADM CLI
Exec=mradm
Icon=mradm
Terminal=true
Categories=AudioVideo;Audio;
EOF

icon_file="$repo_root/gui/MacinRender.Gui/Assets/AppIcon.crossplatform.svg"
if [[ ! -f "$icon_file" ]]; then
    echo "AppImage icon is missing: $icon_file" >&2
    exit 1
fi
appimage_icon="$work_dir/mradm.svg"
cp "$icon_file" "$appimage_icon"

cp "$repo_root/LICENSE" "$appdir/usr/share/doc/macinrender-adm-core/LICENSE"
cp "$repo_root/docs/THIRD_PARTY_LICENSES.md" "$appdir/usr/share/doc/macinrender-adm-core/THIRD_PARTY_NOTICES.md"
cp -R "$repo_root/third_party/licenses" "$appdir/usr/share/doc/macinrender-adm-core/licenses"
cp "$repo_root/third_party/sbom.cyclonedx.json" "$appdir/usr/share/doc/macinrender-adm-core/sbom.cyclonedx.json"

export APPIMAGE_EXTRACT_AND_RUN=1
"$linuxdeploy" \
    --appdir "$appdir" \
    --executable "$binary_path" \
    --desktop-file "$desktop_file" \
    --icon-file "$appimage_icon"

if [[ ! -x "$appdir/usr/bin/mradm" ]]; then
    echo "linuxdeploy did not create AppDir executable: $appdir/usr/bin/mradm" >&2
    exit 1
fi

LD_LIBRARY_PATH="$appdir/usr/lib${LD_LIBRARY_PATH:+:$LD_LIBRARY_PATH}" ldd "$appdir/usr/bin/mradm" > "$raw_deps_file"
sed "s|$appdir|<AppDir>|g" "$raw_deps_file" > "$deps_file"
{
    echo
    echo "Bundled shared libraries:"
    if [[ -d "$appdir/usr/lib" ]]; then
        find "$appdir/usr/lib" -maxdepth 2 -type f -name '*.so*' -printf '  %P\n' | sort
    fi
} >> "$deps_file"

if grep -F 'not found' "$raw_deps_file" >/dev/null; then
    echo "Linux AppImage has unresolved shared libraries:" >&2
    grep -F 'not found' "$raw_deps_file" >&2
    exit 1
fi

if grep -F "$repo_root/" "$raw_deps_file" | grep -Fv "$appdir/" >/dev/null; then
    echo "Linux AppImage links a build-tree dependency:" >&2
    grep -F "$repo_root/" "$raw_deps_file" | grep -Fv "$appdir/" >&2
    exit 1
fi

if grep -E '=>[[:space:]]+/usr/local/' "$raw_deps_file" >/dev/null; then
    echo "Linux AppImage links /usr/local, which is outside the release boundary:" >&2
    grep -E '=>[[:space:]]+/usr/local/' "$raw_deps_file" >&2
    exit 1
fi

cat > "$appdir/usr/share/doc/macinrender-adm-core/BUILD_INFO.txt" <<EOF
name: MacinRender ADM Core
binary: mradm
version: $version
commit: $commit_sha
platform: linux-appimage
arch: $arch
built_at_utc: $(date -u +"%Y-%m-%dT%H:%M:%SZ")
dependency_policy: Linux CLI releases are AppImage/standalone packages. Non-core runtime libraries are bundled in the AppImage; unresolved, /usr/local, and build-tree shared-library dependencies are rejected. The host kernel, dynamic loader, glibc baseline, and other core platform facilities remain host-provided.
EOF

(
    cd "$dist_dir"
    OUTPUT="$appimage" "$linuxdeploy" --appdir "$appdir" --output appimage
)

if [[ ! -x "$appimage" ]]; then
    echo "AppImage was not produced: $appimage" >&2
    exit 1
fi

(
    cd "$dist_dir"
    sha256sum "$(basename "$appimage")" > "$(basename "$checksum")"
)

echo "$appimage"
echo "$checksum"
