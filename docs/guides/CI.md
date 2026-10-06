# CI 设计

本文档记录麦渲峰 ADM Core 的 GitHub Actions CI 设计与当前落地状态。目标是先建立稳定的
跨平台构建与测试基线，再逐步完善发布打包、许可证 bundle 和更重的静态分析。

## 目标

- 每个 pull request 都能验证 CMake 配置、编译、单元 / fixture 测试和 CLI smoke。
- macOS 路径必须覆盖 APAC、CAF layout、AudioToolbox / AVFoundation 相关代码。
- Linux 路径用于验证跨平台构建和非 Apple 输出格式，避免 Apple framework 偶然泄漏到公共路径。
- Windows 路径验证 MSVC 构建、Windows-only 系统空间监听 sink 和 PowerShell 打包脚本。
- C++ 与 Rust（`rust/` Cargo workspace）在同一次 CMake 构建中验证；Rust 单元测试经 ctest 运行。
- 质量检查与本地脚本一致，优先复用 `scripts/quality/*`。
- CI 不依赖私有音频素材；只使用测试代码运行时生成的小 fixture。

## 阶段划分

### 第一阶段：必需 CI

当前已落地在 `.github/workflows/ci.yml`。PR、push 到 `main` 和手动触发都会运行。

| Job | Runner | 内容 | 说明 |
|---|---|---|---|
| `version-metadata` | `ubuntu-24.04` | `version_metadata.py --check`、`check_gui_i18n.py` | 见下文版本与 GUI 国际化门禁 |
| `debug`（macOS debug） | `macos-26` | `cmake --preset debug`、`cmake --build --preset debug`、`ctest --preset debug` | 主验证路径；覆盖 APAC smoke、CoreAudio layout 和 Apple 后端 |
| `debug`（Linux debug） | `ubuntu-24.04` | 同上 | Apple-only 测试自动 skip；验证跨平台核心 |
| `windows-debug` | `windows-2025-vs2026` | PowerShell 脚本语法检查；MSVC + Ninja Debug 构建；`ctest` | vcpkg 安装 libear 所需 Boost 头文件；显式开启 `MR_ADM_ENABLE_SOFA` 以覆盖纯 Rust SOFA reader；测试在 vcvars 环境内运行（Debug CRT） |

三个构建 job 都先用 `rustup` 安装固定的 Rust 1.98.0（含 rustfmt / clippy），并显式使用
`MR_ADM_FLAC_PROVIDER=VENDORED`、`MR_ADM_OPUS_PROVIDER=VENDORED`、`MR_ADM_ENABLE_IAMF=OFF`
和 `MR_ADM_BUILD_CAPI_BUNDLE=ON`：减少系统包差异，避免普通 PR 构建 AOM `iamf-tools` bridge，
同时验证 GUI 使用的自包含 C ABI bundle 可以链接。系统仍需安装 CMake、Ninja、Boost headers、
ccache（Windows 用 sccache）和平台编译工具。C/C++ 第三方依赖（libear、FLAC、Opus、miniaudio 等）
由 FetchContent 或 vendored provider 处理；Rust 依赖由 Cargo 按 `rust/Cargo.lock`（`--locked`）获取。

生产构建不再需要 SAF、OpenBLAS/LAPACKE、libmysofa、libebur128、libsamplerate、libadm 或 libbw64；
它们只在默认关闭的 `MR_ADM_BUILD_*_REFERENCE_TESTS` 开关下作为维护对照获取，CI 不开启这些开关。

### 第二阶段：质量 CI

当前已落地在 `.github/workflows/quality.yml`。质量 job 只跑在 macOS，因为
clang-tidy 脚本已经包含 macOS SDK 参数处理，且项目当前主要开发环境是 macOS。

| Job | Runner | 触发 | 内容 |
|---|---|---|---|
| `quality` | `macos-26` | 所有触发 | `cmake --preset debug`、`cmake --build build/debug --target mr_adm_rust_quality`（`cargo fmt --check` + `cargo clippy -D warnings`） |
| `quality` | `macos-26` | pull request / 手动 changed | `scripts/quality/check-changed.sh --base origin/main --build-dir build/debug` |
| `quality` | `macos-26` | push 到 `main` / 手动 full | `scripts/quality/check-all.sh build/debug` |

`check-changed.sh` / `check-all.sh` 只扫描 `include/`、`src/`、`tests/` 下的 C/C++，Rust 由
`mr_adm_rust_quality` 覆盖。如果后续耗时过长，可以继续保留 PR changed / main full 的分层策略。

许可证与 SBOM 校验（`scripts/quality/check-licenses.sh`，包含 Cargo 依赖校验）目前在 release
workflow 中以 `--require-full` 运行；本地改动依赖后也应手动执行。

