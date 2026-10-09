# 麦渲峰 ADM Core 文档

「麦渲峰」是 MacinRender 的正式中文名；技术标识继续使用 `MacinRender`。

## 渲染流程命名

文档、代码注释与诊断统一按输入方式和调用契约命名：

| 中文名称 | 英文名称 | 接口与职责 |
|---|---|---|
| 离线渲染 | offline rendering | `IRenderer::render_window`：根据文件与 ADM 场景渲染整段或指定窗口，并写出音频文件。 |
| 文件流式渲染 | file streaming | `IRenderer::open_stream` → `IRenderStream`：内部读取文件，通过 `process()` 分块输出 PCM，跨调用保存渲染状态；双耳实现为 `BinauralStream`。 |
| Scene 流式渲染 | Scene streaming | `SceneStreamEngine` / `ILiveSceneRenderer`：调用方提交音频块与对象元数据，再拉取渲染后的 PCM。 |

三者都是受支持的处理流程，名称不表示实现先后、替代关系或弃用状态。实时监听是用途，
可以使用文件流式渲染或 Scene 流式渲染；Scene 流式渲染也可用于受控回放验证。
各流程的支持范围、状态推进及数值契约分别定义，不能仅因复用 DSP 内核就视为等价。
迁移对照应明确写出“参考实现”“Rust 实现”或具体版本／提交。

## 架构文档

- [C++ ADM 渲染平台化重构规划](architecture/CPP_ADM_PLATFORM_REWRITE.md)
- [PCM 一致性审计与 Rust 迁移路线](architecture/PCM_CONSISTENCY_AUDIT.md) — 剩余数值运算与本轮分期的统一入口，一期为 Scene 流式渲染及其实际共享依赖
- [ADM 特性覆盖审计](architecture/ADM_FEATURE_COVERAGE.md)
- [adm_apple 后端实现说明（AUSpatialMixer）](architecture/ADM_APPLE_BACKEND.md)
- [adm_windows：系统空间音频监听 sink（ISpatialAudioClient）](architecture/ADM_WINDOWS_SYSTEM_SPATIAL.md)
- [实时监听引擎](architecture/REALTIME_MONITORING.md)
- [语义编辑器 GUI](architecture/SEMANTIC_EDITOR_GUI.md)
- [头部追踪与 OSC 跨仓库设计（原生接收接口／GUI 待适配）](architecture/HEAD_TRACKING_OSC.md)
- [原生 OSC 头部姿态接收接口（C++／C ABI v1.40）](architecture/OSC_HEAD_TRACKING_API.md)
- [Scene 算术规则统一](architecture/SCENE_ARITHMETIC_POLICY.md)
- [Rust 二期结项：验收范围与维护契约](architecture/RUST_PHASE2_CLOSEOUT.md)
- [Rust 后续覆盖：高采样率、HRTF 与退化 OM](architecture/RUST_COVERAGE_EXTENSION.md)
- [dr_flac 替换准备、风险验证与优先级](architecture/RUST_FLAC_DECODER_PLAN.md)
- [Rust 二期基线与分歧定位](architecture/RUST_PHASE2_BASELINE.md)
- [Rust 二期 FFT 收敛](architecture/RUST_PHASE2_FFT.md)
- [Rust 二期重采样收敛](architecture/RUST_PHASE2_RESAMPLER.md)
- [Rust 二期 OM spreader 收敛](architecture/RUST_PHASE2_SPREADER.md)
- [Rust 二期 FFT 性能回收](architecture/RUST_PHASE2_PERFORMANCE.md)
- [Rust 落地与 SAF 替换路线图](architecture/RUST_SAF_REPLACEMENT_ROADMAP.md)
- [迁移参考实现的登记与退役条件](architecture/RUST_REFERENCE_RETENTION.md)

## Rust 迁移验收记录

