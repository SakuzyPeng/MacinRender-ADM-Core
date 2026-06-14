#!/usr/bin/env bash
set -euo pipefail

usage() {
    cat <<'EOF'
Usage: scripts/release/package-gui.sh [--rid osx-arm64] [--skip-native]

Builds and packages the Avalonia GUI as a standalone macOS .app tarball.

Policy: release packages may depend on Apple system libraries/frameworks only.
All other native libraries must be bundled inside the package; /opt/homebrew,
/usr/local, and build-tree dynamic-library dependencies are rejected.
EOF
}

repo_root="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
rid=""
skip_native=0

while [[ $# -gt 0 ]]; do
    case "$1" in
        --rid)
            rid="${2:-}"
            shift 2
            ;;
        --skip-native)
            skip_native=1
            shift
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

if [[ -z "$rid" ]]; then
    case "$(uname -s)-$(uname -m)" in
        Darwin-arm64) rid="osx-arm64" ;;
        Darwin-x86_64) rid="osx-x64" ;;
        *)
            echo "cannot infer RID for $(uname -s)-$(uname -m); pass --rid" >&2
            exit 2
            ;;
    esac
fi

case "$rid" in
    osx-arm64|osx-x64) ;;
    *)
        echo "unsupported GUI package RID '$rid' (current script supports macOS only)" >&2
        exit 2
        ;;
esac

if [[ "$(uname -s)" != "Darwin" ]]; then
    echo "macOS GUI packaging must run on macOS" >&2
    exit 2
fi

if [[ "$skip_native" -eq 0 ]]; then
    case "$rid" in
        osx-arm64) osx_arch="arm64" ;;
        osx-x64) osx_arch="x86_64" ;;
    esac

    native_build_dir="$repo_root/build/gui-native-package/$rid"
    cmake_args=(
        -S "$repo_root"
        -B "$native_build_dir"
        -G Ninja
        -DCMAKE_BUILD_TYPE=Release
        -DCMAKE_EXPORT_COMPILE_COMMANDS=ON
        -DCMAKE_OSX_ARCHITECTURES="$osx_arch"
        -DMR_ADM_BUILD_CAPI_BUNDLE=ON
        -DMR_ADM_CORE_BUILD_CLI=OFF
        -DMR_ADM_CORE_BUILD_TESTS=OFF
        -DMR_ADM_CORE_USE_INSTALLED_DEPS=OFF
        -DMR_ADM_FLAC_PROVIDER=VENDORED
        -DMR_ADM_OPUS_PROVIDER=VENDORED
    )
    if [[ -n "${FC_CACHE_DIR:-}" ]]; then
        mkdir -p "$FC_CACHE_DIR"
        cmake_args+=(-DFETCHCONTENT_BASE_DIR="$FC_CACHE_DIR")
    fi
    cmake "${cmake_args[@]}"
    cmake --build "$native_build_dir" --target mradm_capi_bundle

    native_lib="$native_build_dir/libmradm_capi.dylib"
    native_dst="$repo_root/gui/MacinRender.Gui/runtimes/$rid/native/libmradm_capi.dylib"
    if [[ ! -e "$native_lib" ]]; then
        echo "native C ABI bundle is missing: $native_lib" >&2
        exit 1
    fi
    mkdir -p "$(dirname "$native_dst")"
    cp -Lf "$native_lib" "$native_dst"
    echo "copied native bundle: $native_dst ($(du -h "$native_dst" | cut -f1))"
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
publish_dir="$repo_root/build/gui-publish/$rid"
package_name="MacinRender-Gui-${version}-macos-${rid#osx-}"
package_root="$dist_dir/$package_name"
app_name="MacinRender ADM.app"
app_root="$package_root/$app_name"
contents="$app_root/Contents"
macos_dir="$contents/MacOS"
resources_dir="$contents/Resources"
archive="$dist_dir/$package_name.tar.gz"
checksum="$archive.sha256"

rm -rf "$publish_dir" "$package_root" "$archive" "$checksum"
mkdir -p "$publish_dir" "$macos_dir" "$resources_dir"

dotnet publish "$repo_root/gui/MacinRender.Gui/MacinRender.Gui.csproj" \
    -c Release \
    -r "$rid" \
    --self-contained true \
    -p:PublishAot=true \
    -o "$publish_dir"