### 一致性记录

`.github/workflows/consistency.yml` 在 push 到 `main` 和手动触发时运行，不在 PR 上运行。

| Job | Runner | 内容 |
|---|---|---|
| `render` | `macos-26`、`ubuntu-24.04` × config A/B | 用 `consistency-a` / `consistency-b` Release preset 构建 CLI 与工具，运行数值工具测试和 `scripts/consistency/render-matrix.sh`，上传 PCM 与构建记录 |
| `render-windows` | `windows-2025-vs2026` × config A/B | Windows 对应构建与渲染矩阵 |
| `compare` | `ubuntu-24.04` | 汇总三平台结果，`scripts/consistency/compare-platforms.sh` 比较并上传报告 |

config A 为默认数值配置，config B 打开 `MR_ADM_STRICT_FP` / `MR_ADM_EAR_SCALAR_REFERENCE` 等受控
数值选项。Rust 一期**不设跨平台逐位一致门禁**：使用的清单
`scripts/consistency/expected-identical-rust-phase1.txt` 为空，只记录差异；输入完整性和同进程
重复性仍是硬门禁。跨平台逐位一致属二期目标（见 `docs/architecture/RUST_SAF_REPLACEMENT_ROADMAP.md`）。
历史 SAF 时期的门禁清单和定位脚本（`expected-identical.txt`、`expected-identical-controlled.txt`、`run-localization.py` 等）保留为
原始证据，不适用于当前 Rust 构建。

### 第三阶段：发布构建

当前已落地在 `.github/workflows/release.yml`。发布构建用于验证 vendored static provider 和优化配置，
不在 PR 或普通 push 中运行。

| Job | 触发 | 内容 |
|---|---|---|
| `release` | tag `v*`、手动触发 | 构建 `mradm_exe` + `mradm_capi_bundle`、打包并上传 `mradm-<version>-macos-arm64.tar.gz` 与 `.sha256` |
| `release-linux-appimage` | 手动触发 | 构建 `mradm_exe`、打包并上传 `mradm-<version>-linux-x86_64.AppImage` 与 `.sha256` |
| `release-windows` | tag `v*`、手动触发 | 构建 `mradm_exe`、打包并上传 `mradm-<version>-windows-x64.zip` 与 `.sha256` |
| `release-gui-macos` | tag `v*`、手动触发 | 构建 GUI C ABI bundle、打包并上传 `MacinRender-Gui-<version>-macos-arm64.tar.gz` 与 `.sha256` |
| `release-gui-windows` | tag `v*`、手动触发 | 构建 GUI C ABI bundle、打包并上传 `MacinRender-Gui-<version>-windows-x64.zip` 与 `.sha256` |
| `publish-github-release` | tag `v*` | 汇总各平台产物，创建或更新 GitHub Release |

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
CLI smoke 或 GUI `--selftest`。macOS / Linux release 构建使用 `MR_ADM_CORE_USE_INSTALLED_DEPS=OFF`，
避免 runner 上的系统包混入发行产物；打包前运行 `check-licenses.sh --require-full`。Linux AppImage 构建基线为 Ubuntu 24.04 x86_64；
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
| FetchContent | `.fc-cache` | 缓存第三方源码 checkout（含 Corrosion），降低网络波动 | OS + `cmake/MRDependencies.cmake` + `CMakeLists.txt` + `CMakePresets.json` |
| ccache | `.ccache` | macOS / Linux 缓存 C/C++ 编译产物 | OS + job 类型 + commit SHA，带 OS/job restore key |
| sccache | `.sccache` | Windows 缓存 MSVC 编译产物 | commit SHA，带 restore key |
| vcpkg | `.vcpkg-bincache` | Windows 缓存 Boost 二进制包 | workflow 文件 hash |
| Bazel | `.bazel-cache` | 仅 IAMF bridge prebuild 使用，缓存 AOM `iamf-tools` 构建产物 | OS + iamf-tools ref + bridge source hash |

所有 job 都 fresh configure。这样即使 CMake cache 或 FetchContent 状态变化，也不会复用旧 build tree。
Cargo registry 和 Rust 构建产物（`build/rust/`）目前**不缓存**：每次运行按 `Cargo.lock` 从 crates.io
下载并重新编译 Rust 依赖。若 Rust 部分成为瓶颈，可再为 `~/.cargo/registry` 与 `build/rust/` 增加缓存。

`.github/workflows/cache-maintenance.yml` 每周一 03:23 UTC（也可手动）运行，按 key 族只保留最近
访问的一代缓存，删除旧代以控制仓库缓存配额。

## 依赖安装建议

所有平台都需要 Rust 1.98.0（含 rustfmt / clippy）：

