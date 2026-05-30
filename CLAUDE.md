# CLAUDE.md

This file provides guidance to Claude Code (claude.ai/code) when working with code in this repository.

## 项目概要

MacinRender ADM Core 是一个跨平台 ADM（ITU-R BS.2076 Audio Definition Model）空间音频渲染核心，使用 C++20 实现，提供 `mradm` 命令行工具和稳定 C ABI 库。输入是 ADM BWF/BW64 文件，输出包括多声道扬声器、HOA、双耳，以及 WAV / CAF / FLAC / Opus MKA / IAMF / APAC 容器。详见 `README.md`。

项目长期方向是平台化重构（不是简单的 CLI 重写）：见 `docs/architecture/CPP_ADM_PLATFORM_REWRITE.md`。

## 常用构建与测试命令

主要使用 CMake preset：

```bash
cmake --preset debug
cmake --build --preset debug
ctest --preset debug --output-on-failure
```

Release / Quality preset：

```bash
cmake --preset release && cmake --build --preset release
cmake --preset quality && cmake --build --preset quality   # 编译时挂接 clang-tidy + cppcheck
```

跑单个测试：

```bash
ctest --test-dir build/debug -R mr_adm_ear_fixture_tests --output-on-failure
# 或直接执行：
./build/debug/mr_adm_ear_fixture_tests
```

CLI 二进制名固定为 `mradm`（`mradm_exe` 是 CMake target；二进制输出名是 `mradm`）。**不要**为兼容旧名再生成 `adm` 入口。

## 常用 CLI 渲染示例

始终使用 **release** 构建（`build/release/mradm`）跑实际渲染，debug 构建仅用于开发调试。

容器选择原则：
- **双耳 / ≤8ch 无高度布局**（如 stereo、5.1）→ FLAC
- **高度布局**（5.1.4 / 7.1.4 / 9.1.6 / 22.2）或 **HOA** → macOS 用 APAC（`.m4a`），Windows/跨平台测试用 Opus MKA（`.mka`）
- WAV 不作为首选输出格式

```bash
# 双耳 → FLAC（2ch）
./build/release/mradm render \
  -i input.wav -o output.binaural.flac \
  --renderer binaural

# ≤8ch 无高度扬声器布局（如 5.1）→ FLAC
./build/release/mradm render \
  -i input.wav -o output.5_1.flac \
  --output-layout 5.1

# 高度布局（7.1.4 等）→ macOS APAC
./build/release/mradm render \
  -i input.wav -o output.7_1_4.m4a \
  --output-layout 7.1.4

# 高度布局 → Windows/跨平台测试用 Opus MKA
./build/release/mradm render \
  -i input.wav -o output.7_1_4.mka \
  --output-layout 7.1.4

# HOA 3 阶 → macOS APAC
./build/release/mradm render \
  -i input.wav -o output.hoa3.m4a \
  --renderer hoa --output-layout hoa3

# HOA 3 阶 → Opus MKA（跨平台测试）
./build/release/mradm render \
  -i input.wav -o output.hoa3.mka \
  --renderer hoa --output-layout hoa3

# 查看场景元数据
./build/release/mradm inspect -i input.wav

# 列出可用渲染后端与支持布局
./build/release/mradm backends

# 查看某输出格式的最终声道顺序（可按 renderer 过滤）
./build/release/mradm layouts --output-layout 7.1.4
```

CLI 共四个子命令：`render`、`inspect`、`backends`、`layouts`（定义在 `src/adm_cli/*_command.cpp`）。`-v` 打印详细进度日志。

## 质量检查

```bash
scripts/quality/check-changed.sh --base origin/main --build-dir build/debug   # 增量
scripts/quality/check-all.sh build/debug                                       # 全量
scripts/quality/format.sh --check
scripts/quality/clang-tidy.sh build/debug
scripts/quality/cppcheck.sh build/debug
```

`check-changed.sh` 只扫描 `include/`、`src/`、`tests/` 下相对 `origin/main` + staged + worktree 的变更文件，是日常推荐的本地检查路径。`check-all.sh` 在 CI 的 main/manual 路径上跑。

clang-tidy 依赖 `compile_commands.json`，必须先 `cmake --preset debug`。macOS 上 LLVM 来自 Homebrew，需要 `export PATH="/opt/homebrew/opt/llvm/bin:$PATH"`。

## 依赖与构建选项

依赖通过 `cmake/MRDependencies.cmake` 的 `mr_adm_core_find_or_fetch()` 统一接入（`find_package(CONFIG)` 优先，FetchContent 兜底）。新增依赖**必须**走该函数，不要在 `CMakeLists.txt` 散落 `FetchContent_Declare`（ADR 0004）。

关键开关：