- [Rust ADM 元数据迁移](architecture/RUST_ADM_MIGRATION.md)
- [Rust 双耳卷积与滤波过渡迁移](architecture/RUST_BINAURAL_DSP_MIGRATION.md)
- [Rust 整数 WAVE/BW64 迁移](architecture/RUST_BW64_MIGRATION.md)
- [Rust 浮点 WAVE 迁移（移除 dr_wav）](architecture/RUST_DR_WAV_MIGRATION.md)
- [Rust EAR 算法迁移](architecture/RUST_EAR_MIGRATION.md)
- [Rust EAR 后处理与连续短块尾音修复](architecture/RUST_EAR_POST_MIGRATION.md)
- [Rust HOA 编码、状态与计量前处理迁移](architecture/RUST_HOA_MIGRATION.md)
- [Rust HpTF 耳机补偿 DSP 迁移](architecture/RUST_HPTF_MIGRATION.md)
- [Rust HpTF ParametricEQ 文本解析迁移](architecture/RUST_HPTF_PARSE_MIGRATION.md)
- [Rust HRTF 插值与频域状态迁移](architecture/RUST_HRTF_MIGRATION.md)
- [Rust Live VBAP 混音与独立渐变迁移](architecture/RUST_LIVE_VBAP_MIGRATION.md)
- [Rust Meter 迁移记录](architecture/RUST_METER_MIGRATION.md)
- [Rust Monitor 淡化、seek 过渡与 Peak/RMS 迁移](architecture/RUST_MONITOR_DSP_MIGRATION.md)
- [Rust PoseBridge OSC 协议解析迁移](architecture/RUST_OSC_PROTOCOL_MIGRATION.md)
- [Rust 峰值保护与实时增益迁移](architecture/RUST_OUTPUT_DSP_MIGRATION.md)
- [Rust 共享 PCM 混音与状态迁移](architecture/RUST_PCM_MIX_MIGRATION.md)
- [Rust 重采样迁移](architecture/RUST_RESAMPLER_MIGRATION.md)
- [Rust Scene 流式渲染的过渡、空间数学与双耳迁移](architecture/RUST_SCENE_NUMERIC_MIGRATION.md)
- [Rust Triple Balance 数值状态迁移](architecture/RUST_TRIPLE_BALANCE_MIGRATION.md)
- [Rust WAVE 容器元数据迁移](architecture/RUST_WAV_CONTAINER_MIGRATION.md)

## 架构决策记录

- [ADR 0001：新 C++ 核心采用 C++20](adr/0001-cpp-standard.md)
- [ADR 0002：先建立 C++ 地基，后续渐进引入 Rust](adr/0002-cpp-first-rust-later.md)
- [ADR 0003：自有 ADM 领域模型与后端边界](adr/0003-owned-domain-model-and-backend-boundaries.md)
- [ADR 0004：第三方依赖管理策略](adr/0004-third-party-dependency-management.md)
- [ADR 0005：错误处理模型与 ABI 错误码翻译](adr/0005-error-handling-model.md)
- [ADR 0006：CLI 参数解析库采用 CLI11](adr/0006-cli-argument-library.md)
- [ADR 0007：C ABI 稳定性承诺与版本策略](adr/0007-c-abi-stability-policy.md)
- [ADR 0008：Rust 落地方向与 SAF 按模块替换](adr/0008-rust-entry-and-saf-replacement.md)
- [ADR 0009：外置头部追踪的独立仓库与 OSC 接入边界](adr/0009-head-tracking-input-boundary.md)
- [ADR 0010：以统一 Rust Meter 替换 C 计量库](adr/0010-rust-loudness-meter.md)
- [ADR 0011：固定采样率转换迁入 Rust](adr/0011-rust-fixed-rate-resampling.md)
- [ADR 0012：Rust ADM 元数据与 libadm 参考边界](adr/0012-rust-adm-metadata.md)
- [ADR 0013：Rust EAR 算法与生产依赖移除](adr/0013-rust-ear.md)
- [ADR 0014：统一 Scene 的乘加舍入规则](adr/0014-scene-arithmetic-policy.md)
- [ADR 0015：RustFFT 固定使用标量路径](adr/0015-rust-fft-scalar-path.md)
- [ADR 0016：重采样固定标量插值与可移植三角函数](adr/0016-deterministic-resampling.md)
- [ADR 0017：OM spreader 固定分组预算与可移植数学函数](adr/0017-deterministic-spreader.md)

## 使用指南

- [CLI 用法指南](guides/CLI_USAGE.md)（[English](guides/CLI_USAGE.en.md)）
- [普通多声道输入语义](guides/CHANNEL_BED_INPUT.md)（[English](guides/CHANNEL_BED_INPUT.en.md)）
- [发行包](guides/BINARY_RELEASE.md)（[English](guides/BINARY_RELEASE.en.md)）
- [CI 设计草案](guides/CI.md)
- [质量工具配置](guides/QUALITY.md)
- [第三方许可证与发行边界](THIRD_PARTY_LICENSES.md)

## 维护说明

- 架构规划放在 `architecture/`。
- 已接受或废弃的重要技术决策放在 `adr/`。
- 使用指南放在 `guides/`。
- 新增依赖、语言路线、公共 API 和后端边界变化都应补充 ADR。
