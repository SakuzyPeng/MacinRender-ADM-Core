#!/usr/bin/env bash
set -euo pipefail

usage() {
    cat <<'EOF'
Usage: scripts/release/package-macos-gui-release.sh [--rid osx-arm64] [--skip-native] [--single-file]

Builds and packages the Avalonia GUI as a standalone macOS .app tarball.

Policy: release packages may depend on Apple system libraries/frameworks only.
All other native libraries must be bundled inside the package; /opt/homebrew,
/usr/local, and build-tree dynamic-library dependencies are rejected.

--single-file builds a non-AOT .NET single-file executable and embeds native
libraries for runtime self-extraction. The default remains NativeAOT.
EOF
}

repo_root="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
rid=""
skip_native=0
single_file=0

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
        --single-file)
            single_file=1
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

case "$rid" in
    osx-arm64) osx_arch="arm64" ;;
    osx-x64) osx_arch="x86_64" ;;
esac

if [[ "$(uname -s)" != "Darwin" ]]; then
    echo "macOS GUI packaging must run on macOS" >&2
    exit 2
fi

if [[ "$skip_native" -eq 0 ]]; then
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
version_tool="$repo_root/scripts/release/version_metadata.py"
product_version="$(python3 "$version_tool" --repo-root "$repo_root" --field product-version)"
c_api_version="$(python3 "$version_tool" --repo-root "$repo_root" --field c-api-version)"
version="$(python3 "$version_tool" --repo-root "$repo_root" --field package-version)"

dist_dir="$repo_root/dist"
publish_dir="$repo_root/build/gui-publish/$rid"
package_suffix="macos-${rid#osx-}"
publish_mode="native-aot"
if [[ "$single_file" -eq 1 ]]; then
    package_suffix="$package_suffix-singlefile"
    publish_mode="single-file-il"
fi
package_name="MacinRender-Gui-${version}-$package_suffix"
package_root="$dist_dir/$package_name"
app_name="MacinRender ADM.app"
app_root="$package_root/$app_name"
contents="$app_root/Contents"
macos_dir="$contents/MacOS"
resources_dir="$contents/Resources"
legal_dir="$resources_dir/Legal"
zh_hans_dir="$resources_dir/zh-Hans.lproj"
archive="$dist_dir/$package_name.tar.gz"
checksum="$archive.sha256"

rm -rf "$publish_dir" "$package_root" "$archive" "$checksum"
mkdir -p "$publish_dir" "$macos_dir" "$resources_dir" "$legal_dir" "$zh_hans_dir"

publish_args=(
    -c Release
    -r "$rid"
    --self-contained true
    -p:DebugType=none
    -p:DebugSymbols=false
    -p:Version="$product_version"
    -p:FileVersion="$product_version"
    -p:InformationalVersion="$version+$short_sha"
)
if [[ "$single_file" -eq 1 ]]; then
    publish_args+=(
        -p:PublishAot=false
        -p:PublishSingleFile=true
        -p:IncludeNativeLibrariesForSelfExtract=true
    )
else
    publish_args+=(
        -p:PublishAot=true
    )
fi
publish_args+=(
    -o "$publish_dir"
)

dotnet publish "$repo_root/gui/MacinRender.Gui/MacinRender.Gui.csproj" "${publish_args[@]}"

find "$publish_dir" -name '*.dSYM' -type d -prune -exec rm -rf {} +
find "$publish_dir" -name '*.pdb' -type f -delete

while IFS= read -r dylib; do
    archs="$(lipo -archs "$dylib" 2>/dev/null || true)"
    if [[ " $archs " == *" $osx_arch "* && "$(wc -w <<<"$archs")" -gt 1 ]]; then
        thin_dylib="$dylib.thin"
        lipo "$dylib" -thin "$osx_arch" -output "$thin_dylib"
        mv "$thin_dylib" "$dylib"
    fi
done < <(find "$publish_dir" -maxdepth 1 -name '*.dylib' -type f -print)

