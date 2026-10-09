# 麦渲峰 ADM Core

[English](README.en.md) | 中文

ITU-R BS.2076 ADM 空间音频渲染核心 · C++20 + Rust 2024 · Rust 1.98 · 稳定 C ABI v1 · 三平台逐位一致门禁

## 这是什么

ADM（Audio Definition Model，ITU-R BS.2076）用 XML 元数据描述对象、直达扬声器与 HOA 等沉浸式音频内容，
通常以 ADM BWF / BW64 文件交付。麦渲峰 ADM Core（英文名：MacinRender ADM Core）读取 ADM 场景或普通多声道
WAVE，**渲染**为多声道扬声器、HOA 或 HRTF 双耳信号，并封装为 WAV / CAF / FLAC / Opus MKA / IAMF / APAC 等交付格式。
除离线渲染外，它还提供带后端热切换与头部追踪的实时监听链路。

项目提供三种入口：`mradm` 命令行工具、稳定 C ABI 库，以及基于该 C ABI 的 Avalonia 桌面 GUI。

本项目**不是** ADM 母版制作工具：语义策略只在渲染时改写语义，`export` 写回新 ADM 时复用源 PCM。音频编码器本身也不在
本项目内实现：FLAC / Opus / IAMF / APAC 分别经 libFLAC、libopus、AOM iamf-tools 与 AudioToolbox。项目不涉及
Dolby / Apple 产品认证。

> **名称约定：**「麦渲峰」是 MacinRender 的正式中文名。英文品牌名以及仓库、包名、CMake 目标、命名空间、
> 可执行文件等技术标识继续使用 `MacinRender`。

本项目与 Dolby Laboratories、Apple Inc. 不存在隶属、赞助或认可关系。Dolby Atmos、Apple、AirPods 等是其各自权利人
的商标；文中名称仅用于兼容性说明。

## 特性

- **ADM 与声道床输入**：ADM BWF / BW64 / RF64 场景导入；普通多声道 WAVE 按 channel mask、预设布局或受控自定义标签合成 DirectSpeakers 场景
- **多后端渲染**：EAR（BS.2127）、VBAP、Triple Balance 房间坐标渲染、HOA3 编码、HRTF 双耳，以及 macOS 上的 Apple AUSpatialMixer
- **ADM 语义**：Objects / DirectSpeakers / HOA，含时间块、增益、插值、diffuse、extent、channelLock、objectDivergence
- **语义策略**：用 JSON 在渲染时改写对象语义、写出生效语义快照，或导出应用策略后的新 ADM BWF
- **输出后处理**：响度归一化、True Peak 限制、位深转换；HOA 经 7.1.4 AllRAD 参考解码计量
- **多格式交付**：WAV / CAF / FLAC / Opus MKA / IAMF / APAC，WAV 输出与母版输入均支持超过 4 GB
- **实时监听**：后端/布局/设备热切换、头部追踪（OSC、AirPods）、HpTF 耳机补偿、系统空间音频（macOS / Windows）
- **桌面工作台**：批量渲染、逐对象语义编辑、实时监听与空间可视化
- **稳定 C ABI**：自 1.0 起向后二进制兼容，结构化进度回调，Scene 流式渲染输入接口
- **跨平台逐位一致**：一致性 CI 的 78 个 PCM 用例在 macOS arm64 / Linux x64 / Windows x64 上逐位相同，并设为门禁
- **Rust 数值核心**：DSP、EAR、ADM XML 与 WAVE I/O 由 Rust 实现；项目自有算法 crate 禁用 `unsafe`，`unsafe` 只出现在私有 C 边界 `mradm-ffi`

## 快速开始

### 前置条件

- C++20 编译器（Clang / GCC / MSVC）与 CMake ≥ 3.24
- Rust 1.98.0（含 rustfmt / clippy，见 `rust/rust-toolchain.toml`）

```bash
rustup toolchain install 1.98.0 --profile minimal --component rustfmt --component clippy
```

C/C++ 依赖默认由 CMake `FetchContent` 获取，Rust 依赖使用提交的 `Cargo.lock`；生产构建不需要 Boost、vcpkg、SAF 或
OpenBLAS。APAC 编码与 Apple 后端只在 macOS 上可用。

