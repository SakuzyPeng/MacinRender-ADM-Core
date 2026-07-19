# CI 设计

本文档记录麦渲峰 ADM Core 的 GitHub Actions CI 设计与当前落地状态。目标是先建立稳定的
跨平台构建与测试基线，再逐步完善发布打包、许可证 bundle 和更重的静态分析。

## 目标

- 每个 pull request 都能验证 CMake 配置、编译、单元 / fixture 测试和 CLI smoke。
- macOS 路径必须覆盖 APAC、CAF layout、AudioToolbox 和 Accelerate 相关代码。
- Linux 路径用于验证跨平台构建和非 Apple 输出格式，避免 Apple framework 偶然泄漏到公共路径。
- 质量检查与本地脚本一致，优先复用 `scripts/quality/*`。
- CI 不依赖私有音频素材；只使用测试代码运行时生成的小 fixture。

## 阶段划分

### 第一阶段：必需 CI

当前已落地在 `.github/workflows/ci.yml`。PR、push 到 `main` 和手动触发都会运行。

| Job | Runner | 内容 | 说明 |
|---|---|---|---|
| `macos-debug` | `macos-26` | `cmake --preset debug`、`cmake --build --preset debug`、`ctest --preset debug` | 主验证路径；覆盖 APAC smoke 和 CoreAudio layout |
| `linux-debug` | `ubuntu-24.04` | `cmake --preset debug`、`cmake --build --preset debug`、`ctest --preset debug` | APAC 测试会自动 skip；验证跨平台核心 |

这两个 job 均显式使用 `MR_ADM_FLAC_PROVIDER=VENDORED`、`MR_ADM_OPUS_PROVIDER=VENDORED`
和 `MR_ADM_ENABLE_IAMF=OFF` 作为 CI 默认值，减少系统包差异，并避免普通 PR 构建 AOM
`iamf-tools` bridge。系统仍需安装 CMake、Ninja、Boost headers、ccache 和平台编译工具；
`libbw64/libadm/libear/SAF/FLAC/Opus` 由 FetchContent 或 vendored provider 处理。

### 第二阶段：质量 CI

当前已落地在 `.github/workflows/quality.yml`。质量 job 初期只跑在 macOS，因为
clang-tidy 脚本已经包含 macOS SDK 参数处理，且项目当前主要开发环境是 macOS。

| Job | Runner | 触发 | 内容 |
|---|---|---|---|
| `quality` | `macos-26` | pull request | `cmake --preset debug`、`scripts/quality/check-changed.sh --base origin/main --build-dir build/debug` |
| `quality` | `macos-26` | push 到 `main` / 手动 full | `cmake --preset debug`、`scripts/quality/check-all.sh build/debug` |
| `quality` | `macos-26` | 手动 changed | `check-changed.sh` |

`check-all.sh` 会全量扫描 `include/`、`src/`、`tests/`。如果后续耗时过长，可以继续保留
PR changed / main full 的分层策略。

### 第三阶段：发布构建

当前已落地在 `.github/workflows/release.yml`。发布构建用于验证 vendored static provider 和优化配置，
不在 PR 或普通 push 中运行。

| Job | 触发 | 内容 |
|---|---|---|
| `release` | tag `v*`、手动触发 | 构建 `mradm_exe`、打包并上传 `mradm-<version>-macos-arm64.tar.gz` 与 `.sha256` |
| `release-linux-appimage` | tag `v*`、手动触发 | 构建 `mradm_exe`、打包并上传 `mradm-<version>-linux-x86_64.AppImage` 与 `.sha256` |
| `release-windows` | tag `v*`、手动触发 | 构建 `mradm_exe`、打包并上传 `mradm-<version>-windows-x64.zip` 与 `.sha256` |
| `release-gui-macos` | tag `v*`、手动触发 | 构建 GUI C ABI bundle、打包并上传 `MacinRender-Gui-<version>-macos-arm64.tar.gz` 与 `.sha256` |
| `release-gui-windows` | tag `v*`、手动触发 | 构建 GUI C ABI bundle、打包并上传 `MacinRender-Gui-<version>-windows-x64.zip` 与 `.sha256` |

