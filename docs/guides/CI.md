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
| `version-metadata` | `ubuntu-24.04` | `version_metadata.py --check`、`check_gui_i18n.py`、`check-reference-retention.py` | 见下文版本与 GUI 国际化门禁；参考代码登记见 `docs/architecture/RUST_REFERENCE_RETENTION.md` |
| `debug`（macOS debug） | `macos-26` | `cmake --preset debug`、`cmake --build --preset debug`、`check-licenses.sh --build-dir build/debug`、`check-capi-exports.py`、`ctest --preset debug` | 主验证路径；覆盖 APAC smoke、CoreAudio layout 和 Apple 后端 |
| `debug`（Linux debug） | `ubuntu-24.04` | 同上 | Apple-only 测试自动 skip；验证跨平台核心 |
| `windows-debug` | `windows-2025-vs2026` | PowerShell 脚本语法检查；MSVC + Ninja Debug 构建；`check-capi-exports.py`；`ctest` | 生产构建无需 Boost；显式开启 `MR_ADM_ENABLE_SOFA` 以覆盖纯 Rust SOFA reader；测试在 vcvars 环境内运行（Debug CRT） |

三个构建 job 都先用 `rustup` 安装固定的 Rust 1.98.0（含 rustfmt / clippy），并显式使用
`MR_ADM_FLAC_PROVIDER=VENDORED`、`MR_ADM_OPUS_PROVIDER=VENDORED`、`MR_ADM_ENABLE_IAMF=OFF`
和 `MR_ADM_BUILD_CAPI_BUNDLE=ON`：减少系统包差异，避免普通 PR 构建 AOM `iamf-tools` bridge，
同时验证 GUI 使用的自包含 C ABI bundle 可以链接。系统仍需安装 CMake、Ninja、
ccache（Windows 用 sccache）和平台编译工具。C/C++ 第三方依赖（libear、FLAC、Opus、miniaudio 等）
由 FetchContent 或 vendored provider 处理；Rust 依赖由 Cargo 按 `rust/Cargo.lock`（`--locked`）获取。

生产构建不再需要 SAF、OpenBLAS/LAPACKE、libmysofa、libebur128、libsamplerate、libadm、libbw64 或 dr_wav（dr_libs 仍为 dr_flac 获取）；
它们只在默认关闭的 `MR_ADM_BUILD_*_REFERENCE_TESTS` 开关下作为维护对照获取，必需 CI 不开启这些开关；
手动的 `.github/workflows/reference-tests.yml` 在 Release 下逐个开启并运行对应 ctest，防止这些对照无声腐烂
（SAF 对照保持在其验收所用的 macOS + Accelerate 数值环境；其余跑在 Linux）。失败的处理见 `docs/architecture/RUST_REFERENCE_RETENTION.md`。

### 第二阶段：质量 CI

当前已落地在 `.github/workflows/quality.yml`。质量 job 只跑在 macOS，因为
clang-tidy 脚本已经包含 macOS SDK 参数处理，且项目当前主要开发环境是 macOS。

| Job | Runner | 触发 | 内容 |
|---|---|---|---|
| `quality` | `macos-26` | 所有触发 | `cmake --preset debug`、`cmake --build build/debug --target mr_adm_rust_quality`（`cargo fmt --check` + `cargo clippy -D warnings`）、`mr_adm_ffi_header_check` |
| `quality` | `macos-26` | pull request / 手动 changed | `scripts/quality/check-changed.sh --base origin/main --build-dir build/debug` |
| `quality` | `macos-26` | push 到 `main` / 手动 full | `scripts/quality/check-all.sh build/debug` |

`check-changed.sh` / `check-all.sh` 只扫描 `include/`、`src/`、`tests/` 下的 C/C++，Rust 由
`mr_adm_rust_quality` 覆盖。如果后续耗时过长，可以继续保留 PR changed / main full 的分层策略。

