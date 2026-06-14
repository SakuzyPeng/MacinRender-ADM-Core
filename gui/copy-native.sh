#!/usr/bin/env bash
# 把已构建的自包含 C ABI dylib 拷到 GUI 工程的 runtimes 目录,供 P/Invoke 加载。
#
# 前置:先在仓库根构建 bundle 目标 —
#   cmake --preset release -DMR_ADM_BUILD_CAPI_BUNDLE=ON
#   cmake --build --preset release --target mradm_capi_bundle
#
# 一期手动跑;Windows/Linux 产物与 CI 化留后。
set -euo pipefail

here="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
repo_root="$(cd "$here/.." && pwd)"

# 平台 → RID + 库名(一期只覆盖 macOS arm64)
case "$(uname -s)-$(uname -m)" in
    Darwin-arm64) rid="osx-arm64";  lib="libmradm_capi.dylib" ;;
    Darwin-x86_64) rid="osx-x64";   lib="libmradm_capi.dylib" ;;
    *) echo "暂未支持的平台: $(uname -s)-$(uname -m)(一期仅 macOS)"; exit 1 ;;
esac

src_dir="$repo_root/build/release"
dst_dir="$here/MacinRender.Gui/runtimes/$rid/native"

# dylib 是带版本号实体 + symlink;cp -L 跟随 symlink 拷成实体文件,避免依赖 symlink 链。
if [[ ! -e "$src_dir/$lib" ]]; then
    echo "找不到 $src_dir/$lib —— 请先构建 mradm_capi_bundle 目标"; exit 1
fi

mkdir -p "$dst_dir"
cp -Lf "$src_dir/$lib" "$dst_dir/$lib"
echo "已拷贝: $dst_dir/$lib ($(du -h "$dst_dir/$lib" | cut -f1))"
