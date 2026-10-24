# CLAUDE.md

This file provides guidance to Claude Code (claude.ai/code) when working with code in this repository.

## 项目概要

麦渲峰 ADM Core（英文名：MacinRender ADM Core）是一个跨平台 ADM（ITU-R BS.2076 Audio Definition Model）空间音频渲染核心，以 C++20 + Rust 实现，提供 `mradm` 命令行工具、稳定 C ABI 库，以及基于该 C ABI 的 Avalonia GUI（`gui/`）。输入是 ADM BWF/BW64 文件，输出包括多声道扬声器、HOA、双耳，以及 WAV / CAF / FLAC / Opus MKA / IAMF / APAC 容器；离线渲染之外还有实时监听（monitor）链路。详见 `README.md`。

项目最终目标是尽可能减少 C++、逆转为 Rust 项目（ADR 0021，2026-10-16）：C/C++ 只留在 Apple / Windows 平台岛与第三方 C 库中，对外契约只有 `c_api.h`、CLI 行为与锁定矩阵上的渲染结果。路线与模块去向见 `docs/architecture/RUST_INVERSION_ROADMAP.md`；旧的 `CPP_ADM_PLATFORM_REWRITE.md` 为历史规划。

渲染流程统一称为**离线渲染**（`IRenderer::render_window`）、**文件流式渲染**
（`IRenderer::open_stream` / `IRenderStream`）和 **Scene 流式渲染**
（`SceneStreamEngine` / `ILiveSceneRenderer`）。讨论、文档、注释与诊断按功能命名，
不按实现先后称呼，也不暗示 Scene 替代文件渲染；对照实现注明语言、版本或提交。
完整定义见[渲染流程命名](docs/README.md#渲染流程命名)。

**Rust 迁移现状**：数值 DSP（原 SAF 子集、计量、重采样、HRTF/双耳、HpTF、输出保护、PCM 混音、EAR 后处理、Triple Balance、HOA、Monitor、Live VBAP、Scene 空间数学/过渡）、EAR 布局/增益/FIR 设计（原 libear）、ADM XML 元数据（原 libadm）、输入导入与声道床、扬声器布局表（`mradm-scene`，四期 4.3）、语义策略（解析、匹配、应用、报告与模板，Scene 流式的逐更新应用）、导出回写与 DirectSpeakers 矩阵 / 屏幕配置 JSON（`mradm-scene` + `mradm-json`，四期 4.4）、扬声器后端（VBAP、HOA、EAR 与 Scene 流式 VBAP，含 EAR 的 channelLock/divergence 预处理与 22.2 LFE 策略）及其公共渲染层（DirectSpeakers / 标签 / LFE 路由、扬声器离线驱动与文件流式；`mradm-render`，四期 4.5）、WAVE/RF64/BW64 样本读写与容器元数据（原 libbw64 / dr_wav / C++ chunk 改写）、PoseBridge OSC 协议解析（`mradm-osc`，见 `RUST_OSC_PROTOCOL_MIGRATION.md`）和 AutoEq ParametricEQ 文本解析（`mradm-dsp` `hptf::parametric_eq`，见 `RUST_HPTF_PARSE_MIGRATION.md`）已迁入同仓库 Cargo workspace `rust/`。C++ 仍持有 `AdmScene`、各后端的适配壳与能力表、渲染编排、线程/设备调度（含 OSC 头追踪的 UDP 收发与快照）、输出文件的临时文件/替换编排与公开 C ABI。长期目标是把 C++ 面逐步压到最小，但每一步的边界以已接受的 ADR（0008 / 0010 / 0011 / 0012 / 0013 / 0014 / 0015 / 0016 / 0017 / 0018）和 `docs/architecture/RUST_*_MIGRATION.md` 验收记录为准。二期已按锁定矩阵结项（见 `docs/architecture/RUST_PHASE2_CLOSEOUT.md`）：macOS arm64 / Linux x64 / Windows x64 的全部 78 个 PCM 用例逐位相同并设为门禁。完成切片包括 Scene `scene-separate-v1`（ADR 0014）、RustFFT 标量规划器（ADR 0015）、重采样固定归约及可移植三角函数（ADR 0016）、OM spreader 固定分组与可移植数学函数（ADR 0017），以及保持每列算术树的 FFT 独立列优化。后续覆盖已扩展到 118 个精确 PCM id 和 8 个内核模式（123 个文件），新增 96 kHz Scene、96/192 kHz 重采样、HRTF 边界及退化 OM 输入（见 `RUST_COVERAGE_EXTENSION.md`）；一期 1.3 起 FFT twiddle 取自 `libm`，f64 twiddle 列也设为门禁；1.4 起 HpTF 设计内核同样设为门禁；二期 2.1 起生产计量的 True Peak 也设为门禁，2.2 起计量的 K-weighting 系数与响度也设为门禁，2.3 起离线后处理增益内核也设为门禁，2.4 起计量验收节目（门限边界、跨采样峰值、长静音、分块、reset）也设为门禁；三期 3.1 起文件流式双耳重放（6 个 PCM 用例）与文件流式/监听运算内核也设为门禁，3.2 起 EAR 增益内核与 6 个 EAR 文件流式重放也设为门禁，3.3 起 HOA 编解码内核、三阶 HOA 与 HOA 响度离线用例及 6 个 HOA 文件流式重放也设为门禁，3.4 起 Triple Balance 声像内核、size 与 D 模式离线用例及 6 个 Triple Balance 文件流式重放也设为门禁，3.5 起 ADM 导入换算、十进制参数解析内核与 5 个 `params-*` 离线用例也设为门禁，3.6 起秒到帧换算、整数 WAVE 量化内核与 9 个 `final-*` 最终文件用例也设为门禁（三期结项）；四期 4.2 起 5 个观察内核（EAR 布局、Scene）也设为门禁，并补上语义策略矩阵、DirectSpeakers 矩阵、各后端离线平滑与后置增益和 `room-binaural` exact 档的用例（见 `RUST_INVERSION_COVERAGE.md`）；4.4 起语义策略与参数 JSON 在 Rust，`06-policy` 由 Rust 写出，extent 缺省上限改为 ADM 取值范围（见 `RUST_INVERSION_POLICY.md`）；4.5 起扬声器后端与公共渲染层在 Rust `mradm-render`，`speaker/s*`、`vbap.0*`、`hoa.0*`、`ear.*` 与 `.30-vbap*` 检查点由 Rust 写出（见 `RUST_INVERSION_SPEAKERS.md`）。新增 PCM 用例必须同步加入门禁（`phase2_tools_test.py` 强制）；不得把当前矩阵扩展为任意输入或工具链的全局承诺。dr_flac 按用户决定暂缓；覆盖继续按批次扩展，进一步优化与参考退役分别推进。

上述迁移清单表示主要内核已经迁入 Rust；4.5 起扬声器后端的计划构建与渲染循环也在 Rust（C++ 只驱动离线循环并负责写出、取消与进度），双耳、Triple Balance 与 room 仍有计划构建、块时间线、系数组装、样本归约和渲染循环，尚非纯调度层。四期（ADR 0021）把 PCM 写出为止的数据链路整体迁入 Rust（导入投影、语义策略、全部自有后端、公共渲染层、PCM 写出编排），C++ 只留适配壳与调度，同时让 `room-binaural` exact 档进矩阵、收尾 S03 / 观察内核 / N01 JSON 残留与覆盖缺口；切片 4.1–4.10 见 `PCM_CONSISTENCY_AUDIT.md` 的四期一节。
一期 1.2 已把 Scene 的语义策略数值求值（`mradm-dsp` `semantic_policy`，dB 经 `libm::powf`）、Live VBAP 源增益合成与 Triple Balance 床系数迁入 Rust（见 `RUST_SCENE_RESIDUAL_MIGRATION.md`）；1.3 让 Scene 空间数学、HRTF/SOFA 几何与 FFT twiddle 全部改用 `libm`，RealFFT 3.5.0 因此纳入 `rust/vendor/realfft`（见 `RUST_SCENE_PORTABLE_MATH.md`）；1.4 把 HpTF 设计与峰值保护释放系数改用 `libm`，HpTF 尾音清理改为固定 1024 帧网格，无保护输出的音量乘法移入 Rust（见 `RUST_SCENE_OUTPUT_DSP.md`）；1.5 核对事件、时钟、分块、reset、EOS 与欠载规则，HpTF 排队目标改为在当前淡化结束的那一帧开始，11 个同采样率 VBAP/Triple Balance 回放用例要求固定与碎片分块逐位相同，双耳卷积与分数比率重采样记录为依赖分块（见 `RUST_SCENE_TIMING.md`）；1.6 让一致性 config A 使用发布构建的选项（C ABI bundle / PIC），新增变采样后接 HpTF 与峰值保护的外部 SOFA 与 VBAP 设备回放（PCM 门禁增至 196 个），并以分配计数测试约束设备回调（见 `RUST_SCENE_RELEASE_ACCEPTANCE.md`）。二期 2.1 把 ebur128 纳入 `rust/vendor` 并让 True Peak 插值系数改用 `libm`，三平台确认 192 kHz 响度存在微小分歧（见 `RUST_METER_TRUE_PEAK.md`）；2.2 让 K-weighting、门控系数与 LUFS 换算改用 `libm`，并去掉 x86 FTZ、所有架构统一清零极小滤波器状态，响度三平台相同并设为门禁（见 `RUST_METER_LOUDNESS.md`）；2.3 把离线后处理的峰值转 dB、合成增益判定、`apply_loudness_norm` / `apply_peak_limit` 的增益与逐样本乘法移入 `mradm-dsp` `post_gain`（`libm`，见 `RUST_POST_GAIN.md`）；2.4 以独立的 BS.1770-4 参考与 EBU Tech 3341 合成信号验收计量准确性，把 ebur128 极小状态的清零改到 100 ms 块网格上使读数与分块无关，并重新测量归一化后的最终 PCM（见 `RUST_METER_ACCEPTANCE.md`）。三期 3.1 把双耳文件流式渲染的逐源增益求和与拓扑淡化、双耳 extent 锥角、Monitor 立体声折叠矩阵与实时覆盖增益的 dB 换算移入 `mradm-dsp` `stream_dsp` / `monitor::stereo_fold`（分别舍入、`libm`），一致性采集新增文件流式重放工具 `mr_adm_phase2_stream`（见 `RUST_FILE_STREAM_BINAURAL.md`）；3.2 让 `mradm-ear` 的几何、extent 权重、立体声 panner 与去相关随机相位改用 `libm`（HOA 解码留到 3.3），重放工具经共享的渲染器工厂支持 EAR（见 `RUST_EAR_PORTABLE_MATH.md`）；3.3 让 HOA 编码与 EAR HOA 解码改用 `libm`，AllRAD 解码器的矩阵乘积改为固定顺序的标量计算（不再经 nalgebra 动态矩阵调用运行时选择 SIMD 内核的 matrixmultiply），HOA 文件流式的实时覆盖改按声道解析扬声器标签（见 `RUST_HOA_PORTABLE_MATH.md`）；3.4 去掉 Triple Balance 的 `PORTABLE` 泛型，标准模式的离线、文件流式与 Scene 入口共用 `libm` 实现与 size 积分格点（见 `RUST_TRIPLE_BALANCE_PORTABLE_MATH.md`）；3.5 让 ADM 导入的 dB 增益与 Cartesian 投影改用 `libm`（`mradm-adm` `numeric`），CLI 浮点选项与 HOA 标签改用 Rust 正确舍入、与 locale 无关的十进制解析（`mradm-dsp` `decimal`），标签 / UID 规范化只看 ASCII，同起点块在各后端建立增益、总线或路由前只保留文档顺序中的最后一块（见 `RUST_PARAMETER_ENTRY.md`）；3.6 让输出裁剪、预览窗口与 Monitor 定位共用 Rust 的秒到帧换算（`mradm-dsp` `timeline`），CAF float32 显式按小端序列化，并为整数 WAVE 量化、最终增益、声道排列与 CAF 建立门禁，三期结项（见 `RUST_FINAL_PCM.md`）。
剩余数值运算与本轮分期统一维护在 [PCM 一致性审计与 Rust 迁移路线](docs/architecture/PCM_CONSISTENCY_AUDIT.md)：
一期 Scene 流式渲染及其实际共享依赖，二期计量与归一化，三期文件流式渲染和离线渲染；Apple 后端及后续编码排除。
基于 `456a3ba` 的门禁清单为 188 个 PCM id、160 份内核测量，其中 150 份内核设为门禁（1.3 起为 151 份，1.4 起为 155 份；1.6 起 PCM id 为 196 个；2.1 起内核测量 184 份、门禁 171 份；2.2 起内核测量 188 份、门禁 183 份；2.3 起内核测量 193 份、门禁 188 份；2.4 起内核测量 213 份、门禁 208 份；3.1 起 PCM id 为 202 个，内核测量 217 份、门禁 212 份；3.2 起 PCM id 为 208 个，内核测量 220 份、门禁 215 份；3.3 起 PCM id 为 216 个，内核测量 224 份、门禁 219 份；3.4 起 PCM id 为 228 个，内核测量 228 份、门禁 223 份；3.5 起 PCM id 为 233 个，内核测量 231 份、门禁 226 份；3.6 起 PCM id 为 242 个，内核测量 235 份、门禁 230 份；4.2 起 PCM id 为 283 个，内核测量 237 份并全部设为门禁）；
清单规模与历史已完成验收分别记录，不将 118 项历史报告直接当作新增范围的三平台结果。

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
  --renderer saf-binaural

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
./build/release/mradm inspect input.wav

# 列出可用渲染后端与支持布局
./build/release/mradm backends

# 查看某输出格式的最终声道顺序（可按 renderer 过滤）
./build/release/mradm layouts --output-layout 7.1.4

# 列出输出容器格式及其可用性/约束（FLAC ≤8ch、Opus 48k、APAC macOS-only 等）
./build/release/mradm formats
```

CLI 共七个用户可见子命令：`render`、`inspect`（输入为位置参数，不用 `-i`）、`backends`、`layouts`、`input-layouts`、`formats`、`export`（定义在 `src/adm_cli/*_command.cpp`，完整参考见 `docs/guides/CLI_USAGE.md`）。另有内部子命令 `__apac-encode`（APAC 编码子进程 worker，带 heartbeat 协议，勿在文档/GUI 中暴露）。`-v` 打印详细进度日志。

## ADM 语义策略（semantic policy）

在 import 之后、渲染之前，可用语义策略 JSON 改写场景中各 ADM 语义维度（diffuse / extent / divergence / channelLock / jumpPosition / objectImportance 等，定义见 `rust/crates/mradm-scene/src/policy.rs`），schema 为 `mradm.semantic-policy.v1`。这是**验证 ADM 语义行为的首选路径**——不要直接编辑源 ADM BWF/WAV 文件。extent 的 `max` 使用块自身的单位（极坐标 width / height 为度）；不写时只钳到 ADM 取值范围（极坐标 width / height ≤ 360、depth ≤ 1，Cartesian ≤ 1），中性模板不写 `max`。

```bash
# 1. 生成可编辑的中性策略模板（含场景实际取值）
./build/release/mradm inspect input.wav --write-semantic-policy-template policy.json

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

C/C++ 规模棘轮（ADR 0021）：`python3 scripts/quality/language-surface.py --check`（CI `version-metadata` job 执行）；删除 C++ 后在同一提交运行 `--update`。

clang-tidy 依赖 `compile_commands.json`，必须先 `cmake --preset debug`。macOS 上 LLVM 来自 Homebrew，需要 `export PATH="/opt/homebrew/opt/llvm/bin:$PATH"`。

## 依赖与构建选项

依赖通过 `cmake/MRDependencies.cmake` 的 `mr_adm_core_find_or_fetch()` 统一接入（`find_package(CONFIG)` 优先，FetchContent 兜底）。新增 C/C++ 依赖**必须**走该函数，不要在 `CMakeLists.txt` 散落 `FetchContent_Declare`（ADR 0004）；新增 Rust 依赖写进 `rust/Cargo.toml` 的 `[workspace.dependencies]`（精确版本 `=x.y.z`）并更新 `Cargo.lock` 与许可证清单。

当前生产 C/C++ 第三方依赖：dr_flac、libFLAC、libopus、miniaudio、CLI11、spdlog/fmt、nlohmann_json（只剩 engine 的能力表与场景 JSON、C ABI、Triple Balance 语义与 OSC 遥测；ADMCore 与语义策略不再使用，四期 4.4）、tl-expected，可选 IAMF AOM bridge。Rust 依赖：realfft/rustfft（`rust/vendor/rustfft` 保持位模式的列优化补丁；两者的 twiddle 均改用 `libm`，`rust/vendor/realfft` 只有这一处补丁）、nalgebra（`libm-force`，ADR 0017）、libm、ebur128（`rust/vendor/ebur128` 本地补丁：True Peak 插值系数改用 `libm`，二期 2.1；K-weighting、门控系数与 LUFS 换算改用 `libm`，去掉 x86 FTZ，二期 2.2；极小状态在 100 ms 块网格上清零，二期 2.4）、rubato（`rust/vendor/rubato` 本地补丁，ADR 0016）、sofar（`rust/vendor/sofar` 本地补丁）、quick-xml、serde_json（开启 `std` / `float_roundtrip`、无类型 `Value`，PoseBridge 遥测）。项目内 crate `mradm-json`（MIT，移植 nlohmann 的接受规则与 Grisu2，`NOTICE.txt` / `PROVENANCE.json` 登记）不是外部依赖。`mradm-ear` 是移植自 libear 的项目内 crate（Apache-2.0，来源与数据登记在 crate 的 `LICENSE` / `NOTICE.txt` / `PROVENANCE.json`），`mradm-math` 是移植自 musl 的可移植 sin/cos（MIT，同样登记 `NOTICE.txt` / `PROVENANCE.json`），都不是外部依赖。生产构建不需要 Boost / vcpkg。

关键开关：

- `MR_ADM_CORE_FETCH_DEPS=ON/OFF` — 关闭后必须系统库齐全，用于发行版打包
- `MR_ADM_FLAC_PROVIDER=AUTO|VENDORED|SYSTEM` — Release 默认 vendored static，Debug 优先系统库
- `MR_ADM_OPUS_PROVIDER=AUTO|VENDORED|SYSTEM` — 同上
- `MR_ADM_ENABLE_SOFA=ON`（默认）— binaural 渲染器的用户 SOFA HRIR 支持
- `MR_ADM_ENABLE_IAMF=OFF`（默认）— IAMF 编码，需配合 `MR_ADM_IAMF_AOM_ROOT=/path/to/iamf-sdk` 指向预构建的官方 AOM iamf-tools bridge SDK（提供 `lib/libmr_iamf_aom_bridge.*`）。关闭时 `.iamf` 输出直接返回 `unsupported`，**不**回退到任何手写 OBU writer
- `MR_ADM_CORE_BUILD_CLI=ON`、`MR_ADM_CORE_BUILD_TESTS=ON`
- `MR_ADM_BUILD_CAPI_BUNDLE=OFF` — 打开后生成自包含 `libmradm_capi` 共享库（target `mradm_capi_bundle`），供 GUI P/Invoke 加载
- `MR_ADM_BUILD_{SAF,LIBADM,LIBBW64,LIBEAR,EBUR128,SAMPLERATE,DRWAV}_REFERENCE_TESTS=OFF`（默认）— 只为维护对照获取/构建旧库参考工具，**不改变生产实现**；SAF 参考只用 Release；libear / libadm 参考需要 Boost
- `MR_ADM_STRICT_FP` / `MR_ADM_EAR_SCALAR_REFERENCE` / `MR_ADM_CONSISTENCY_DIAGNOSTICS` / `MR_ADM_DIAGNOSTIC_PORTABLE_RNG` — 一致性测量专用（`cmake/MRStrictFp.cmake`），不用于发行构建。生产 Scene 算术不依赖这些开关：C++ 生产入口固定用私有非融合标志（`scene-separate-v1`，ADR 0014），冻结的 Scene / Live 双耳 / Live VBAP / HOA 参考测试目标单独加 `-ffp-contract=off`（MSVC `/fp:strict`）

CI 显式使用 `MR_ADM_FLAC_PROVIDER=VENDORED` 与 `MR_ADM_OPUS_PROVIDER=VENDORED` 以消除 runner 差异（见 `docs/guides/CI.md`）。

## 架构与目标边界

项目按模块分目标，每个模块在 `CMakeLists.txt` 中是独立的 `add_library`，通过 `MacinRender::ADM*` alias 暴露。**目标边界由 ADR 0003 强制约束**，第三方类型不允许跨边界泄漏。

依赖图（PUBLIC 表示出现在公共头；PRIVATE 表示只在实现中）：

```
ADMCore (领域模型、errors、logging、options、progress、scene、capability、render、DS 矩阵与屏幕配置的 C++ 视图)
  ↑ PUBLIC for 几乎所有模块；PRIVATE: ADMDsp（Scene 空间数学）
ADMDsp              INTERFACE：src/adm_dsp/*.h 私有 FFI 头 + Rust mradm-ffi staticlib（见下文 Rust 层）
ADMMetadata         PRIVATE: mradm-ffi（mradm-adm / mradm-scene）；由只读记录视图生成 AdmScene，ADM AXML 回写
ADMIo               PRIVATE: ADMMetadata + ADMAudio + ADMRenderCommon + ADMDsp  → AdmScene（导入本身在 Rust mradm-scene）
ADMRenderCommon     PRIVATE: ADMAudio + ADMDsp；扬声器离线驱动与文件流式的适配（循环在 Rust mradm-render）、
                    给其余后端的路由/对象预处理薄包装、HpTF 控制
ADMRenderEar        PRIVATE: ADMDsp + ADMRenderCommon（适配壳与能力表；计划构建在 Rust mradm-render，布局/增益/FIR
                    设计在 mradm-ear；CLI `ear` 与兼容 backend 名 `libear` 不变）
ADMRenderVBAP       PRIVATE: ADMDsp + ADMAudio + ADMRenderCommon（适配壳、能力表与布局注册，含 Scene 流式 VBAP；
                    逻辑在 Rust mradm-render）
ADMRenderTripleBalance  PRIVATE: ADMRenderCommon + ADMAudio + nlohmann_json（数值状态在 Rust）
ADMRenderHOA        PRIVATE: ADMDsp + ADMRenderCommon（适配壳与能力表，HOA encode 在 Rust mradm-render；output_layout="hoa3"）
ADMRenderBinaural   PRIVATE: ADMDsp + ADMAudio + ADMRenderCommon（HRTF/SOFA/卷积/spreader 均在 Rust）
ADMRenderApple      macOS-only（if(APPLE)）PRIVATE: AudioToolbox/AVFoundation/CoreMedia/Foundation
                    + ADMDsp + ADMAudio + ADMRenderCommon（AUSpatialMixer 后端 + ASBR 系统空间监听 sink）
ADMRenderWindows    Windows-only（if(WIN32)）PRIVATE: Ole32 + ADMRenderCommon（ISpatialAudioClient sink）
ADMAudio            PRIVATE: dr_flac, FLAC, Opus, mradm-ffi（mradm-wav：全部 WAVE 样本读写与容器元数据）
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
mradm-dsp   #![forbid(unsafe_code)]；FFT（RustFFT 标量规划器，workspace 关闭 SIMD 特性，ADR 0015）、VBAP/MDAP、HRTF/SOFA(sofar 本地补丁)、afSTFT、去相关、OM spreader（libm 可移植几何/系数，ADR 0017）、
            卷积、HpTF（含 AutoEq 文本解析）、Meter(ebur128 crate)、重采样(rubato 本地补丁：固定标量插值，ADR 0016)、PCM 混音、EAR 后处理、Triple Balance、HOA、
            Monitor、Live VBAP/双耳、scene_math / scene_transition、post_gain（离线后处理增益）、stream_dsp（文件流式运算）、
            decimal（与 locale 无关的十进制参数解析）、timeline（秒到帧换算）
mradm-adm   #![forbid(unsafe_code)]；ADM XML（quick-xml）、内嵌 BS.2094 common definitions、场景投影与语义回写
mradm-wav   #![forbid(unsafe_code)]；只依赖 std，RIFF/RF64/BW64 读写、u64 帧定位；容器编辑（bext/ambi 追加、
            布局重写 LayoutRewriter、chunk 替换、容错 chunk 探测）
mradm-math  #![forbid(unsafe_code)]；无依赖，移植自 musl 的 sin/cos（只用 IEEE 加减乘，跨平台逐位一致；
            |x| < 2^20·π/2），供 vendored rubato 的 sinc/窗函数表使用
mradm-ear   #![forbid(unsafe_code)]；移植自 libear 2db69f8f：标准布局、nominal/effective 拓扑、Objects extent、
            DirectSpeakers、HOA AllRAD、512-tap FIR 设计（MT19937）；初等函数取自 `libm`，HOA 解码的矩阵乘积为固定顺序（三期 3.2、3.3）；
            实现版本 rust-ear-0.1.0
mradm-osc   #![forbid(unsafe_code)]；PoseBridge 协议 3 OSC 数据报解码（回环 UDP 的不可信输入）、source_id 校验、
            姿态流顺序判定；遥测 JSON 用 serde_json，接受范围与冻结的 nlohmann 实现逐字段一致
mradm-json  #![forbid(unsafe_code)]；无依赖，与 nlohmann/json 3.12.0 接受规则、数字分类和 dump(indent) 字节一致的 JSON
            （Grisu2 格式化移植自 nlohmann，MIT），供语义策略、报告与模板、DS 矩阵与屏幕配置使用（四期 4.4）
mradm-scene #![forbid(unsafe_code)]；渲染场景（四期 4.3）：扬声器布局表（标准与 Apple 几何）、输入导入（WAVE 格式、
            CHNA 映射、经 mradm-adm 投影）、声道床预设 / 目录 / 标签解析 / 场景合成，以及 `05-import` 场景检查点；
            语义策略（policy / live_policy / report）、`06-policy` 检查点、导出回写（writeback）、DS 矩阵与屏幕配置 JSON（四期 4.4）
mradm-render #![forbid(unsafe_code)]；扬声器后端与公共渲染层（四期 4.5）：DirectSpeakers / 标签 / LFE 路由（routing）、
            生效块与对象预处理（scene）、分步离线驱动（offline）与文件流式（stream）、VBAP / HOA / EAR 计划构建，
            Scene 流式 VBAP（live_vbap）；只读策略后的 Rust 渲染场景
mradm-ffi   staticlib；唯一含 unsafe 的私有 C 边界，聚合 mradm_dsp_* / mradm_adm_* / mradm_wav_* / mradm_ear_* / mradm_osc_* / mradm_scene_* / mradm_render_* 等入口
```

Rust/C++ 边界规则：

- 调用方向按 ADR 0021 分期反转（取代 ADR 0008 决策三、四）。四期保留 C++ 的 `IRenderer` / `IRenderStream` / `ILiveSceneRenderer`、`RenderService`、`SceneStreamEngine` 作适配壳（一致性工具驱动这些接口），后端逻辑与渲染循环迁入 Rust，进度、日志、取消与 PCM sink 经 C 回调接口；不新增 C++，生产 C/C++ 行数由 `scripts/quality/language-surface.py --check` 设棘轮（只减不增，删除后在同一提交 `--update`）。迁入 Rust 的代码用 `mradm-dsp` `diagnostics::process` 发出与 C++ `consistency::dump` 同名同格式的检查点，每个检查点名只归一方
- FFI 用显式长度缓冲、opaque 句柄、调用方提供的错误消息缓冲；状态码对应 `adm::ErrorCode`；句柄由分配侧配对销毁；panic 不穿过 C 边界
- C++ 侧的私有 FFI 头（`src/adm_dsp/*_ffi.h`、`src/adm_render_common/*_ffi.h`、`src/adm_metadata/adm_ffi.h`、`src/adm_audio/wav_ffi.h`、`src/adm_realtime/osc_ffi.h`，纯 C、`extern "C"` 由 `__cplusplus` 守护；C++ 包装另放 `scene_math.h` 等）是**手写**的，改 Rust 签名必须同步修改，并跑 `cmake --build build/debug --target mr_adm_ffi_header_check`（需 `cargo install cbindgen --locked --version 0.29.2`；比较导出符号、签名与 `#[repr(C)]` 字段，quality CI 同样执行）。被签名引用的 `#[repr(C)]` 类型名在 workspace 内必须唯一（cbindgen 按名字合并）。Rust 类型与这些头不得出现在 `include/adm/*`
- 共享 C ABI bundle 只导出 `c_api.h` 声明的 `adm_*`（当前 157 个；Windows 用从 `c_api.h` 生成的 `.def`）；Rust 分配器、panic 入口与私有 `mradm_*` 符号不得外泄，`scripts/quality/check-capi-exports.py <lib>` 在 ci/release 校验
- 新的 Rust 依赖必须锁定精确版本、关闭不需要的 features，并登记到许可证清单/SBOM（`scripts/quality/check-licenses.sh` 会校验 Cargo 依赖）
- 迁移批次的惯例：先保留旧 C++ 实现作对照（`tests/reference/*` + `provenance.json`，或 `MR_ADM_BUILD_*_REFERENCE_TESTS` 开关），记录验收到 `docs/architecture/RUST_*_MIGRATION.md` 与 `evidence/`；**禁止**把旧实现当运行时静默回退
- `tests/reference/` 下每个文件都登记在 `tests/reference/retention.json`（单元、测试、开关、SHA-256、未满足的退役条件），`scripts/quality/check-reference-retention.py` 校验（ci 的 version-metadata job）；冻结参考不得改算法，必须改时同步更新哈希并在迁移文档说明。退役条件（已发布 + 独立回归 + 二期确认）与步骤见 `docs/architecture/RUST_REFERENCE_RETENTION.md`

### 绝对边界（ADR 0003，按迁移现状更新）

- `include/adm/*` 不得 `#include` 任何第三方 ADM/renderer 头（Apple 框架等）或 Rust FFI 头
- libear / libadm / libbw64 / SAF / libebur128 / libsamplerate / dr_wav **已不是生产依赖**，只能出现在对应 `MR_ADM_BUILD_*_REFERENCE_TESTS` 开关下的参考测试与工具中；不得重新引入生产路径
- CLI 不直接调用任何 renderer 或 IO 库；只构造 `RenderRequest`，调用 `RenderService`
- Apple 框架（AudioToolbox、CoreAudio、CoreFoundation、AVFoundation）只允许出现在 `src/adm_audio/` 与 `src/adm_apple/`（`if(APPLE)` 门控）
- Windows COM / SpatialAudio（`spatialaudioclient.h`、`mmdeviceapi.h`、WRL）只允许出现在 `src/adm_windows/`（Windows-only 系统空间监听 sink，`if(WIN32)` 门控）；工厂返回第三方无关的 `IAudioOutputDevice`

输入路径：WAVE 容器与 ADM chunk（`mradm-wav`）+ AXML 语义（`mradm-adm`）+ CHNA 映射与声道床（`mradm-scene`，四期 4.3）→ `adm_io` / `adm_metadata` 由 Rust 场景视图生成 `adm::AdmScene` → `RenderPlan` → `IRenderer` 后端。`RenderPlan::scene` 由 `RenderService` 填好；`RenderPlan::scene_handle` 是同一份策略后场景的不透明 Rust 句柄（四期 4.4；4.5 起预览、监听与一致性工具的预导入场景也带句柄），Rust 扬声器后端只读它。**后端不得自行重新解析 ADM**，渲染循环中不持有 Rust ADM 文档句柄。

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
- 空间布局 / HOA 的 APAC 默认码率以 `7.1.4=2048 kbps` 为 12 声道基准缩放（`docs/guides/CLI_USAGE.md` 输出格式）
- HOA 输出的响度归一化可用；测量先解码到 7.1.4 AllRAD 参考播放域，LFE 不计入 LUFS 但单独计入 True Peak
- binaural 默认使用内置 KEMAR HRTF（已提交的二进制资源 `rust/crates/mradm-dsp/assets/`，`manifest.json` 记录来源与 SHA-256，构建不再从 SAF 提取）；`--sofa <path>` 支持 SimpleFreeFieldHRIR / GeneralFIR、2 receivers、48 kHz、**不重采样**
- `--renderer apple`：**macOS-only** AUSpatialMixer 后端（`src/adm_apple/`），能力见 `apple_capabilities()`，在 Linux 不编译；`mr_adm_apple_smoke_tests` 在非 macOS 跳过
- 系统空间音频监听（`monitor_system_spatial`，仅实时监听非离线）：把多声道床交 OS 做 HRTF。**macOS** 经 `AVSampleBufferAudioRenderer`（`src/adm_apple/avsamplebuffer_device.mm`，含动态头追踪）；**Windows** 经 `ISpatialAudioClient`（`src/adm_windows/spatialaudioclient_device.cpp`，Windows Sonic / Dolby Atmos / DTS 头戴，**静态空间化无 OS 头追**，需声音设置启用某空间格式否则返回 `unsupported`）。布局白名单各自由 `apple_layouts` / `windows_layouts` 定义，经 capabilities JSON 的 `system_spatial_layouts` 字段统一暴露给 GUI（**唯一权威源，勿在 GUI 硬编码**）。sink 选择在 `monitor_session.cpp::make_monitor_device`
- FLAC 解码在 `flac_io.cpp` 中定义 `DR_FLAC_IMPLEMENTATION`（dr_flac），编码用 `libFLAC`；dr_wav 只在 `MR_ADM_BUILD_DRWAV_REFERENCE_TESTS` 对照中使用
- WAV / BW64 IO 是 64-bit clean（支持 >4GB 母版与输出）：**所有 WAV 样本读写经 Rust `mradm-wav`**（C++ 包装在 `src/adm_audio/wav_backend.h`；`FloatWavReader`/`FloatWavWriter`/`RenderInputReader` 均基于它，只接受 PCM16/24/32 与 float32）：**f32 WAV 固定写 RF64**（流式写无法预知总大小，统一用 `ds64` 承载真实大小，小文件也是 RF64），`FloatWavWriter` 覆盖已有文件，析构尽力收尾、先写后改名的路径须显式 `finish()`；整数输出默认先写 RIFF、需要 64 位长度时升级 BW64，整数转换写排他临时文件、成功 `finish()` 后才安装，失败/取消保留原文件；reader 以 u64 帧 `seek_frame` 定位（不再有 libbw64 的 2^31 帧上限）。容器元数据也全部经 `mradm-wav`（见 `RUST_WAV_CONTAINER_MIGRATION.md`）：`write_wav_metadata`（bext/ambi 追加，按 RIFF/RF64/BW64 更新顶层 size 或 `ds64.bw64Size`）、`finalize_wav_layout`（声道置换/掩码/ADM chunk/容器选择，C++ 按 2048 帧驱动 `step` 并负责取消、进度与备份替换）、ADM 导入的 AXML/CHNA 读取、export 的 axml 替换和 channel-bed 路由探测；C++ 不再手写 RIFF chunk 解析，冻结的旧实现在 `tests/reference/wav_container/` 由 `mr_adm_wav_container_tests` 逐字节对照。Windows 私有 FFI 路径先按当前进程代码页把原生窄路径转 UTF-8，**不要**靠字节是否为合法 UTF-8 猜编码。回归守卫用稀疏文件造 >4GB / 跨 2^31 帧 fixture（`core_smoke_test` / `render_trim_fixture_test`，不真烧盘）；真实 >4GiB 写入测试默认忽略，需设 `MRADM_WAV_LARGE_TEST_DIR`

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
- Rust：edition 2024、默认 rustfmt；算法 crate（`mradm-dsp` / `mradm-adm` / `mradm-wav` / `mradm-ear`）保持 `#![forbid(unsafe_code)]`，unsafe 只写在 `mradm-ffi`；准备阶段之后的处理调用不应分配内存（FFT、运动控制、spreader 等已有分配计数测试约束）；随机状态归实例所有，不用全局 RNG

## 测试约束

- 测试不依赖私有音频素材；fixture 在运行时由代码生成（见 `tests/unit/*_fixture_test.cpp`）
- CLI smoke 测试通过 CMake 注入二进制路径：`MRADM_EXE_PATH` 编译期 define（指向 `$<TARGET_FILE:mradm_exe>`）
- 跨平台测试在 Linux 自动跳过 macOS-only 功能（APAC、CoreAudio layout）；不要把 Apple-only 路径放进默认 ctest 断言
- 行为变更必须考虑回归基线：解析摘要、布局摘要、输出声道、时长、响度、True Peak、音频误差阈值

## CI 与发布

- `.github/workflows/ci.yml` — PR/push main：macOS + Linux + Windows debug（均安装 Rust 1.98.0，带 Cargo 缓存），FLAC/Opus 均 vendored；生产构建无需 Boost/vcpkg；macOS/Linux 构建后跑 `check-licenses.sh --build-dir build/debug`，新增依赖未登记会让 PR 失败；另有 Linux ARM64 Release job（`ubuntu-24.04-arm`，显式 `MR_ADM_STRICT_FP=OFF`）守护 Scene 算术规则在优化构建下不漂移
- `.github/workflows/consistency.yml` — push main / manual：Rust 二期三平台 A/B 采集；门禁输入完整性、同进程/新进程重复性、诊断无扰动，以及 `scripts/consistency/phase2-gates.json` 列出的跨平台位相等项（当前为 FFT 及其 f32/f64 twiddle、EAR 去相关 FIR、可移植三角函数、重采样、OM/spreader、HRTF、SOFA、HpTF 设计内核、生产计量的系数、True Peak 与响度、计量验收节目、离线后处理增益、文件流式与监听运算、EAR 增益、HOA 编解码、Triple Balance 声像、ADM 导入换算、十进制参数解析、秒到帧换算、整数 WAVE 量化、EAR 布局、Scene 空间数学、`room-binaural` 准备内核及全部 283 个 PCM 用例，含 28 个文件流式重放）；全部 237 个内核文件设为门禁
- `.github/workflows/quality.yml` — 所有触发跑 Rust fmt/clippy 与 FFI 头校验；PR 跑 `check-changed.sh`；push main / manual full 跑 `check-all.sh`；只在 macOS
- `.github/workflows/windows-bringup.yml` — 手动触发的 Windows MSVC Release 探针构建
- `.github/workflows/release.yml` — tag `v*` 或手动触发：macOS CLI `.tar.gz`、Linux CLI `.AppImage`、Windows CLI `.zip`，外加 macOS/Windows GUI 包（`MacinRender-Gui-*`，经 `scripts/release/package-*.sh` + smoke 脚本）
- `.github/workflows/reference-tests.yml` — 手动触发：Release 逐个打开（SAF 在 macOS，其余 Linux） `MR_ADM_BUILD_*_REFERENCE_TESTS` 跑第三方对照，防止默认 OFF 的参考腐烂
- `.github/workflows/iamf-bridge-prebuild.yml` — 预构建 AOM iamf-tools bridge SDK；`cache-maintenance.yml` — FetchContent/ccache 缓存维护

详见 `docs/guides/CI.md`。

## 关键文档索引

- `docs/architecture/RUST_INVERSION_ROADMAP.md` — 逆转为 Rust 项目的总路线（ADR 0021）：终态、模块去向与期次、C/C++ 规模棘轮、oracle 驱动面迁移；`RUST_INVERSION_BASELINE.md` — 四期 4.1 记录（场景检查点编码、规模基线）；`RUST_INVERSION_COVERAGE.md` — 四期 4.2 记录（策略矩阵、平滑与后置增益、`room-binaural` 用例与合成房间包）；`RUST_INVERSION_SCENE.md` — 四期 4.3 记录（Rust 渲染场景：输入导入、声道床、布局表与 `05-import` 检查点）；`RUST_INVERSION_POLICY.md` — 四期 4.4 记录（语义策略、Scene 流式应用、导出回写、DS 矩阵与屏幕 JSON、`mradm-json`、发布说明）；`RUST_INVERSION_SPEAKERS.md` — 四期 4.5 记录（`mradm-render`：路由、离线驱动与文件流式、VBAP / HOA / EAR 与 Scene 流式 VBAP、发布说明）
- `docs/architecture/CPP_ADM_PLATFORM_REWRITE.md` — 历史规划（已被 ADR 0021 取代）
- `docs/architecture/PCM_CONSISTENCY_AUDIT.md` — 到 PCM 写出为止的 C++/Rust 数值审计、唯一的本轮迁移路线与逐切片验证要求；一期 Scene 流式渲染及其实际共享依赖；`RUST_SCENE_RESIDUAL_MIGRATION.md` — 一期 1.2 记录；`RUST_SCENE_PORTABLE_MATH.md` — 一期 1.3 记录；`RUST_SCENE_OUTPUT_DSP.md` — 一期 1.4 记录；`RUST_SCENE_TIMING.md` — 一期 1.5 记录；`RUST_SCENE_RELEASE_ACCEPTANCE.md` — 一期 1.6 三平台验收与结项；`RUST_METER_TRUE_PEAK.md` — 二期 2.1 记录；`RUST_METER_LOUDNESS.md` — 二期 2.2 记录；`RUST_POST_GAIN.md` — 二期 2.3 记录；`RUST_METER_ACCEPTANCE.md` — 二期 2.4 计量与归一化验收；`RUST_FILE_STREAM_BINAURAL.md` — 三期 3.1 记录；`RUST_EAR_PORTABLE_MATH.md` — 三期 3.2 记录；`RUST_HOA_PORTABLE_MATH.md` — 三期 3.3 记录；`RUST_TRIPLE_BALANCE_PORTABLE_MATH.md` — 三期 3.4 记录；`RUST_PARAMETER_ENTRY.md` — 三期 3.5 记录；`RUST_FINAL_PCM.md` — 三期 3.6 记录与三期结项
- `docs/architecture/ADM_FEATURE_COVERAGE.md` — ADM 特性覆盖审计
- `docs/architecture/ADM_APPLE_BACKEND.md` — macOS AUSpatialMixer 后端 + ASBR 系统空间监听 sink
- `docs/architecture/ADM_WINDOWS_SYSTEM_SPATIAL.md` — Windows ISpatialAudioClient 系统空间监听 sink（静态床/能力实测/切换恢复）
- `docs/architecture/hptf-eq.md` — HpTF 耳机补偿（v1.38 内存参数接口、AutoEq ParametricEQ，实时监听专用，设备绑定）
- `docs/adr/0001` C++20 标准 | `0002` C++-first，Rust-later | `0003` 自有领域模型与后端边界 | `0004` 第三方依赖管理 | `0005` 错误处理模型 | `0006` CLI11 选择 | `0007` C ABI 稳定性 | `0008` Rust 落地与 SAF 替换 | `0009` 头追踪输入边界 | `0010` Rust Meter | `0011` Rust 固定采样率转换 | `0012` Rust ADM 元数据与 libadm 参考边界 | `0013` Rust EAR 与 libear 生产依赖移除 | `0014` Scene 统一乘加舍入规则 | `0015` RustFFT 固定标量路径 | `0016` 重采样固定标量插值与可移植三角函数 | `0017` OM spreader 固定分组预算与可移植数学函数 | `0018` Scene 流式渲染的数值与重放规则（一期） | `0019` 房间渲染按声源自身事件网格更新滤波器 | `0020` 房间渲染的精确与实时档位 | `0021` 项目逆转为 Rust 主导（取代 0002 与 0008 决策三、四）
- `docs/architecture/SCENE_ARITHMETIC_POLICY.md` — `scene-separate-v1` 的验证证据（二期第一个切片）
- `docs/architecture/RUST_COVERAGE_EXTENSION.md` — 二期后的 96 kHz Scene、96/192 kHz 重采样、HRTF 与退化 OM 覆盖；该批验收为 118 份 PCM / 133 份内核测量
- `docs/architecture/RUST_PHASE2_CLOSEOUT.md` — 二期结项、限定矩阵、维护门禁与后续边界；`RUST_PHASE2_PERFORMANCE.md` — 保持位模式的性能回收
- `docs/architecture/RUST_PHASE2_BASELINE.md` — 二期三平台基线、回放与分歧定位工具；`RUST_PHASE2_FFT.md` — FFT 收敛切片（标量路径、twiddle 证据、位相等门禁、性能代价）；`RUST_PHASE2_RESAMPLER.md` — 重采样收敛切片（rubato 补丁、mradm-math）；`RUST_PHASE2_SPREADER.md` — OM spreader 收敛切片（固定分组预算、libm-force）
- `docs/architecture/RUST_REFERENCE_RETENTION.md` — 迁移参考实现的登记、冻结校验与退役条件
- `docs/architecture/RUST_SAF_REPLACEMENT_ROADMAP.md` — Rust 一期总览与二期已验收范围；各批次验收见 `docs/architecture/RUST_*_MIGRATION.md`（ADM、BW64、dr_wav、EAR、EAR post、Live VBAP、Scene numeric、HOA、Monitor 等）及 `docs/architecture/evidence/`
- `docs/guides/QUALITY.md` — 质量工具与策略
- `docs/guides/CI.md` — CI 设计与边界
- `docs/THIRD_PARTY_LICENSES.md` — 第三方许可证与发行边界