`mr_adm_ffi_header_check`（`scripts/quality/check-ffi-headers.py`）用固定版本的 cbindgen（0.29.2，单独缓存
`~/.cargo/bin/cbindgen`）把 `mradm-ffi` 的导出渲染成一次性 C 头，再与 C++ 侧手写的私有 FFI 头
（`src/adm_dsp/*_ffi.h`、`src/adm_metadata/adm_ffi.h`、`src/adm_audio/wav_ffi.h`）逐项比较：导出符号集合、
参数个数/顺序/指针层级/const、返回值，以及被签名引用的 `#[repr(C)]` 结构体字段类型与顺序（含数组长度）。
`void*` 句柄与 Rust 侧任意结构体指针视为兼容；字段名不比较。生成头不入库、也不被 C++ include。

C ABI 导出面（`scripts/quality/check-capi-exports.py`）在 ci 的三个构建 job、release 的三个 CLI job 和两个 GUI
打包脚本中检查 `mradm_capi` 共享库恰好导出 `include/adm/c_api.h` 声明的 `adm_*` 函数：ELF / Mach-O 用 `nm`，
PE 直接读 DLL 导出表。Windows bundle 链接由同一脚本从 `c_api.h` 生成的 `.def`，不再使用
`WINDOWS_EXPORT_ALL_SYMBOLS`。

许可证与 SBOM 校验（`scripts/quality/check-licenses.sh`）在必需 CI 的 macOS / Linux debug job 中运行：
校验 `third_party/manifest.json` 与 `cmake/MRDependencies.cmake`、`rust/Cargo.lock` 覆盖、DSP/ADM 资源
哈希、SBOM 与文档依赖表，以及本次构建实际获取的依赖许可原文是否漂移。因此新增或升级 FetchContent /
Cargo 依赖而未登记的 PR 会直接失败。release workflow 另以 `--require-full` 要求所有依赖都在构建中。

### 一致性记录

`.github/workflows/consistency.yml` 在 push 到 `main` 和手动触发时运行，不在 PR 上运行。

| Job | Runner | 内容 |
|---|---|---|
| `render` | `macos-26`、`ubuntu-24.04` × config A/B | 用 `consistency-a` / `consistency-b` Release preset 构建 CLI 与工具，运行数值工具测试、Rust 二期采集和诊断无扰动验证，上传 PCM/检查点与构建记录 |
| `render-windows` | `windows-2025-vs2026` × config A/B | Windows 对应构建与渲染矩阵 |
| `compare` | `ubuntu-24.04` | 汇总三平台结果，`scripts/consistency/compare-rust-phase2.py` 校验、比较并上传 JSON 报告 |

config A 为默认数值配置，config B 只打开 C/C++ 严格浮点选项；两组 RustFFT 均使用标量规划器
（ADR 0015，构建记录中含 SIMD 特性的结果会被比较器拒收）。
二期结项后的扩展继续使用 `run-rust-phase2.py`，当前收集 40 个离线场景、70 个实时 Scene 配置（各含两个 epoch）及
160 个内核测量（含 FFT、重采样、HRTF、外部 SOFA、退化 OM 与 spreader），再在原构建树启用诊断并验证 PCM 无扰动。`compare-rust-phase2.py`
验证三平台源码、输入和产物后输出逐用例 JSON。输入完整性、重复性、帧数和无扰动性是硬门禁；
`scripts/consistency/phase2-gates.json` 列出的内核/用例（当前为 FFT、EAR 去相关 FIR、可移植三角函数、重采样、OM/spreader、HRTF、SOFA 内核及全部 180 个 PCM 用例）必须三平台逐位相同；
新增用例必须同时按精确 id 加入门禁（`phase2_tools_test.py` 校验）。9 个内核模式展开为 150 个
文件，其余 10 个为观察项（EAR 布局、Scene、HpTF 和平台 libm f64 twiddle）；目前只有 f64
twiddle 观察列存在差异。`compare` job 先写出全部四份报告，再按门禁结果失败。
SOFA 必须开启；三份仓库内固定文件受 SHA-256 校验，覆盖解析、HRTF 准备、离线与实时输出，见
[外部 SOFA 一致性检查](../architecture/RUST_SOFA_CONSISTENCY.md)。未设置本机数据集环境变量不会跳过这些检查。
A 组还在同一构建树交替测量原标量与补丁 FFT，核对位指纹并上传性能 JSON；共享 runner 的
细小计时变化不作为硬门禁。
原 78 项矩阵的范围和维护要求见 [二期结项](../architecture/RUST_PHASE2_CLOSEOUT.md)，当前新增范围见[覆盖扩展](../architecture/RUST_COVERAGE_EXTENSION.md)。
历史 SAF 清单和定位脚本保留原始边界。