```bash
rustup toolchain install 1.98.0 --profile minimal --component rustfmt --component clippy
```

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
sudo apt-get install -y cmake ninja-build build-essential git pkg-config ccache curl file patchelf desktop-file-utils libboost-all-dev
sudo apt-get install -y libfuse2t64 || sudo apt-get install -y libfuse2
```

`curl`、`file`、`patchelf`、`desktop-file-utils` 和 libfuse 只在 AppImage 打包时需要；普通 debug job
只安装 `cmake ninja-build build-essential git pkg-config ccache libboost-all-dev`。

如果 Linux job 后续启用质量检查，再安装：

```bash
sudo apt-get install -y clang-format clang-tidy cppcheck
```

### Windows

Visual Studio（含 VC x64 工具）、Ninja、vcpkg，以及 Boost 头文件：

```powershell
vcpkg install boost-format boost-functional boost-algorithm boost-integer boost-iterator `
  boost-math boost-optional boost-range boost-rational boost-smart-ptr boost-variant --triplet x64-windows
```

在 `vcvars64.bat` 环境中配置，并传入 vcpkg toolchain file。维护者本机验证使用规范构建树
`build\win-canon`（见 `AGENTS.md`），不需要 OpenBLAS。

## 需要注意的边界

- APAC 编码只在 macOS 可用；Linux / Windows 上 `mr_adm_apac_smoke_tests` 会跳过。
- Apple 后端（`--renderer apple`）与 ASBR 系统空间监听只在 macOS 编译；Windows 系统空间监听 sink 只在 Windows 编译。
- SOFA 由纯 Rust `sofar`（`rust/vendor/sofar` 本地补丁）解析，默认开启；CI 使用 `tests/fixtures/sofa/` 下的小型 fixture，不下载外部 SOFA 数据集。
- RustFFT 使用默认 SIMD 分派，跨平台 PCM 位差属预期；不要把一致性 workflow 的差异记录当作 CI 失败处理。
- Rust 私有符号不得从 C ABI bundle 导出；bundle 只导出 `adm_*`。
- Release preset 默认会让 FLAC / Opus 的 `AUTO` provider 走 vendored static；Debug 在本机可能优先系统库。CI 建议显式指定 provider，减少 runner 差异。
- `mradm` 是唯一正式 CLI 二进制名；CI 不应检查或生成 `adm` 兼容入口。

## 当前 workflow 摘要

```yaml
.github/workflows/ci.yml
  pull_request / push main / workflow_dispatch
  version metadata + macOS debug + Linux debug + Windows debug

.github/workflows/quality.yml
  pull_request / push main / workflow_dispatch
  all: Rust fmt + clippy
  PR: changed quality
  main/manual full: full quality

.github/workflows/consistency.yml
  push main / workflow_dispatch
  macOS/Linux/Windows × config A/B render + cross-platform compare (record only)

.github/workflows/release.yml
  tag v* / workflow_dispatch
  macOS tarball + Linux AppImage (manual) + Windows zip + macOS/Windows GUI, all with checksums
  tag v*: publish GitHub Release

.github/workflows/windows-bringup.yml
  workflow_dispatch
  Windows MinSizeRel probe build + CLI artifact

.github/workflows/iamf-bridge-prebuild.yml
  workflow_dispatch / bridge-related pull_request
  macOS/Linux/Windows prebuilt AOM IAMF bridge SDK artifact

.github/workflows/cache-maintenance.yml
  weekly schedule / workflow_dispatch
  prune stale cache generations
```

## 后续实施顺序

1. 评估为 Cargo registry 和 `build/rust/` 增加缓存，缩短 Rust 依赖重复编译时间。
2. 如果 quality 太慢，保留 PR changed，必要时把 main full 改成夜间 schedule。
3. 评估把 `check-licenses.sh` 加入 PR 路径，尽早发现未登记的 Cargo / FetchContent 依赖。
4. release job 后续补 macOS 签名/notarization 和完整第三方 license bundle。
5. Windows release 与 `windows-bringup.yml` 仍以 `MinSizeRel` 构建，只运行 `mradm --version`、`mradm backends`、
   `mr_adm_flac_io_smoke_tests` 和打包 smoke。当初选 `MinSizeRel` 是为了绕开 MSVC 14.44 在 SAF
   `saf_utility_filters.c` 上的 `/O2` 内部编译器错误；SAF 已不在生产构建中，可以评估改回 `Release`
   并逐步打开实际渲染 fixture。`windows-bringup.yml` 保留为手动探针，用于在不触发完整 release 的情况下
   验证 Windows 构建边界。
6. 二期跨平台逐位一致落地后，在一致性 workflow 中按 renderer / 布局 / 语义组合恢复位相等门禁。
