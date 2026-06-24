#!/usr/bin/env bash
# 编译头部追踪原生 shim(CoreMotion / CMHeadphoneMotionManager)为 dylib,放进 GUI runtimes,
# 供 P/Invoke 加载。macOS-only;产物不入 git(与 libmradm_capi.dylib 同)。
#
#   gui/build-headtrack.sh
#
# 注:真正取数据还需 .app bundle + Info.plist NSMotionUsageDescription + 用户授权(单独打包步骤)。
set -euo pipefail

here="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"

case "$(uname -s)-$(uname -m)" in
    Darwin-arm64) rid="osx-arm64"; arch="arm64" ;;
    Darwin-x86_64) rid="osx-x64"; arch="x86_64" ;;
    *) echo "头部追踪 shim 仅 macOS(CoreMotion);跳过 $(uname -s)-$(uname -m)"; exit 0 ;;
esac

src="$here/native/mr_headtrack.m"
dst_dir="$here/MacinRender.Gui/runtimes/$rid/native"
dst="$dst_dir/libmr_headtrack.dylib"

mkdir -p "$dst_dir"
clang -fobjc-arc -dynamiclib -arch "$arch" \
    -framework CoreMotion -framework Foundation \
    -install_name "@rpath/libmr_headtrack.dylib" \
    -o "$dst" "$src"
echo "已构建: $dst ($(du -h "$dst" | cut -f1))"