while IFS= read -r binary; do
    if file "$binary" | grep -q 'Mach-O'; then
        # A .NET single-file host stores its bundle index after the Mach-O image.
        # strip rewrites the image and invalidates those offsets, so only strip the
        # NativeAOT executable and standalone dylibs.
        if [[ "$single_file" -eq 1 && "$(basename "$binary")" == "MacinRender.Gui" ]]; then
            continue
        fi
        strip -Sx "$binary"
    fi
done < <(find "$publish_dir" -maxdepth 1 -type f \( -perm -111 -o -name '*.dylib' \) -print)

cp -a "$publish_dir"/. "$macos_dir/"
cp "$repo_root/gui/MacinRender.Gui/Assets/AppIcon.icns" "$resources_dir/AppIcon.icns"
cp "$repo_root/LICENSE" "$package_root/LICENSE"
cp "$repo_root/docs/THIRD_PARTY_LICENSES.md" "$package_root/THIRD_PARTY_NOTICES.md"
cp -R "$repo_root/third_party/licenses" "$package_root/licenses"
cp "$repo_root/third_party/sbom.cyclonedx.json" "$package_root/sbom.cyclonedx.json"
cp "$repo_root/LICENSE" "$legal_dir/LICENSE"
cp "$repo_root/docs/THIRD_PARTY_LICENSES.md" "$legal_dir/THIRD_PARTY_NOTICES.md"
cp -R "$repo_root/third_party/licenses" "$legal_dir/licenses"
cp "$repo_root/third_party/sbom.cyclonedx.json" "$legal_dir/sbom.cyclonedx.json"

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
  <key>CFBundleDevelopmentRegion</key>
  <string>en</string>
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
  <key>LSApplicationCategoryType</key>
  <string>public.app-category.music</string>
  <key>NSMotionUsageDescription</key>
  <string>MacinRender uses AirPods head orientation to rotate the monitored spatial-audio scene in real time (head tracking).</string>
</dict>
</plist>
EOF

cat > "$zh_hans_dir/InfoPlist.strings" <<'EOF'
"CFBundleDisplayName" = "麦渲峰 ADM";
"CFBundleName" = "麦渲峰 ADM";
"NSMotionUsageDescription" = "麦渲峰读取 AirPods 头部姿态，用于实时旋转空间音频监听的声场（头部追踪）。";
EOF

if [[ ! -x "$macos_dir/MacinRender.Gui" ]]; then
    echo "published app executable is missing: $macos_dir/MacinRender.Gui" >&2
    exit 1
fi
if [[ "$single_file" -eq 1 ]]; then
    extra_payload="$(find "$macos_dir" -mindepth 1 -maxdepth 1 ! -name 'MacinRender.Gui' -print -quit)"
    if [[ -n "$extra_payload" ]]; then
        echo "single-file publish produced extra Contents/MacOS payload files:" >&2
        find "$macos_dir" -mindepth 1 -maxdepth 1 ! -name 'MacinRender.Gui' -print >&2
        exit 1
    fi
fi

# Normalize install names for bundled dylibs. Some NuGet-provided native
# libraries carry absolute build/install IDs such as /usr/local/lib/...; those
# must not appear in a release package even if the dylib itself is bundled.
while IFS= read -r dylib; do
    install_name_tool -id "@rpath/$(basename "$dylib")" "$dylib"
done < <(find "$macos_dir" -maxdepth 1 -name '*.dylib' -type f -print)

# A .NET single-file host also has bundle data after the Mach-O image; do not
# rewrite it after publish. Its extracted dylibs already receive normalized
# install names from the project-level single-file preparation target.
if [[ "$single_file" -eq 0 ]] && ! otool -l "$macos_dir/MacinRender.Gui" | grep -F '@executable_path' >/dev/null; then
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
product_version: $product_version
c_api_version: $c_api_version
commit: $commit_sha
rid: $rid
publish_mode: $publish_mode
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
