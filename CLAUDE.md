# CLAUDE.md

This file provides guidance to Claude Code (claude.ai/code) when working with code in this repository.

## 项目概要

麦渲峰 ADM Core（英文名：MacinRender ADM Core）是一个跨平台 ADM（ITU-R BS.2076 Audio Definition Model）空间音频渲染核心，以 C++20 + Rust 实现，提供 `mradm` 命令行工具、稳定 C ABI 库，以及基于该 C ABI 的 Avalonia GUI（`gui/`）。输入是 ADM BWF/BW64 文件，输出包括多声道扬声器、HOA、双耳，以及 WAV / CAF / FLAC / Opus MKA / IAMF / APAC 容器；离线渲染之外还有实时监听（monitor）链路。详见 `README.md`。

项目长期方向是平台化重构（不是简单的 CLI 重写）：见 `docs/architecture/CPP_ADM_PLATFORM_REWRITE.md`。

**Rust 迁移现状**：数值 DSP（原 SAF 子集、计量、重采样、HRTF/双耳、HpTF、输出保护、PCM 混音、EAR 后处理、Triple Balance、HOA、Monitor、Live VBAP、Scene 空间数学/过渡）、ADM XML 元数据（原 libadm）和整数 WAVE/BW64 读写（原 libbw64）已迁入同仓库 Cargo workspace `rust/`。C++ 仍持有 `AdmScene`、语义策略、渲染编排、线程/设备调度、容器收尾与公开 C ABI；libear 仍是 EAR 后端的标准增益/滤波器设计依赖。长期目标是把 C++ 面逐步压到最小，但每一步的边界以已接受的 ADR（0008 / 0010 / 0011 / 0012）和 `docs/architecture/RUST_*_MIGRATION.md` 验收记录为准。跨平台 PCM 逐位一致是二期目标，当前只把同平台重复性作为硬门禁（见 `RUST_SAF_REPLACEMENT_ROADMAP.md` §5）。

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

Rust 是必需工具链：固定 Rust 1.98.0（`rust/rust-toolchain.toml`，含 rustfmt/clippy）+ Corrosion 0.6.1 + `Cargo.lock`（`--locked`）。CMake 只导入 `mradm-ffi` staticlib，Cargo 产物统一在 `build/rust/`。Rust 单元测试经 ctest 的 `mr_adm_rust_unit_tests`（`cargo test --workspace`）运行；格式与 lint：

```bash
cmake --build build/debug --target mr_adm_rust_quality   # cargo fmt --check + clippy
```

CLI 二进制名固定为 `mradm`（`mradm_exe` 是 CMake target；二进制输出名是 `mradm`）。**不要**为兼容旧名再生成 `adm` 入口。

### Windows 规范构建

Windows 验证在维护者的 Windows 测试机上走规范 MSVC/Ninja 配方（机器相关的脚本路径见本机 `local/` 私有笔记，不入库）：规范构建树是 `build\win-canon`（cl + vendored FLAC/Opus，IAMF/SOFA off；生产构建不再依赖 OpenBLAS/LAPACKE），干净构建只删 `build\win-canon` 后重跑配方。**不要**用 `win-debug` / `win-msvc` 备用构建树作为默认验证路径（会踩无关的 MSVC 复杂头等问题）。详见 `AGENTS.md`。

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