### 第三阶段：发布构建

当前已落地在 `.github/workflows/release.yml`。发布构建用于验证 vendored static provider 和优化配置，
不在 PR 或普通 push 中运行。

| Job | 触发 | 内容 |
|---|---|---|
| `release` | tag `v*`、手动触发 | 构建 `mradm_exe` + `mradm_capi_bundle`、打包并上传 `mradm-<version>-macos-arm64.tar.gz` 与 `.sha256` |
| `release-linux-appimage` | 手动触发 | 构建 `mradm_exe`、打包并上传 `mradm-<version>-linux-x86_64.AppImage` 与 `.sha256` |
| `release-windows` | tag `v*`、手动触发 | `Release` 构建 `mradm_exe`，运行优化构建下的渲染 fixture（见下），打包并上传 `mradm-<version>-windows-x64.zip` 与 `.sha256` |
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
| Cargo | `~/.cargo/registry/{index,cache}`、`~/.cargo/git/db`、`build/rust/` | 缓存 crates.io 下载与 Rust 依赖编译产物（ci / quality / consistency；单 OS 约 400 MB） | OS + 构建类别（`debug` 由 ci 与 quality 共用，`consistency` 为 Release profile）+ `rust/Cargo.lock` + `rust/rust-toolchain.toml`，带 restore key |
| Bazel | `.bazel-cache` | 仅 IAMF bridge prebuild 使用，缓存 AOM `iamf-tools` 构建产物 | OS + iamf-tools ref + bridge source hash |

所有 job 都 fresh configure。这样即使 CMake cache 或 FetchContent 状态变化，也不会复用旧 build tree。
Cargo 缓存的收益来自第三方依赖：fresh checkout 后项目自有 crate（以及以 path 引入的 `rust/vendor/sofar`）
的源码 mtime 变化，Cargo 会照常重编译它们；crates.io 依赖按版本指纹复用。release workflow 不使用 Cargo 缓存，保证发行产物从干净的
Rust 构建生成。

`.github/workflows/cache-maintenance.yml` 每周一 03:23 UTC（也可手动）运行，按 key 族只保留最近
访问的一代缓存，删除旧代以控制仓库缓存配额。

## 依赖安装建议

所有平台都需要 Rust 1.98.0（含 rustfmt / clippy）：

```bash
rustup toolchain install 1.98.0 --profile minimal --component rustfmt --component clippy
```

### macOS

```bash
brew install cmake ninja llvm cppcheck ccache
```

CI 中应显式把 Homebrew LLVM 放入 `PATH`：

```bash
echo "$(brew --prefix llvm)/bin" >> "$GITHUB_PATH"
```

### Linux

```bash
sudo apt-get update
sudo apt-get install -y cmake ninja-build build-essential git pkg-config ccache curl file patchelf desktop-file-utils
sudo apt-get install -y libfuse2t64 || sudo apt-get install -y libfuse2
```

`curl`、`file`、`patchelf`、`desktop-file-utils` 和 libfuse 只在 AppImage 打包时需要；普通 debug job
只安装 `cmake ninja-build build-essential git pkg-config ccache`。

如果 Linux job 后续启用质量检查，再安装：

```bash
sudo apt-get install -y clang-format clang-tidy cppcheck
```

### Windows