- `MR_ADM_CORE_FETCH_DEPS=ON/OFF` — 关闭后必须系统库齐全，用于发行版打包
- `MR_ADM_FLAC_PROVIDER=AUTO|VENDORED|SYSTEM` — Release 默认 vendored static，Debug 优先系统库
- `MR_ADM_OPUS_PROVIDER=AUTO|VENDORED|SYSTEM` — 同上
- `MR_ADM_ENABLE_SOFA=ON`（默认）— binaural 渲染器的用户 SOFA HRIR 支持
- `MR_ADM_ENABLE_IAMF=OFF`（默认）— IAMF 编码，需配合 `MR_ADM_IAMF_AOM_ROOT=/path/to/iamf-sdk` 指向预构建的官方 AOM iamf-tools bridge SDK（提供 `lib/libmr_iamf_aom_bridge.*`）。关闭时 `.iamf` 输出直接返回 `unsupported`，**不**回退到任何手写 OBU writer
- `MR_ADM_CORE_BUILD_CLI=ON`、`MR_ADM_CORE_BUILD_TESTS=ON`

CI 显式使用 `MR_ADM_FLAC_PROVIDER=VENDORED` 与 `MR_ADM_OPUS_PROVIDER=VENDORED` 以消除 runner 差异（见 `docs/guides/CI.md`）。

## 架构与目标边界

项目按模块分目标，每个模块在 `CMakeLists.txt` 中是独立的 `add_library`，通过 `MacinRender::ADM*` alias 暴露。**目标边界由 ADR 0003 强制约束**，第三方类型不允许跨边界泄漏。

依赖图（PUBLIC 表示出现在公共头；PRIVATE 表示只在实现中）：

```
ADMCore (领域模型、errors、logging、options、progress、scene、capability、render)
  ↑ PUBLIC for 几乎所有模块
ADMIo               PRIVATE: libbw64 + libadm  → AdmScene
ADMRenderCommon     无第三方；后端共用 block timeline / object preprocessing
ADMRenderEar        PRIVATE: libear + saf + ebur128 + ADMAudio
ADMRenderVBAP       PRIVATE: saf (vbap module) + ebur128 + ADMAudio
ADMRenderHOA        PRIVATE: ebur128 + ADMAudio（HOA encode；output_layout="hoa3"）
ADMRenderBinaural   PRIVATE: saf (hrir/sofa_reader/vbap/utilities) + ebur128 + ADMAudio
ADMAudio            PRIVATE: dr_wav, dr_flac, FLAC, Opus, libbw64
                    macOS: AudioToolbox + CoreFoundation（APAC / CAF metadata）
                    可选: IamfAomBridge（MR_ADM_ENABLE_IAMF）；IAMF 编码 + MP4 打包
ADMPeak / ADMLoudness  PRIVATE: ebur128 + ADMAudio
ADMEngine           PRIVATE: 上述所有；提供 RenderService 编排
ADMCAPI             PUBLIC: ADMEngine；纯 C 头 + extern "C" 实现
mradm_exe (CLI)     PRIVATE: ADMEngine + 所有 renderer + CLI11 + spdlog
```

绝对边界（ADR 0003）：

- `include/adm/*` 不得 `#include` 任何第三方 ADM/renderer 头（libadm、libbw64、libear、SAF、Apple 框架）
- `libadm` 类型只允许出现在 `src/adm_io/` 内部
- `libear` 类型只允许出现在 `src/adm_render_ear/` 内部
- SAF 类型只允许出现在 `src/adm_render_vbap/`、`src/adm_render_hoa/`（仅必要时）、`src/adm_render_binaural/` 内部
- CLI 不直接调用任何 renderer 或 IO 库；只构造 `RenderRequest`，调用 `RenderService`
- Apple 框架（AudioToolbox、CoreAudio、CoreFoundation）只允许出现在 `src/adm_audio/` 与未来的 `adm_apple` 后端

输入路径：`libbw64/libadm` → `adm_io` 适配 → `adm::AdmScene` → `RenderPlan` → `IRenderer` 后端。`RenderPlan::scene` 由 `RenderService` 填好；**后端不得自行重新解析 ADM**。

## 错误处理（ADR 0005）

- 公共 API 用 `mradm::Result<T>` = `tl::expected<T, mradm::Error>` 表达可恢复错误；不通过异常返回错误
- `Error::message` 默认中文，UTF-8；`Error::context` 携带文件路径/阶段等定位信息
- 内部允许 `throw`；**只要不跨公共边界**
- `adm_c_api` 所有导出函数必须 `noexcept`，在 `.cpp` 内 `try { ... } catch (...) { ... }` 翻译为 `adm_error_code_t`
- `mradm::ErrorCode` 与 `adm_error_code_t` 数值一一对应，由 `static_assert` 守护

## C ABI 稳定性（ADR 0007）

当前 `include/adm/c_api.h` 是 **stable v1**，自 1.0.0 起承诺向后二进制兼容，并通过 `ADM_API_VERSION_*`、`SOVERSION 1` 与 deprecation 宏维护 ABI。修改 C ABI signature、enum 数值或对象生命周期语义时必须先走 ADR/版本策略评审。