release workflow 使用 `scripts/release/package-macos-cli-release.sh` 生成 macOS CLI 包，使用
`scripts/release/package-linux-cli-appimage-release.sh` 生成 Linux CLI AppImage，使用
`scripts/release/package-windows-cli-release.ps1` 生成 Windows CLI 包，并使用
`scripts/release/package-macos-gui-release.sh` / `scripts/release/package-windows-gui-release.ps1` 生成 GUI 包。
CLI 包内包含 `bin/mradm` 或 `bin/mradm.exe`，GUI 包内包含 `.app` 或 `app/MacinRender.Gui.exe`；
Windows GUI 包根目录额外包含 `MacinRender ADM.cmd` 启动器，macOS GUI 的 `.app` 内部额外包含
`Contents/Resources/Legal/` 许可副本。
所有包都包含 `LICENSE`、`THIRD_PARTY_NOTICES.md`、`BUILD_INFO.txt` 和依赖清单。macOS 包会拒绝
`/opt/homebrew` 与 `/usr/local` 动态库，并只允许 Apple 系统库/framework；Linux CLI 包采用
AppImage/standalone 形式，非核心运行时库打包进 AppImage，并拒绝缺失库、构建目录依赖和
`/usr/local` 依赖；Windows 包只允许 Windows 系统 DLL 作为外部依赖，其他 `dumpbin /dependents`
发现的 DLL 必须复制进包内 `bin/` 或 `app/`。所有平台都会在上传前解包或抽取、校验 checksum，并运行
CLI smoke 或 GUI `--selftest`。Linux AppImage 构建基线为 Ubuntu 24.04 x86_64；
Windows CLI/GUI 支持基线为 Windows Server 2025 + MSVC。签名、notarization 和完整 license bundle
留到后续阶段；tag `v*` 会自动创建或更新 GitHub Release。

### 版本门禁

- 产品版本以 `CMakeLists.txt` 的 `project(... VERSION ...)` 为唯一来源，CLI、GUI、包名、应用元数据和
  `BUILD_INFO.txt` 都由 `scripts/release/version_metadata.py` 派生。
- C ABI 版本以 `include/adm/c_api.h` 的 `ADM_API_VERSION_*` 宏为唯一来源，独立于产品版本迭代。
- 发布 tag 必须为 `v<产品版本>`；不匹配时打包脚本会失败。开发构建使用
  `<产品版本>-dev.<12 位提交 SHA>`。
- 本地可运行 `python3 scripts/release/version_metadata.py --check` 检查元数据；CI 的
  `version-metadata` job 对每个 PR 和 `main` push 执行相同检查。

### GUI 国际化门禁

- 中英文词典必须拥有相同 key，格式化参数编号也必须一致。
- XAML 中面向用户的标题、正文、按钮和提示必须使用 `DynamicResource`；产品名、声道缩写和
  ADM 维度名等语言中立文本列入明确白名单。
- 本地运行 `python3 scripts/quality/check_gui_i18n.py`；CI 的 `version-metadata` job 会执行同一检查。

### IAMF bridge 预构建

IAMF 编码依赖官方 AOM `iamf-tools` bridge，但普通 CI、质量 CI 和默认 release 都显式关闭
`MR_ADM_ENABLE_IAMF`，不会在每次提交时构建 Bazel 工具链。需要更新 bridge SDK 时，手动运行
`.github/workflows/iamf-bridge-prebuild.yml`；该 workflow 会 checkout `AOMediaCodec/iamf-tools`，
把 `tools/iamf_aom_bridge/` 注入为 `iamf/cli/mr_bridge`，构建 `libmr_iamf_aom_bridge.*`，
并上传 `mr-iamf-aom-sdk-<platform>-<arch>` artifact。macOS / Linux SDK 将动态库放在
`lib/`，Windows SDK 将 import library 放在 `lib/`、运行时 DLL 放在 `bin/`。PR 只有修改 bridge workflow 或
`tools/iamf_aom_bridge/**` 时才触发该预构建验证。