cp -a "$publish_dir"/. "$macos_dir/"
cp "$repo_root/gui/MacinRender.Gui/Assets/AppIcon.icns" "$resources_dir/AppIcon.icns"
cp "$repo_root/LICENSE" "$package_root/LICENSE"
cp "$repo_root/docs/THIRD_PARTY_LICENSES.md" "$package_root/THIRD_PARTY_NOTICES.md"

cat > "$contents/Info.plist" <<EOF
<?xml version="1.0" encoding="UTF-8"?>
<!DOCTYPE plist PUBLIC "-//Apple//DTD PLIST 1.0//EN" "http://www.apple.com/DTDs/PropertyList-1.0.dtd">
<plist version="1.0">
<dict>
  <key>CFBundleExecutable</key>
  <string>MacinRender.Gui</string>
  <key>CFBundleIconFile</key>
  <string>AppIcon</string>
  <key>CFBundleIdentifier</key>
  <string>com.macinrender.adm.gui</string>
  <key>CFBundleName</key>
  <string>MacinRender ADM</string>
  <key>CFBundleDisplayName</key>
  <string>MacinRender ADM</string>
  <key>CFBundlePackageType</key>
  <string>APPL</string>
  <key>CFBundleShortVersionString</key>
  <string>$version</string>
  <key>CFBundleVersion</key>
  <string>$short_sha</string>
  <key>LSMinimumSystemVersion</key>
  <string>13.0</string>
  <key>NSHighResolutionCapable</key>
  <true/>
</dict>
</plist>
EOF

if [[ ! -x "$macos_dir/MacinRender.Gui" ]]; then
    echo "published app executable is missing: $macos_dir/MacinRender.Gui" >&2
    exit 1
fi

# Normalize install names for bundled dylibs. Some NuGet-provided native
# libraries carry absolute build/install IDs such as /usr/local/lib/...; those
# must not appear in a release package even if the dylib itself is bundled.
while IFS= read -r dylib; do
    install_name_tool -id "@rpath/$(basename "$dylib")" "$dylib"
done < <(find "$macos_dir" -maxdepth 1 -name '*.dylib' -type f -print)

# Give the executable a deterministic local rpath for any @rpath references.
if ! otool -l "$macos_dir/MacinRender.Gui" | grep -F '@executable_path' >/dev/null; then
    install_name_tool -add_rpath "@executable_path" "$macos_dir/MacinRender.Gui" || true
fi

deps_file="$package_root/DEPENDENCIES.txt"
: > "$deps_file"
while IFS= read -r binary; do
    rel_binary="${binary#$package_root/}"
    echo "== $rel_binary" >> "$deps_file"
    otool -L "$binary" | sed "s|^$binary|$rel_binary|" >> "$deps_file"
done < <(find "$macos_dir" -maxdepth 1 \( -name 'MacinRender.Gui' -o -name '*.dylib' \) -type f -print | sort)

if grep -E '^[[:space:]]+(/opt/homebrew|/usr/local|/Users/|'"$repo_root"')' "$deps_file" >/dev/null; then
    echo "GUI package links user, Homebrew, /usr/local, or build-tree libraries:" >&2
    grep -E '^[[:space:]]+(/opt/homebrew|/usr/local|/Users/|'"$repo_root"')' "$deps_file" >&2
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
done < "$deps_file"

cat > "$package_root/BUILD_INFO.txt" <<EOF
name: MacinRender ADM GUI
binary: MacinRender ADM.app
version: $version
commit: $commit_sha
rid: $rid
built_at_utc: $(date -u +"%Y-%m-%dT%H:%M:%SZ")
dependency_policy: package may depend on Apple system libraries/frameworks only; all other native libraries must be bundled inside the .app package. /opt/homebrew, /usr/local, user, and build-tree dynamic-library dependencies are rejected.
EOF

codesign --force --sign - --timestamp=none --deep "$app_root" >/dev/null
codesign --verify --deep --strict "$app_root"

(
    cd "$dist_dir"
    tar -czf "$archive" "$package_name"
)

(
    cd "$dist_dir"
    shasum -a 256 "$(basename "$archive")" > "$(basename "$checksum")"
)

echo "$archive"
echo "$checksum"