## 输出格式与渲染后端约束

- FLAC：固定 24-bit integer，最多 8 声道；超过 8ch（5.1.4、7.1.4、9.1.6、22.2）不支持
- Opus MKA：输入采样率固定 48 kHz；1–2ch 用 mapping family 0，3–8ch family 1，9–255ch family 255
- IAMF：仅 `MR_ADM_ENABLE_IAMF=ON` 构建可用；编码经 AOM iamf-tools bridge（用 integer PCM staging），输出 raw OBU stream（`.iamf`）+ Opus，面向 IAMF 测试/交付链路而非通用播放器。`--iamf-container mp4` 进一步打包为 ISOBMFF：运行时探测 PATH 中的打包器，**mp4box（GPAC）优先于 ffmpeg**（ffmpeg 需 ≥7），探测用 `fork`+`execvp` / `CreateProcessW`（不走 shell）；找不到则返回 `unsupported`。目前 IAMF 只开放到 `7.1.4`，`9.1.6`（需 expanded/Base-Enhanced IAMF）因播放器兼容性暂时禁用
- APAC：**macOS-only**；通过 AudioToolbox；CI 在 Linux 上 `mr_adm_apac_smoke_tests` 自动 skip
- 空间布局 / HOA 的 APAC 默认码率以 `7.1.4=2048 kbps` 为 12 声道基准缩放（README 输出格式表）
- HOA 输出的响度归一化可用；测量先解码到 7.1.4 AllRAD 参考播放域，LFE 不计入 LUFS 但单独计入 True Peak
- binaural 默认使用 SAF 内置 KEMAR HRTF；`--sofa <path>` 支持 SimpleFreeFieldHRIR / GeneralFIR、2 receivers、48 kHz、**不重采样**
- WAV `wav_io.cpp` 中定义 `DR_WAV_IMPLEMENTATION`；FLAC 解码 `dr_flac.cpp` 中定义 `DR_FLAC_IMPLEMENTATION`；编码用 `libFLAC`

## 代码风格

- C++20，标准模式（不允许 GNU 扩展，`CMAKE_CXX_EXTENSIONS OFF`）
- `.clang-format`：LLVM base、`ColumnLimit: 120`、`IndentWidth: 4`、`PointerAlignment: Left`、`Standard: c++20`
- `.clang-tidy`：bugprone / clang-analyzer / cppcoreguidelines / misc / modernize / performance / portability / readability，warning-only（`WarningsAsErrors: ''`）
- 命名约定：namespace lower_case、class/struct CamelCase、function/variable/enum-constant lower_case
- 错误日志、用户可见消息、ADR 文档以**中文**为主，与代码注释/英文混用
- 优先使用 `std::span`、`std::filesystem`、`std::optional`、`std::variant`、`std::jthread`/`std::stop_token`；谨慎使用 concepts / ranges；**不使用** modules、coroutines、复杂 ranges 链式、`std::format`、C++23-only 标准库

## 测试约束

- 测试不依赖私有音频素材；fixture 在运行时由代码生成（见 `tests/unit/*_fixture_test.cpp`）
- CLI smoke 测试通过 CMake 注入二进制路径：`MRADM_EXE_PATH` 编译期 define（指向 `$<TARGET_FILE:mradm_exe>`）
- 跨平台测试在 Linux 自动跳过 macOS-only 功能（APAC、CoreAudio layout）；不要把 Apple-only 路径放进默认 ctest 断言
- 行为变更必须考虑回归基线：解析摘要、布局摘要、输出声道、时长、响度、True Peak、音频误差阈值

## CI 与发布

- `.github/workflows/ci.yml` — PR/push main：macOS-15 + ubuntu-24.04 debug，FLAC/Opus 均 vendored
- `.github/workflows/quality.yml` — PR 跑 `check-changed.sh`；push main / manual full 跑 `check-all.sh`；只在 macos-15
- `.github/workflows/release.yml` — tag `v*` 或手动触发，上传 `mradm-macos` / `mradm-linux` 裸二进制

详见 `docs/guides/CI.md`。

## 关键文档索引

- `docs/architecture/CPP_ADM_PLATFORM_REWRITE.md` — 平台化重构方向、模块边界
- `docs/architecture/ADM_FEATURE_COVERAGE.md` — ADM 特性覆盖审计
- `docs/adr/0001` C++20 标准 | `0002` C++-first，Rust-later | `0003` 自有领域模型与后端边界 | `0004` 第三方依赖管理 | `0005` 错误处理模型 | `0006` CLI11 选择 | `0007` C ABI 稳定性
- `docs/guides/QUALITY.md` — 质量工具与策略
- `docs/guides/CI.md` — CI 设计与边界
- `docs/THIRD_PARTY_LICENSES.md` — 第三方许可证与发行边界