也可以从 [GitHub Release](https://github.com/SakuzyPeng/MacinRender-ADM-Core/releases) 下载预编译的 CLI 与 GUI：
macOS arm64 与 Windows x64，产物与 SHA-256 校验见[发行包](docs/guides/BINARY_RELEASE.md)。

### 构建与测试

```bash
cmake --preset debug
cmake --build --preset debug
ctest --preset debug --output-on-failure
```

实际渲染使用 Release 构建：

```bash
cmake --preset release
cmake --build --preset release
```

### 查看场景与能力

```bash
./build/release/mradm inspect input.wav      # ADM / 声道床场景元数据
./build/release/mradm backends               # 渲染后端、能力与支持布局
./build/release/mradm formats                # 输出格式、可用性与约束
./build/release/mradm layouts --format wav   # 某输出格式的最终声道顺序
```

### 常用构建选项

| 选项 | 说明 | 默认 |
|---|---|---|
| `MR_ADM_CORE_FETCH_DEPS` | 关闭后使用系统依赖，需另备 Corrosion 0.6.1 与 Cargo 依赖缓存 | `ON` |
| `MR_ADM_FLAC_PROVIDER` / `MR_ADM_OPUS_PROVIDER` | `AUTO`（Release 用 vendored static，Debug 优先系统库）/ `VENDORED` / `SYSTEM` | `AUTO` |
| `MR_ADM_ENABLE_SOFA` | 双耳后端的用户 SOFA HRIR 支持 | `ON` |
| `MR_ADM_ENABLE_IAMF` | IAMF 编码，需 `MR_ADM_IAMF_AOM_ROOT` 指向官方 AOM iamf-tools bridge SDK | `OFF` |
| `MR_ADM_BUILD_CAPI_BUNDLE` | 生成自包含 `libmradm_capi` 共享库，供 GUI 加载 | `OFF` |
| `MR_ADM_BUILD_*_REFERENCE_TESTS` | 构建旧库参考对照（SAF、libear、libadm 等），不改变生产实现 | `OFF` |

## 基础用法

以下是最常用的命令。全部子命令、后端、输出格式与布局、选项和语义策略见 [CLI 用法指南](docs/guides/CLI_USAGE.md)。

**双耳 → FLAC**——默认输出语义为 `binaural`，可用 `--sofa` 换用户 HRIR：

```bash
./build/release/mradm render -i input.wav -o out.binaural.flac --renderer saf-binaural
```

**扬声器布局**——≤ 8 声道无高度布局用 FLAC，高度布局用 Opus MKA（要求 48 kHz；macOS 可用 APAC `.m4a`）：

```bash
./build/release/mradm render -i input.wav -o out.5_1.flac --renderer ear --output-layout 5.1
./build/release/mradm render -i input.wav -o out.7_1_4.mka --renderer ear --output-layout 7.1.4
```

**HOA 三阶**：

```bash
./build/release/mradm render -i input.wav -o out.hoa3.mka --renderer hoa --output-layout hoa3
```

**普通多声道输入**——按预设或自定义标签解释 WAVE 声道：

```bash
./build/release/mradm render -i bed.wav -o bed_714.mka --input-layout 5.1 --renderer ear --output-layout 7.1.4
```

**语义策略**——生成中性模板，编辑后应用，并写出生效语义快照：

```bash
./build/release/mradm inspect input.wav --write-semantic-policy-template policy.json
./build/release/mradm render -i input.wav -o out.flac --semantic-policy policy.json \
  --write-semantic-report report.json
```

**C ABI**——`include/adm/c_api.h` 是稳定 v1 接口：文件渲染优先使用带结构化进度的 `adm_render_file_ex2`，
文件流式渲染的监听使用 `adm_monitor_*` 家族，外部解码器可经 Scene 流式渲染接口逐对象提交 PCM 与空间元数据。兼容策略见
[ADR 0007](docs/adr/0007-c-abi-stability-policy.md)。

## 图形界面

麦渲峰 GUI 是基于 Avalonia / .NET NativeAOT 的桌面工作台，通过稳定 C ABI 调用与 CLI 相同的渲染核心；后端、布局、
格式和平台能力均直接使用 core 的查询结果。发行包覆盖 macOS arm64 和 Windows x64。

| 工作流 | 当前能力 |
|---|---|
| 批量渲染 | 添加文件或文件夹，选择后端、布局、编码与容器，查看结构化进度、日志并取消任务 |
| 语义编辑 | 载入单个 ADM，逐对象编辑 gain、diffuse、extent、divergence 和头追踪参与状态 |
| 实时监听 | 边播放边比较语义覆盖，切换后端、布局与输出设备，使用自定义 SOFA HRIR；双耳监听支持鼠标、触控板与键盘控制 yaw / pitch / roll |
| 空间可视化 | 对象位置、轨迹与逐声道电平表；空间角色支持自定义 64×64 PNG 皮肤 |
| 检查与导出 | 导出生效 ADM，保留源音频并写回支持的语义修改 |

界面支持中文 / English 与深浅色主题；macOS 额外提供 Apple AUSpatialMixer、APAC 与 AirPods 头部追踪。设计见
[语义编辑器 GUI](docs/architecture/SEMANTIC_EDITOR_GUI.md) 与[实时监听引擎](docs/architecture/REALTIME_MONITORING.md)。

## 项目结构

```text
mradm (CLI) ──→ ADMEngine ──→ RenderService / monitor_session
GUI (C#) ──→ ADMCAPI ──→ ADMEngine
ADMEngine ──→ ADMRendererFactory ──→ ADMRender{Ear,VBAP,TripleBalance,HOA,Binaural,Apple}
          ──→ ADMRealtime / ADMIo / ADMMetadata / ADMAudio / ADMPeak / ADMLoudness
C++ 模块 ──→ ADMDsp ──→ mradm-ffi（Rust staticlib）──→ mradm-dsp / mradm-ear / mradm-adm / mradm-wav / mradm-osc
所有模块 ──→ ADMCore（领域模型、错误、选项、能力）
```

| 组件 | 职责 |
|---|---|
| `ADMCore` | 自有 ADM 领域模型（`AdmScene`）、错误、日志、选项、能力与语义策略 |
| `ADMMetadata` / `ADMIo` | ADM AXML 解析与回写、声道床场景合成，产出 `AdmScene` |
| `ADMRender*` | EAR、VBAP、Triple Balance、HOA、HRTF 双耳与 Apple AUSpatialMixer 后端 |
| `ADMRendererFactory` | 离线渲染与实时监听共用的后端选择 |
| `ADMAudio` / `ADMPeak` / `ADMLoudness` | 容器编码与元数据、响度与 True Peak |
| `ADMRealtime` | 实时监听：MonitorEngine、SceneStream、设备输出与头部追踪 |
| `ADMEngine` / `ADMCAPI` | `RenderService` 编排与稳定 C ABI |
| `rust/crates/mradm-dsp` | FFT、VBAP、HRTF/SOFA、卷积、OM spreader、重采样、计量、HOA、Monitor 等数值 DSP |
| `rust/crates/mradm-ear` | 移植自 libear 的布局、增益与 FIR 设计 |
| `rust/crates/mradm-adm` / `mradm-wav` | ADM XML 元数据；WAVE / RF64 / BW64 读写与容器编辑 |
| `rust/crates/mradm-math` | 跨平台逐位一致的可移植 sin/cos |
| `rust/crates/mradm-osc` | PoseBridge OSC 头追踪数据报解码（回环 UDP 输入） |
| `rust/crates/mradm-ffi` | 唯一含 `unsafe` 的私有 C 边界 |
| `gui/MacinRender.Gui` | Avalonia NativeAOT 桌面 GUI，经 P/Invoke 调用 C ABI |

模块边界与第三方类型隔离见 [ADR 0003](docs/adr/0003-owned-domain-model-and-backend-boundaries.md)。

## 数据流

```text
ADM BWF / BW64 / RF64，或普通多声道 WAVE
    → 容器与 chunk 读取（mradm-wav）
    → ADM AXML 解析（mradm-adm）或声道床合成
    → AdmScene  ← 可选语义策略改写
    → RenderPlan → 渲染后端（EAR / VBAP / Triple Balance / HOA / 双耳 / Apple）
    → 后处理（响度归一化、True Peak 限制、位深转换）
    → 容器编码（WAV / CAF / FLAC / Opus MKA / IAMF / APAC）
```

文件流式渲染与离线渲染共用后端选择；Scene 流式渲染接收调用方提交的 PCM 与对象元数据。
两种流式渲染都可用于实时监听。名称、接口与边界见[渲染流程命名](docs/README.md#渲染流程命名)。

## 当前进度

| 方向 | 状态 | 摘要 |
|---|---|---|
| 渲染后端 | ✅ | EAR、VBAP、Triple Balance、HOA3、HRTF 双耳、Apple AUSpatialMixer（macOS） |
| 输出格式 | ✅ | WAV / CAF / FLAC / Opus MKA 全平台；APAC 仅 macOS |
| IAMF | 🚧 | 需官方 AOM bridge SDK；布局开放到 7.1.4，9.1.6 因播放器兼容性暂缓 |
| 实时监听 | ✅ | 热切换、头部追踪、HpTF、系统空间音频（Windows 为静态空间化，无系统头追） |
| C ABI | ✅ | 稳定 v1（当前 1.43），`struct_size` 扩展与 deprecation 维护兼容 |
| 桌面 GUI | ✅ | macOS arm64 / Windows x64 发行包 |
| Rust 一期迁移 | ✅ | SAF、libear、libadm、libbw64、dr_wav、libebur128、libsamplerate 已移出生产路径，仅保留为参考对照 |
| Rust 二期一致性 | ✅ 已结项 | 原 78 项矩阵已[结项](docs/architecture/RUST_PHASE2_CLOSEOUT.md)并完成首批性能回收；[后续覆盖](docs/architecture/RUST_COVERAGE_EXTENSION.md)扩展至 118/118 PCM 三平台逐位相同 |

迁移范围与验收见 [Rust 落地与 SAF 替换路线图](docs/architecture/RUST_SAF_REPLACEMENT_ROADMAP.md)，ADM 特性覆盖见
[ADM 特性覆盖审计](docs/architecture/ADM_FEATURE_COVERAGE.md)。

## 设计原则

1. 自有 ADM 领域模型，第三方类型不跨模块边界；渲染后端不重新解析 ADM。
2. 公共 API 用 `Result` 表达可恢复错误，C ABI 导出函数全部 `noexcept` 并翻译为错误码。
3. C ABI 自 1.0 起向后二进制兼容；结构体扩展一律走 `struct_size`。
4. 数值实现集中在 Rust，调用方向固定为 C++ 调 Rust；算法 crate 禁用 `unsafe`，准备阶段后的处理不分配内存。
5. 替换旧库时保留冻结的参考实现做对照，禁止把旧实现当作运行时静默回退。
6. 跨平台逐位一致由门禁持续验证；门禁失败时修正实现，而不是放宽门禁。
7. 测试 fixture 在运行时生成，仓库不提交私有或不可再分发素材。
8. GUI 的后端、格式与布局可用性一律来自 core 能力查询，不硬编码支持表。

## 文档

| 文档 | 说明 |
|---|---|
| [CLI 用法指南](docs/guides/CLI_USAGE.md) | 子命令、后端、输出格式与布局、选项、语义策略 |
| [普通多声道输入](docs/guides/CHANNEL_BED_INPUT.md) | 预设顺序、标签几何、channel mask 与约束 |
| [发行包](docs/guides/BINARY_RELEASE.md) | 发行产物、包内容、SHA-256 与 GUI 启动 |
| [平台化重构规划](docs/architecture/CPP_ADM_PLATFORM_REWRITE.md) | 模块边界与长期方向 |
| [ADM 特性覆盖审计](docs/architecture/ADM_FEATURE_COVERAGE.md) | ADM 语义的支持范围 |
| [实时监听引擎](docs/architecture/REALTIME_MONITORING.md) | 监听链路、后端切换与设备输出 |
| [Rust 路线图](docs/architecture/RUST_SAF_REPLACEMENT_ROADMAP.md) / [二期结项](docs/architecture/RUST_PHASE2_CLOSEOUT.md) | 迁移、已验收矩阵、证据与后续边界 |
| [质量工具](docs/guides/QUALITY.md) / [CI 指南](docs/guides/CI.md) | 本地检查、CI 工作流与门禁 |
| [第三方许可证](docs/THIRD_PARTY_LICENSES.md) | 依赖许可证与发行边界 |
| [文档索引](docs/README.md) / [ADR](docs/adr/) | 全部架构文档与 17 份架构决策记录 |

## 许可证

本项目源码采用 **MIT License**，以仓库根目录 [LICENSE](LICENSE) 为准。

当前默认构建依赖与 MIT 源码许可证兼容；二进制发行包附带第三方依赖的 notice/license 文本。移植算法、内置 KEMAR 与
滤波器数据保留原始许可及来源，见[第三方许可证](docs/THIRD_PARTY_LICENSES.md)。