# 列出输出容器格式及其可用性/约束（FLAC ≤8ch、Opus 48k、APAC macOS-only 等）
./build/release/mradm formats
```

CLI 共六个用户可见子命令：`render`、`inspect`、`backends`、`layouts`、`formats`、`export`（定义在 `src/adm_cli/*_command.cpp`）。另有内部子命令 `__apac-encode`（APAC 编码子进程 worker，带 heartbeat 协议，勿在文档/GUI 中暴露）。`-v` 打印详细进度日志。

## ADM 语义策略（semantic policy）

在 import 之后、渲染之前，可用语义策略 JSON 改写场景中各 ADM 语义维度（diffuse / extent / divergence / channelLock / jumpPosition / objectImportance 等，定义见 `include/adm/semantic_policy.h`），schema 为 `mradm.semantic-policy.v1`。这是**验证 ADM 语义行为的首选路径**——不要直接编辑源 ADM BWF/WAV 文件。

```bash
# 1. 生成可编辑的中性策略模板（含场景实际取值）
./build/release/mradm inspect -i input.wav --write-semantic-policy-template policy.json

# 2. 编辑 policy.json 后应用，并把生效后的语义快照写出验证
./build/release/mradm render -i input.wav -o out.flac \
  --semantic-policy policy.json \
  --write-semantic-report report.json

# 3.（可选）把应用了语义策略的场景写回为新 ADM BWF（复用源 PCM，不重渲染）
./build/release/mradm export -i input.wav -o output.adm.wav \
  --semantic-policy policy.json
```

C ABI 对应入口：`adm_policy_template_json`（生成模板）、`adm_render_options_set_semantic_policy_path` / `adm_render_options_set_semantic_report_path`（传入 `""` 或 `NULL` 清空字段）。

## 质量检查

```bash
scripts/quality/check-changed.sh --base origin/main --build-dir build/debug   # 增量
scripts/quality/check-all.sh build/debug                                       # 全量
scripts/quality/format.sh --check
scripts/quality/clang-tidy.sh build/debug
scripts/quality/cppcheck.sh build/debug
```

`check-changed.sh` 只扫描 `include/`、`src/`、`tests/` 下相对 `origin/main` + staged + worktree 的变更文件，是日常推荐的本地检查路径；它**不覆盖 Rust**，改了 `rust/` 需另跑 `cmake --build build/debug --target mr_adm_rust_quality`（rustfmt check + clippy `-D warnings`，quality CI 同样执行）。`check-all.sh` 在 CI 的 main/manual 路径上跑。改动依赖后跑 `scripts/quality/check-licenses.sh --build-dir build/debug`（含 Cargo 依赖校验；PR CI 的 macOS/Linux debug job 同样执行，release CI 用 `--require-full`）。

clang-tidy 依赖 `compile_commands.json`，必须先 `cmake --preset debug`。macOS 上 LLVM 来自 Homebrew，需要 `export PATH="/opt/homebrew/opt/llvm/bin:$PATH"`。

## 依赖与构建选项

依赖通过 `cmake/MRDependencies.cmake` 的 `mr_adm_core_find_or_fetch()` 统一接入（`find_package(CONFIG)` 优先，FetchContent 兜底）。新增 C/C++ 依赖**必须**走该函数，不要在 `CMakeLists.txt` 散落 `FetchContent_Declare`（ADR 0004）；新增 Rust 依赖写进 `rust/Cargo.toml` 的 `[workspace.dependencies]`（精确版本 `=x.y.z`）并更新 `Cargo.lock` 与许可证清单。

当前生产 C/C++ 第三方依赖：libear（+ Boost 头文件）、dr_wav/dr_flac、libFLAC、libopus、miniaudio、CLI11、spdlog/fmt、nlohmann_json、tl-expected，可选 IAMF AOM bridge。Rust 依赖：realfft/rustfft、nalgebra、ebur128、rubato、sofar（`rust/vendor/sofar` 本地补丁）、quick-xml。

关键开关：

- `MR_ADM_CORE_FETCH_DEPS=ON/OFF` — 关闭后必须系统库齐全，用于发行版打包
- `MR_ADM_FLAC_PROVIDER=AUTO|VENDORED|SYSTEM` — Release 默认 vendored static，Debug 优先系统库
- `MR_ADM_OPUS_PROVIDER=AUTO|VENDORED|SYSTEM` — 同上
- `MR_ADM_ENABLE_SOFA=ON`（默认）— binaural 渲染器的用户 SOFA HRIR 支持
- `MR_ADM_ENABLE_IAMF=OFF`（默认）— IAMF 编码，需配合 `MR_ADM_IAMF_AOM_ROOT=/path/to/iamf-sdk` 指向预构建的官方 AOM iamf-tools bridge SDK（提供 `lib/libmr_iamf_aom_bridge.*`）。关闭时 `.iamf` 输出直接返回 `unsupported`，**不**回退到任何手写 OBU writer
- `MR_ADM_CORE_BUILD_CLI=ON`、`MR_ADM_CORE_BUILD_TESTS=ON`
- `MR_ADM_BUILD_CAPI_BUNDLE=OFF` — 打开后生成自包含 `libmradm_capi` 共享库（target `mradm_capi_bundle`），供 GUI P/Invoke 加载
- `MR_ADM_BUILD_{SAF,LIBADM,LIBBW64,EBUR128,SAMPLERATE}_REFERENCE_TESTS=OFF`（默认）— 只为维护对照获取/构建旧库参考工具，**不改变生产实现**；SAF 参考只用 Release
- `MR_ADM_STRICT_FP` / `MR_ADM_EAR_SCALAR_REFERENCE` / `MR_ADM_CONSISTENCY_DIAGNOSTICS` / `MR_ADM_DIAGNOSTIC_PORTABLE_RNG` — 一致性测量专用（`cmake/MRStrictFp.cmake`），不用于发行构建

CI 显式使用 `MR_ADM_FLAC_PROVIDER=VENDORED` 与 `MR_ADM_OPUS_PROVIDER=VENDORED` 以消除 runner 差异（见 `docs/guides/CI.md`）。

## 架构与目标边界

项目按模块分目标，每个模块在 `CMakeLists.txt` 中是独立的 `add_library`，通过 `MacinRender::ADM*` alias 暴露。**目标边界由 ADR 0003 强制约束**，第三方类型不允许跨边界泄漏。

依赖图（PUBLIC 表示出现在公共头；PRIVATE 表示只在实现中）：

```
ADMCore (领域模型、errors、logging、options、progress、scene、capability、render、semantic_policy)
  ↑ PUBLIC for 几乎所有模块；PRIVATE: ADMDsp（Scene 空间数学）
ADMDsp              INTERFACE：src/adm_dsp/*.h 私有 FFI 头 + Rust mradm-ffi staticlib（见下文 Rust 层）
ADMMetadata         PRIVATE: mradm-ffi（mradm-adm）；ADM AXML 解析/投影/回写，经类型化只读视图复制到 AdmScene
ADMIo               PRIVATE: ADMMetadata + ADMAudio + ADMRenderCommon  → AdmScene
ADMRenderCommon     PRIVATE: ADMAudio + ADMDsp；后端共用 block timeline / object preprocessing / HpTF 控制
ADMRenderEar        PRIVATE: libear + ADMDsp + ADMAudio + ADMRenderCommon
ADMRenderVBAP       PRIVATE: ADMDsp + ADMAudio + ADMRenderCommon（含 Live VBAP）
ADMRenderTripleBalance  PRIVATE: ADMRenderCommon + ADMAudio + nlohmann_json（数值状态在 Rust）
ADMRenderHOA        PRIVATE: ADMDsp + ADMAudio + ADMRenderCommon（HOA encode；output_layout="hoa3"）
ADMRenderBinaural   PRIVATE: ADMDsp + ADMAudio + ADMRenderCommon（HRTF/SOFA/卷积/spreader 均在 Rust）
ADMRenderApple      macOS-only（if(APPLE)）PRIVATE: AudioToolbox/AVFoundation/CoreMedia/Foundation
                    + ADMDsp + ADMAudio + ADMRenderCommon（AUSpatialMixer 后端 + ASBR 系统空间监听 sink）
ADMRenderWindows    Windows-only（if(WIN32)）PRIVATE: Ole32 + ADMRenderCommon（ISpatialAudioClient sink）
ADMAudio            PRIVATE: dr_wav, dr_flac, FLAC, Opus, mradm-ffi（mradm-wav 整数 WAVE/BW64）
                    macOS: AudioToolbox + CoreFoundation（APAC / CAF metadata）
                    可选: IamfAomBridge（MR_ADM_ENABLE_IAMF）；IAMF 编码 + MP4 打包
ADMPeak / ADMLoudness  PRIVATE: ADMDsp（Rust Meter）+ ADMAudio
ADMRendererFactory  RenderService 与 MonitorEngine 共用的后端选择/构造（macOS 额外链 ADMRenderApple），
                    两条链路的后端选择不允许分叉
ADMRealtime         实时监听核心：MonitorEngine + SceneStream + ring buffer + miniaudio 输出设备 + OSC 头追踪
                    PRIVATE: miniaudio + ADMDsp + ADMRenderCommon/VBAP/Binaural
                    （worker 线程渲染入 ring buffer，audio callback 不做重 DSP）
ADMEngine           PRIVATE: 上述所有（含 RendererFactory + Realtime + Metadata）；提供 RenderService 编排
                    与 monitor_session（实时监听 sink 选择）
ADMCAPI             PUBLIC: ADMEngine；纯 C 头 + extern "C" 实现
mradm_exe (CLI)     PRIVATE: ADMEngine + 所有 renderer + CLI11 + spdlog
```

### Rust 层（`rust/`）

单一 Cargo workspace（edition 2024），CMake 经 Corrosion 只导入 `mradm-ffi`：

```
mradm-dsp   #![forbid(unsafe_code)]；FFT、VBAP/MDAP、HRTF/SOFA(sofar 本地补丁)、afSTFT、去相关、OM spreader、
            卷积、HpTF、Meter(ebur128 crate)、重采样(rubato)、PCM 混音、EAR 后处理、Triple Balance、HOA、
            Monitor、Live VBAP/双耳、scene_math / scene_transition
mradm-adm   #![forbid(unsafe_code)]；ADM XML（quick-xml）、内嵌 BS.2094 common definitions、场景投影与语义回写
mradm-wav   #![forbid(unsafe_code)]；只依赖 std，RIFF/RF64/BW64 读写、u64 帧定位
mradm-ffi   staticlib；唯一含 unsafe 的私有 C 边界，聚合 mradm_dsp_* / mradm_adm_* / mradm_wav_* 等入口
```

Rust/C++ 边界规则：

- 方向是 **C++ 调 Rust**（ADR 0008 决策三）；Rust 不实现 `IRenderer` / `IRenderStream` 等 STL 接口
- FFI 用显式长度缓冲、opaque 句柄、调用方提供的错误消息缓冲；状态码对应 `adm::ErrorCode`；句柄由分配侧配对销毁；panic 不穿过 C 边界
- C++ 侧的私有 FFI 头（`src/adm_dsp/*_ffi.h`、`src/adm_metadata/adm_ffi.h`、`src/adm_audio/wav_ffi.h`，纯 C、`extern "C"` 由 `__cplusplus` 守护；C++ 包装另放 `scene_math.h` 等）是**手写**的，改 Rust 签名必须同步修改，并跑 `cmake --build build/debug --target mr_adm_ffi_header_check`（需 `cargo install cbindgen --locked --version 0.29.2`；比较导出符号、签名与 `#[repr(C)]` 字段，quality CI 同样执行）。被签名引用的 `#[repr(C)]` 类型名在 workspace 内必须唯一（cbindgen 按名字合并）。Rust 类型与这些头不得出现在 `include/adm/*`
- 共享 C ABI bundle 只导出 `c_api.h` 声明的 `adm_*`（当前 139 个；Windows 用从 `c_api.h` 生成的 `.def`）；Rust 分配器、panic 入口与私有 `mradm_*` 符号不得外泄，`scripts/quality/check-capi-exports.py <lib>` 在 ci/release 校验
- 新的 Rust 依赖必须锁定精确版本、关闭不需要的 features，并登记到许可证清单/SBOM（`scripts/quality/check-licenses.sh` 会校验 Cargo 依赖）
- 迁移批次的惯例：先保留旧 C++ 实现作对照（`tests/reference/*` + `provenance.json`，或 `MR_ADM_BUILD_*_REFERENCE_TESTS` 开关），记录验收到 `docs/architecture/RUST_*_MIGRATION.md` 与 `evidence/`；**禁止**把旧实现当运行时静默回退
- `tests/reference/` 下每个文件都登记在 `tests/reference/retention.json`（单元、测试、开关、SHA-256、未满足的退役条件），`scripts/quality/check-reference-retention.py` 校验（ci 的 version-metadata job）；冻结参考不得改算法，必须改时同步更新哈希并在迁移文档说明。退役条件（已发布 + 独立回归 + 二期确认）与步骤见 `docs/architecture/RUST_REFERENCE_RETENTION.md`

### 绝对边界（ADR 0003，按迁移现状更新）

- `include/adm/*` 不得 `#include` 任何第三方 ADM/renderer 头（libear、Apple 框架等）或 Rust FFI 头
- `libear` 类型只允许出现在 `src/adm_render_ear/` 内部（测试 `mr_adm_ear_post_tests` 除外）
- libadm / libbw64 / SAF / libebur128 / libsamplerate **已不是生产依赖**，只能出现在对应 `MR_ADM_BUILD_*_REFERENCE_TESTS` 开关下的参考测试与工具中；不得重新引入生产路径
- CLI 不直接调用任何 renderer 或 IO 库；只构造 `RenderRequest`，调用 `RenderService`
- Apple 框架（AudioToolbox、CoreAudio、CoreFoundation、AVFoundation）只允许出现在 `src/adm_audio/` 与 `src/adm_apple/`（`if(APPLE)` 门控）
- Windows COM / SpatialAudio（`spatialaudioclient.h`、`mmdeviceapi.h`、WRL）只允许出现在 `src/adm_windows/`（Windows-only 系统空间监听 sink，`if(WIN32)` 门控）；工厂返回第三方无关的 `IAudioOutputDevice`

输入路径：WAVE 容器（`mradm-wav` / C++ 容器元数据）+ AXML（`mradm-adm`）→ `adm_metadata` / `adm_io` 适配 → `adm::AdmScene` → `RenderPlan` → `IRenderer` 后端。`RenderPlan::scene` 由 `RenderService` 填好；**后端不得自行重新解析 ADM**，渲染循环中也不持有 Rust ADM 句柄。

## 错误处理（ADR 0005）

- 公共 API 用 `mradm::Result<T>` = `tl::expected<T, mradm::Error>` 表达可恢复错误；不通过异常返回错误
- `Error::message` 默认中文，UTF-8；`Error::context` 携带文件路径/阶段等定位信息
- 内部允许 `throw`；**只要不跨公共边界**
- `adm_c_api` 所有导出函数必须 `noexcept`，在 `.cpp` 内 `try { ... } catch (...) { ... }` 翻译为 `adm_error_code_t`
- `mradm::ErrorCode` 与 `adm_error_code_t` 数值一一对应，由 `static_assert` 守护

## C ABI 稳定性（ADR 0007）

当前 `include/adm/c_api.h` 是 **stable v1.x**（版本号看 `ADM_API_VERSION_*` 宏，勿在文档硬编码 minor），自 1.0.0 起承诺向后二进制兼容，并通过 `SOVERSION 1` 与 deprecation 宏维护 ABI。修改 C ABI signature、enum 数值或对象生命周期语义时必须先走 ADR/版本策略评审。结构体扩展一律走 `struct_size` 向后兼容模式。

GUI 新接入进度条优先使用 `adm_render_file_ex2` / `adm_preview_render_window_v2` 的结构化 progress v2；旧 `adm_progress_cb` 仅保留兼容单一 fraction/stage/message 的调用方。v2 的 `message` 指针与旧 callback 一样只在回调期间有效。

实时监听经 `adm_monitor_*` 家族：create/play/pause/seek/loop/status/levels/log、`adm_monitor_set_overrides`（gain 即时；diffuse/extent/divergence 视后端可能轻量 re-prepare）、`adm_monitor_switch_backend`（热切换后端/布局带交叉淡化）、`adm_monitor_output_devices_json` + `adm_create_monitor_ex` / `adm_monitor_set_output_device`（输出设备枚举与切换）、`adm_monitor_set_listener_orientation`（头追踪/自由视角）、`adm_monitor_set_hptf_parameters` + `adm_monitor_get_hptf_info`（HpTF 耳机补偿，直接提交内存 PEQ 参数；`adm_hptf_parse_parametric_eq` 用于文本导入，旧文件入口保留；仅双声道耳机馈送，切换带交叉淡化，**刻意不影响 LUFS 表**——换耳机不该改变节目响度读数）。

## 输出格式与渲染后端约束

- FLAC：固定 24-bit integer，最多 8 声道；超过 8ch（5.1.4、7.1.4、9.1.6、22.2）不支持
- Opus MKA：输入采样率固定 48 kHz；1–2ch 用 mapping family 0，3–8ch family 1，9–255ch family 255
- IAMF：仅 `MR_ADM_ENABLE_IAMF=ON` 构建可用；编码经 AOM iamf-tools bridge（用 integer PCM staging），输出 raw OBU stream（`.iamf`）+ Opus，面向 IAMF 测试/交付链路而非通用播放器。`--iamf-container mp4` 进一步打包为 ISOBMFF：运行时探测 PATH 中的打包器，**mp4box（GPAC）优先于 ffmpeg**（ffmpeg 需 ≥7），探测用 `fork`+`execvp` / `CreateProcessW`（不走 shell）；找不到则返回 `unsupported`。目前 IAMF 只开放到 `7.1.4`，`9.1.6`（需 expanded/Base-Enhanced IAMF）因播放器兼容性暂时禁用
- APAC：**macOS-only**；通过 AudioToolbox；CI 在 Linux 上 `mr_adm_apac_smoke_tests` 自动 skip
- 空间布局 / HOA 的 APAC 默认码率以 `7.1.4=2048 kbps` 为 12 声道基准缩放（README 输出格式表）
- HOA 输出的响度归一化可用；测量先解码到 7.1.4 AllRAD 参考播放域，LFE 不计入 LUFS 但单独计入 True Peak
- binaural 默认使用内置 KEMAR HRTF（已提交的二进制资源 `rust/crates/mradm-dsp/assets/`，`manifest.json` 记录来源与 SHA-256，构建不再从 SAF 提取）；`--sofa <path>` 支持 SimpleFreeFieldHRIR / GeneralFIR、2 receivers、48 kHz、**不重采样**
- `--renderer apple`：**macOS-only** AUSpatialMixer 后端（`src/adm_apple/`），能力见 `apple_capabilities()`，在 Linux 不编译；`mr_adm_apple_smoke_tests` 在非 macOS 跳过
- 系统空间音频监听（`monitor_system_spatial`，仅实时监听非离线）：把多声道床交 OS 做 HRTF。**macOS** 经 `AVSampleBufferAudioRenderer`（`src/adm_apple/avsamplebuffer_device.mm`，含动态头追踪）；**Windows** 经 `ISpatialAudioClient`（`src/adm_windows/spatialaudioclient_device.cpp`，Windows Sonic / Dolby Atmos / DTS 头戴，**静态空间化无 OS 头追**，需声音设置启用某空间格式否则返回 `unsupported`）。布局白名单各自由 `apple_layouts` / `windows_layouts` 定义，经 capabilities JSON 的 `system_spatial_layouts` 字段统一暴露给 GUI（**唯一权威源，勿在 GUI 硬编码**）。sink 选择在 `monitor_session.cpp::make_monitor_device`
- WAV `wav_io.cpp` 中定义 `DR_WAV_IMPLEMENTATION`；FLAC 解码 `dr_flac.cpp` 中定义 `DR_FLAC_IMPLEMENTATION`；编码用 `libFLAC`
- WAV / BW64 IO 是 64-bit clean（支持 >4GB 母版与输出）：**f32 WAV 经 dr_wav 固定写 RF64**（流式写无法预知总大小，统一用 `ds64` 承载真实大小，小文件也是 RF64）；**整数 WAV 读取与写入经 Rust `mradm-wav`**（C++ 包装在 `src/adm_audio/wav_backend.h`）：默认先写 RIFF，需要 64 位长度时整数升级 BW64、浮点升级 RF64；writer 必须显式 `finish()`，整数转换写排他临时文件、成功后才安装，失败/取消保留原文件；reader 以 u64 帧 `seek_frame` 定位（不再有 libbw64 的 2^31 帧上限）。`write_wav_metadata`（bext/ambi 追加）仍在 C++，用 `_fseeki64`/`fseeko` + uint64 全程 64-bit，按 RIFF/RF64/BW64 分别更新顶层 size 或 `ds64.bw64Size`。Windows 私有 FFI 路径先按当前进程代码页把原生窄路径转 UTF-8，**不要**靠字节是否为合法 UTF-8 猜编码。回归守卫用稀疏文件造 >4GB / 跨 2^31 帧 fixture（`core_smoke_test` / `render_trim_fixture_test`，不真烧盘）；真实 >4GiB 写入测试默认忽略，需设 `MRADM_WAV_LARGE_TEST_DIR`

## GUI（gui/MacinRender.Gui）

Avalonia / .NET 10 **NativeAOT** 前端（MIT 边界），经 P/Invoke（`[LibraryImport("mradm_capi")]`）调用 C ABI，不直接触碰 C++ 类型。GUI 的后端/编码器/布局/特性可用性一律来自核心 capabilities / support-matrix JSON，**不要在 GUI 硬编码支持表**。

macOS 本地开发链路（一期手动，脚本内有说明）：

```bash
# 1. 构建自包含 C ABI dylib
cmake --preset release -DMR_ADM_BUILD_CAPI_BUNDLE=ON
cmake --build --preset release --target mradm_capi_bundle
# 2. 拷入 GUI runtimes/ + 编译头追踪 shim（CoreMotion / AirPods，macOS-only）
gui/copy-native.sh && gui/build-headtrack.sh
# 3. 打包 .app（ad-hoc 签名；AirPods 头追踪需 Info.plist NSMotionUsageDescription + 签名，裸 exe 会被 TCC 杀）
gui/package-macos-gui-dev-app.sh   # 产物 gui/dist/MacinRender.app，不入 git
```

AOT 注意：markup extension 返回 `IObservable` 会 cast crash、索引器反射绑定触发 IL 警告——运行时 i18n 用 `DynamicResource` + 显式资源更新。

## 代码风格

- C++20，标准模式（不允许 GNU 扩展，`CMAKE_CXX_EXTENSIONS OFF`）
- `.clang-format`：LLVM base、`ColumnLimit: 120`、`IndentWidth: 4`、`PointerAlignment: Left`、`Standard: c++20`
- `.clang-tidy`：bugprone / clang-analyzer / cppcoreguidelines / misc / modernize / performance / portability / readability，warning-only（`WarningsAsErrors: ''`）
- 命名约定：namespace lower_case、class/struct CamelCase、function/variable/enum-constant lower_case
- 错误日志、用户可见消息、ADR 文档以**中文**为主，与代码注释/英文混用
- 优先使用 `std::span`、`std::filesystem`、`std::optional`、`std::variant`、`std::jthread`/`std::stop_token`；谨慎使用 concepts / ranges；**不使用** modules、coroutines、复杂 ranges 链式、`std::format`、C++23-only 标准库
- Rust：edition 2024、默认 rustfmt；算法 crate（`mradm-dsp` / `mradm-adm` / `mradm-wav`）保持 `#![forbid(unsafe_code)]`，unsafe 只写在 `mradm-ffi`；准备阶段之后的处理调用不应分配内存（FFT、运动控制、spreader 等已有分配计数测试约束）；随机状态归实例所有，不用全局 RNG

## 测试约束

- 测试不依赖私有音频素材；fixture 在运行时由代码生成（见 `tests/unit/*_fixture_test.cpp`）
- CLI smoke 测试通过 CMake 注入二进制路径：`MRADM_EXE_PATH` 编译期 define（指向 `$<TARGET_FILE:mradm_exe>`）
- 跨平台测试在 Linux 自动跳过 macOS-only 功能（APAC、CoreAudio layout）；不要把 Apple-only 路径放进默认 ctest 断言
- 行为变更必须考虑回归基线：解析摘要、布局摘要、输出声道、时长、响度、True Peak、音频误差阈值

## CI 与发布

- `.github/workflows/ci.yml` — PR/push main：macOS + Linux + Windows debug（均安装 Rust 1.98.0，带 Cargo 缓存），FLAC/Opus 均 vendored；Windows job 用 vcpkg Boost（libear 依赖）；macOS/Linux 构建后跑 `check-licenses.sh --build-dir build/debug`，新增依赖未登记会让 PR 失败
- `.github/workflows/consistency.yml` — push main / manual：记录跨平台 PCM 差异，门禁输入完整性与同进程重复性（逐位一致属二期）
- `.github/workflows/quality.yml` — 所有触发跑 Rust fmt/clippy 与 FFI 头校验；PR 跑 `check-changed.sh`；push main / manual full 跑 `check-all.sh`；只在 macOS
- `.github/workflows/windows-bringup.yml` — 手动触发的 Windows MSVC Release 探针构建（vcpkg Boost）
- `.github/workflows/release.yml` — tag `v*` 或手动触发：macOS CLI `.tar.gz`、Linux CLI `.AppImage`、Windows CLI `.zip`，外加 macOS/Windows GUI 包（`MacinRender-Gui-*`，经 `scripts/release/package-*.sh` + smoke 脚本）
- `.github/workflows/reference-tests.yml` — 手动触发：Linux Release 逐个打开 `MR_ADM_BUILD_*_REFERENCE_TESTS` 跑第三方对照，防止默认 OFF 的参考腐烂
- `.github/workflows/iamf-bridge-prebuild.yml` — 预构建 AOM iamf-tools bridge SDK；`cache-maintenance.yml` — FetchContent/ccache 缓存维护

详见 `docs/guides/CI.md`。

## 关键文档索引

- `docs/architecture/CPP_ADM_PLATFORM_REWRITE.md` — 平台化重构方向、模块边界
- `docs/architecture/ADM_FEATURE_COVERAGE.md` — ADM 特性覆盖审计
- `docs/architecture/ADM_APPLE_BACKEND.md` — macOS AUSpatialMixer 后端 + ASBR 系统空间监听 sink
- `docs/architecture/ADM_WINDOWS_SYSTEM_SPATIAL.md` — Windows ISpatialAudioClient 系统空间监听 sink（静态床/能力实测/切换恢复）
- `docs/architecture/hptf-eq.md` — HpTF 耳机补偿（v1.38 内存参数接口、AutoEq ParametricEQ，实时监听专用，设备绑定）
- `docs/adr/0001` C++20 标准 | `0002` C++-first，Rust-later | `0003` 自有领域模型与后端边界 | `0004` 第三方依赖管理 | `0005` 错误处理模型 | `0006` CLI11 选择 | `0007` C ABI 稳定性 | `0008` Rust 落地与 SAF 替换 | `0009` 头追踪输入边界 | `0010` Rust Meter | `0011` Rust 固定采样率转换 | `0012` Rust ADM 元数据与 libadm 参考边界
- `docs/architecture/RUST_REFERENCE_RETENTION.md` — 迁移参考实现的登记、冻结校验与退役条件
- `docs/architecture/RUST_SAF_REPLACEMENT_ROADMAP.md` — Rust 一期总览与二期（跨平台逐位一致）计划；各批次验收见 `docs/architecture/RUST_*_MIGRATION.md`（ADM、BW64、Live VBAP、Scene numeric、HOA、Monitor 等）及 `docs/architecture/evidence/`
- `docs/guides/QUALITY.md` — 质量工具与策略
- `docs/guides/CI.md` — CI 设计与边界
- `docs/THIRD_PARTY_LICENSES.md` — 第三方许可证与发行边界