## 缓存策略

CI 使用两层缓存，不缓存 CMake build tree。

| 缓存 | 路径 | 用途 | key 依据 |
|---|---|---|---|
| FetchContent | `.fc-cache` | 缓存第三方源码 checkout，降低网络波动 | OS + `cmake/MRDependencies.cmake` + `CMakeLists.txt` + `CMakePresets.json` |
| ccache | `.ccache` | 缓存编译产物 | OS + job 类型 + commit SHA，带 OS/job restore key |
| Bazel | `.bazel-cache` | 仅 IAMF bridge prebuild 使用，缓存 AOM `iamf-tools` 构建产物 | OS + iamf-tools ref + bridge source hash |

所有 job 都 fresh configure。这样即使 CMake cache 或 FetchContent 状态变化，也不会复用旧 build tree。

## 依赖安装建议

### macOS

```bash
brew install cmake ninja boost llvm cppcheck ccache
```

CI 中应显式把 Homebrew LLVM 放入 `PATH`：

```bash
echo "$(brew --prefix llvm)/bin" >> "$GITHUB_PATH"
```

### Linux

```bash
sudo apt-get update
sudo apt-get install -y cmake ninja-build build-essential git pkg-config ccache curl file patchelf desktop-file-utils libboost-all-dev libopenblas-dev liblapacke-dev
sudo apt-get install -y libfuse2t64 || sudo apt-get install -y libfuse2
```

如果 Linux job 后续启用质量检查，再安装：

```bash
sudo apt-get install -y clang-format clang-tidy cppcheck
```

## 需要注意的边界

- APAC 编码只在 macOS 可用；Linux / Windows 上 `mr_adm_apac_smoke_tests` 会跳过。
- SAF 使用 Apple Accelerate 的路径只在 macOS 存在；Linux 走 SAF 的通用路径。
- SOFA reader 默认开启，但 NetCDF 关闭；CI 不需要下载外部 SOFA 数据集。
- Release preset 默认会让 FLAC / Opus 的 `AUTO` provider 走 vendored static；Debug 在本机可能优先系统库。CI 建议显式指定 provider，减少 runner 差异。
- `mradm` 是唯一正式 CLI 二进制名；CI 不应检查或生成 `adm` 兼容入口。

## 当前 workflow 摘要

```yaml
.github/workflows/ci.yml
  pull_request / push main / workflow_dispatch
  macOS debug + Linux debug

.github/workflows/quality.yml
  pull_request / push main / workflow_dispatch
  PR: changed quality
  main/manual full: full quality

.github/workflows/release.yml
  tag v* / workflow_dispatch
  macOS/Linux tarballs + Windows zip, all with checksums

.github/workflows/iamf-bridge-prebuild.yml
  workflow_dispatch / bridge-related pull_request
  macOS/Linux/Windows prebuilt AOM IAMF bridge SDK artifact
```

## 后续实施顺序

1. 观察第一轮 GitHub runner 上 FetchContent、SAF、vendored FLAC/Opus 是否稳定。
2. 如果 quality 太慢，保留 PR changed，必要时把 main full 改成夜间 schedule。
3. release job 后续补 macOS 签名/notarization、GitHub Release 自动创建和完整第三方 license bundle。
4. Windows release 目前先关闭测试，以 `MinSizeRel` 构建 `mradm_exe` 并运行 `mradm --version` /
   `mradm backends`；后续再逐步打开 smoke tests 和实际渲染 fixture。当前使用 `MinSizeRel` 是为了绕开
   MSVC 14.44 在 SAF `saf_utility_filters.c` 上的 `/O2` 内部编译器错误；后续需要评估是否改为单文件降优化
   或 pin 工具链版本。`.github/workflows/windows-bringup.yml` 保留为手动探针，用于在不触发完整 release
   的情况下验证 Windows 构建边界。