Visual Studio（含 VC x64 工具）、Ninja 和锁定的 Rust 工具链即可。
在 `vcvars64.bat` 环境中配置；维护者本机验证使用规范构建树 `build\win-canon`。
生产构建无需 Boost、vcpkg 或 OpenBLAS。只有显式启用历史 libear/libadm 参考测试时需要 Boost。

## 需要注意的边界

- APAC 编码只在 macOS 可用；Linux / Windows 上 `mr_adm_apac_smoke_tests` 会跳过。
- Apple 后端（`--renderer apple`）与 ASBR 系统空间监听只在 macOS 编译；Windows 系统空间监听 sink 只在 Windows 编译。
- SOFA 由纯 Rust `sofar`（`rust/vendor/sofar` 本地补丁）解析，默认开启；CI 使用 `tests/fixtures/sofa/` 下的小型 fixture，不下载外部 SOFA 数据集。
- RustFFT 使用标量规划器与保持算术树的独立列优化；运行时 SIMD features 继续关闭。已登记的 118 个 PCM 用例和 123 个内核文件必须三平台逐位相同，差异会使 Consistency 失败；观察项按结项范围单独记录。
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
  all: Rust fmt + clippy + FFI header check
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
  Windows Release probe build + CLI artifact

.github/workflows/reference-tests.yml
  workflow_dispatch
  Release × each default-OFF third-party migration reference (SAF on macOS, others on Linux)

.github/workflows/iamf-bridge-prebuild.yml
  workflow_dispatch / bridge-related pull_request
  macOS/Linux/Windows prebuilt AOM IAMF bridge SDK artifact

.github/workflows/cache-maintenance.yml
  weekly schedule / workflow_dispatch
  prune stale cache generations
```

## Windows 发布构建

Windows CLI release、GUI release（`scripts/release/package-windows-gui-release.ps1`）与
`windows-bringup.yml` 均以 `Release` 构建。早期改用 `MinSizeRel` 是为了绕开 MSVC 14.44 在 SAF
`saf_utility_filters.c` 上的 `/O2` 内部编译器错误；SAF 已不在生产构建中，该绕行随之取消。
Cargo 因此使用 `release` profile（而非 `minsizerel`）。

`release-windows` 在打包前用 ctest 运行一组优化构建下的测试，覆盖各渲染后端和主要编码器：
`mr_adm_flac_io_smoke_tests`、`mr_adm_opus_mka_smoke_tests`、`mr_adm_cli_smoke_tests`、
`mr_adm_ear_fixture_tests`、`mr_adm_vbap_smoke_tests`、`mr_adm_hoa_encode_fixture_tests`、
`mr_adm_binaural_fixture_tests`、`mr_adm_sofa_fixture_tests`、`mr_adm_loudness_fixture_tests`。
完整测试集仍由必需 CI 的 `windows-debug` 覆盖。

## 后续实施顺序

1. 如果 quality 太慢，保留 PR changed，必要时把 main full 改成夜间 schedule。
2. release job 后续补 macOS 签名/notarization 和完整第三方 license bundle。
3. 视 Windows release 耗时与稳定性，决定是否扩大 Release 下运行的测试集，或在 macOS / Linux release 中加入同样的优化构建 fixture。
4. 二期及后续扩展的 118 个用例已全部设为位相等门禁。后续扩展 renderer / 布局 / 语义组合时，明确新增范围并同时登记用例；数值、资源、工具链或相关依赖改动合入前，在分支手动触发 Consistency 并保留结果。

### Scene 算术的 ARM64 Release 回归

`scene-arm64-release` 在 `ubuntu-24.04-arm` 运行 Scene、Live 双耳、Live VBAP 和 HOA
四组 Release 对照，显式设置 `MR_ADM_STRICT_FP=OFF`。参考目标局部采用固定乘加规则，
生产构建保持默认选项；独立位模式、极点、channel-lock 与 HRTF 网格断言随目标执行。
Cargo 缓存键包含体系结构，避免把 ARM64 和 x64 产物作为相同缓存使用。
