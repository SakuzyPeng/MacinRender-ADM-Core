#!/usr/bin/env bash
# 把 GUI 打成 macOS .app bundle(ad-hoc 签名),供本地运行/测试。
#
# 为什么要打包:CMHeadphoneMotionManager(AirPods 头部追踪)要拿数据,必须有 Info.plist 的
# NSMotionUsageDescription + 代码签名(裸 exe 下 TCC 直接杀进程)。打包后该开关才真正可用。
#
# 前置:native dylib 已就位 —
#   cmake --preset release -DMR_ADM_BUILD_CAPI_BUNDLE=ON && cmake --build --preset release --target mradm_capi_bundle
#   gui/copy-native.sh && gui/build-headtrack.sh
# 用法:gui/package-macos-gui-dev-app.sh   产物:gui/dist/MacinRender.app(不入 git)
set -euo pipefail

here="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
repo="$(cd "$here/.." && pwd)"
proj="$here/MacinRender.Gui"
rid="osx-arm64"
app_name="MacinRender"
bundle_id="com.macinrender.gui"
version_tool="$repo/scripts/release/version_metadata.py"
version="$(python3 "$version_tool" --repo-root "$repo" --field product-version)"
short_sha="$(git -C "$repo" rev-parse --short=12 HEAD)"

# 1. AOT 自包含发布
dotnet publish -c Release -r "$rid" "$proj/MacinRender.Gui.csproj" \
    -p:Version="$version" -p:FileVersion="$version" -p:InformationalVersion="$version-dev.$short_sha"
publish="$proj/bin/Release/net10.0/$rid/publish"
if [[ ! -x "$publish/MacinRender.Gui" ]]; then
    echo "找不到发布产物 $publish/MacinRender.Gui"; exit 1
fi

# 2. 组装 bundle 结构
app="$here/dist/$app_name.app"
rm -rf "$app"
macos="$app/Contents/MacOS"
res="$app/Contents/Resources"
legal="$res/Legal"
zh_hans="$res/zh-Hans.lproj"
mkdir -p "$macos" "$res" "$legal" "$zh_hans"

cp "$publish/MacinRender.Gui" "$macos/$app_name"          # 主二进制(AOT)
cp "$publish/"*.dylib "$macos/"                            # 全部 native 依赖(含 capi / headtrack)
[[ -f "$proj/Assets/AppIcon.icns" ]] && cp "$proj/Assets/AppIcon.icns" "$res/AppIcon.icns"
cp "$repo/LICENSE" "$legal/LICENSE"
cp "$repo/docs/THIRD_PARTY_LICENSES.md" "$legal/THIRD_PARTY_NOTICES.md"
cp -R "$repo/third_party/licenses" "$legal/licenses"
cp "$repo/third_party/sbom.cyclonedx.json" "$legal/sbom.cyclonedx.json"

# 3. Info.plist(含 NSMotionUsageDescription —— AirPods 头追踪的硬前提)
cat > "$app/Contents/Info.plist" <<PLIST
<?xml version="1.0" encoding="UTF-8"?>
<!DOCTYPE plist PUBLIC "-//Apple//DTD PLIST 1.0//EN" "http://www.apple.com/DTDs/PropertyList-1.0.dtd">
<plist version="1.0">
<dict>
    <key>CFBundleExecutable</key><string>$app_name</string>
    <key>CFBundleIdentifier</key><string>$bundle_id</string>
    <key>CFBundleDevelopmentRegion</key><string>en</string>
    <key>CFBundleName</key><string>MacinRender ADM</string>
    <key>CFBundleDisplayName</key><string>MacinRender ADM</string>
    <key>CFBundlePackageType</key><string>APPL</string>
    <key>CFBundleInfoDictionaryVersion</key><string>6.0</string>
    <key>CFBundleShortVersionString</key><string>$version</string>
    <key>CFBundleVersion</key><string>$version</string>
    <key>CFBundleIconFile</key><string>AppIcon</string>
    <key>LSMinimumSystemVersion</key><string>14.0</string>
    <key>NSHighResolutionCapable</key><true/>
    <key>LSApplicationCategoryType</key><string>public.app-category.music</string>
    <key>NSMotionUsageDescription</key><string>MacinRender uses AirPods head orientation to rotate the monitored spatial-audio scene in real time (head tracking).</string>
</dict>
</plist>
PLIST

cat > "$zh_hans/InfoPlist.strings" <<'STRINGS'
"CFBundleDisplayName" = "麦渲峰 ADM";
"CFBundleName" = "麦渲峰 ADM";
"NSMotionUsageDescription" = "麦渲峰读取 AirPods 头部姿态，用于实时旋转空间音频监听的声场（头部追踪）。";
STRINGS

# 4. ad-hoc 签名(本地测试足够触发 TCC 授权提示;分发需 Developer ID)。先签内部 dylib 再签 bundle。
codesign --force --sign - "$macos/"*.dylib
codesign --force --sign - "$macos/$app_name"
codesign --force --sign - "$app"

echo "已打包: $app"
echo "运行: open \"$app\"  (或 \"$macos/$app_name\")"
