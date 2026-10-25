# Rust 主导逆转路线

> 2026-10-16 起。决策见 [ADR 0021](../adr/0021-rust-primary-project.md)。四期的切片、门禁与验证要求在
> [PCM 一致性审计](PCM_CONSISTENCY_AUDIT.md#四期-pcm-写出为止的数据链路迁入-rust)中维护，本文只维护总路线、
> 模块去向与度量。五期起的内容为初拟，在各期准备时确定。

## 终态

仓库是一个 Rust 项目：Cargo workspace 持有领域模型、语义策略、导入与导出编排、全部自有渲染后端、计量与后处理、
PCM 写出、实时引擎与设备调度、公开 C ABI 的实现和 `mradm` CLI。

C/C++/ObjC++ 只保留在“岛”里，经窄 C 接口由 Rust 调用：
- Apple 框架：AUSpatialMixer 后端、系统空间监听 sink、APAC 与 CAF 元数据；
- Windows `ISpatialAudioClient` sink；
- 可选 NVIDIA CUDA SDK 资源／模块适配边界，房间语义、时钟和控制保留在 Rust；
- 第三方 C 库：libFLAC、libopus、miniaudio、dr_flac、IAMF bridge。

对外契约只有 `include/adm/c_api.h`（ADR 0007）、`mradm` CLI 的行为，以及锁定矩阵上逐位不变的渲染结果。

## 规模与棘轮

`scripts/quality/language-surface.py` 按区域统计 C/C++ 行数（换行符计数，同 `wc -l`）：

| 区域 | 范围 | 处理 |
| --- | --- | --- |
| `production` | `src/` + `include/`，不含岛 | 棘轮：只减不增 |
| `islands` | `src/adm_apple`、`src/adm_windows` | 棘轮：只减不增 |
| `cuda` | `src/adm_render_room/cuda`，仅可选 CUDA SDK 边界 | 独立棘轮：只减不增 |
| `tests` | `tests/`，不含冻结参考 | 只报告；四期补覆盖时一致性工具仍会增长 |
| `reference` | `tests/reference/` | 只报告，随参考退役减少 |
| `rust`、`rust-vendor` | `rust/crates`、`rust/vendor` | 只报告 |

`--check` 读 `scripts/quality/language-surface-baseline.json`，要求三个棘轮区域的实测行数与基线一致。
删除 C++ 后，在同一提交运行 `--update` 收紧基线；已有基线的上限不能通过 `--update` 提高。
CI 的 `version-metadata` job 还传入 `--base-ref`，校验候选基线没有相对 PR 目标提交或 push 前提交放宽；
手动运行以当前提交的第一父提交为基准。基准提交尚无该文件时允许首次引入，基准提交不可读取时失败。

2026-10-20 将 CUDA V2 已有 SDK 适配单列为平台例外，初始上限固定为 `3558e88d` 的 512 行。
兼容旧基线时只为缺失的 `cuda` 区域使用这个固定上限；一旦登记，`--update` 和 `--base-ref`
都拒绝增加其上限。原有生产代码与 Apple／Windows 岛的上限不变，SDK 目录不接受通用渲染实现。

基线 `0a3b362` 时的生产 C/C++（共 44,834 行，另有岛 3,213 行）：

| 模块 | 行数 | 去向 | 期次 |
| --- | ---: | --- | --- |
| `src/adm_realtime` | 5,402 | Scene 引擎、Monitor、ring buffer、OSC UDP、设备调度 → Rust；miniaudio 留作 C 岛 | 五期 |
| `src/adm_audio` | 4,909 | WAV / CAF 写出编排、增益重写、trim、降位、容器元数据 → Rust（四期 4.8）；FLAC / Opus / IAMF 编码留作 C 岛，APAC 归 Apple 岛 | 四期、七期 |
| `include/adm` | 4,598 | `c_api.h` 保留；其余随调用方迁出而删除（4.4 删除 `semantic_policy.h` 与 `io::write_scene`） | 四期～六期 |
| `src/adm_engine` | 4,176 | `RenderService`、`PreviewSession`、能力表、`SceneOutputSession` → Rust | 五期 |
| `src/adm_c_api` | 3,614 | Rust 实现同一 `c_api.h` | 六期 |
| `src/adm_render_binaural` | 3,486 | Rust（4.7），只留适配壳 | 四期 |
| `src/adm_dsp` | 3,475 | 私有 FFI 头随 C++ 调用点删除（4.5 把只剩测试使用的 HOA、EAR、Live VBAP 头移到 `tests/unit`，4.6 把 Triple Balance 头移到 `tests/support` 并删去 Live TB 与 room session 的声明，剩 2,399 行） | 四期～六期 |
| `src/adm_render_common` | 2,777 | Rust（4.5 已完成路由、对象预处理与扬声器离线驱动；扬声器布局表提前到 4.3）。现为 3,306 行：私有 FFI 头 `routing_ffi.h`（609 行，4.6 增加 Triple Balance 与 room 的声明）和两个 Scene 扬声器渲染器共用的适配 `live_rust_renderer`（4.6 C 由 `src/adm_render_vbap` 移入）；给 Apple 岛与双耳的薄包装随 4.7 删除，适配壳到五期删除 | 四期 |
| `src/adm_core` | 2,225 | 语义策略、DS 矩阵、屏幕 → Rust（4.4 已完成，剩 412 行）；领域模型视图到五期删除 | 四期、五期 |
| `src/adm_render_triple_balance` | 2,055 | Rust `mradm-render`（4.6 已完成，剩 355 行的适配壳与能力表；只剩测试使用的 C++ 移到 `tests/support/triple_balance`） | 四期 |
| `src/adm_cli` | 1,655 | Rust CLI，行为不变 | 六期 |
| `src/adm_render_room` | 1,344 | Rust `mradm-render` 与 `mradm-dsp` `room::loading`（4.6 已完成，剩 568 行：适配壳、`render_window` 循环、`RoomProfile` 与 `IRoomControl` 薄壳） | 四期 |
| `src/adm_render_vbap` / `ear` / `hoa` | 1,236 / 1,039 / 770 | Rust `mradm-render`（4.5 已完成，含 Scene 流式 VBAP；剩 257 / 115 / 111 行的适配壳与能力表，Scene VBAP 的适配 4.6 C 移入 `render_common`） | 四期 |
| `src/adm_metadata`、`src/adm_io` | 788、590 | Rust 渲染场景 `mradm-scene`（4.3）；导出回写随语义策略迁入（4.4 已完成，剩 765、444 行的场景视图与文件编排） | 四期 |
| `src/adm_renderer_factory` | 387 | 五期随编排迁入 Rust | 五期 |
| `src/adm_loudness`、`src/adm_peak` | 165、143 | Rust（4.8） | 四期 |

四期估计删除生产 C/C++ 1.6–1.8 万行（约 35%），以各切片实测为准。

## 各期目标（五期起为初拟）

| 期 | 目标 | 退出条件 |
| --- | --- | --- |
| 四期 | PCM 写出为止的数据链路迁入 Rust；`room-binaural` exact 档进矩阵；S03、观察内核、N01 JSON 残留与覆盖缺口收尾 | 后端与公共渲染层的 C++ 只剩适配壳；锁定矩阵三平台逐位相同；C++ 规模按切片实测下降 |
| 五期 | 编排与实时调度迁入 Rust：`RenderService` / `PreviewSession`、后处理编排、`SceneStreamEngine`、`MonitorEngine`、ring buffer、OSC UDP 收发与快照、设备调度 | 一致性工具改为 Rust，并在同一构建上证明新旧工具输出逐位相同；C++ 的 `AdmScene` 与适配壳删除 |
| 六期 | 边界迁入 Rust：C ABI 由 Rust cdylib 实现（`mradm_capi`、同一 `c_api.h`、`SOVERSION 1`、只导出 `adm_*`）；CLI 改为 Rust；删除 C++ SDK 与安装规则；构建以 Cargo 为主 | ABI 导出、GUI P/Invoke、CLI 输出与退出码回归通过；CLI11、spdlog/fmt、nlohmann_json、tl-expected 移除 |
| 七期 | 平台岛收缩：Apple / Windows sink 与编码器 C 库只留窄 C 接口，评估 objc2 / windows crate；冻结参考按条件退役；C++ 测试清退 | 生产 C/C++ 只剩岛 |

## oracle 的驱动面

一致性工具当前驱动的是内部 C++ 接口，不经 C ABI：

| 路径 | 工具 | 驱动的接口 |
| --- | --- | --- |
| 离线 | `mradm` CLI、`mr_adm_phase2_render` | C++ `RenderService` |
| 文件流式 | `mr_adm_phase2_stream` | `io::import_scene`、内部 `resolve_renderer`、`IRenderStream` |
| Scene | `mr_adm_phase2_scene` | 内部 `SceneStreamEngine` 的时钟缝、`SceneOutputSession` 的捕获设备 |
| 内核 | `mr_adm_phase2_kernels` | 直接调用 Rust crate |

C ABI 目前替代不了这些路径：离线没有 listener orientation；文件流式没有回放入口；Scene 不能注入时钟、
没有 wait_idle、也读不回设备侧 DSP 的输出。

因此：
- 四期保留这些 C++ 接口作适配壳，工具不改，每个结构迁移切片都用同一套工具与切片前逐位对照。
- 迁进 Rust 的代码继续发出同名检查点。Rust 侧的生产路径写出器是 `mradm-dsp` `diagnostics::process`，
  与 C++ `consistency::dump` 使用同一约定；每个检查点名只归一方所有。
- 五期迁移这些接口时，工具改为 Rust 并直接驱动 Rust 引擎。切换前先在同一构建上证明新旧工具输出逐位相同，
  不在 C ABI 中为测试新增入口。

## C++ 删除与冻结参考

`tests/reference/` 的冻结参考引用若干生产头，例如 `render_common.h`、`speaker_layouts.h`、`speaker_pcm.h`、
Triple Balance 的 `panner.h` / `size_panner.h` / `size_processor.h` 与 `adm/scene.h`。删除这些 C++ 之前，
按 [参考实现保留与退役](RUST_REFERENCE_RETENTION.md) 有两条路：把被比较的旧实现连同参考一起冻结，或满足三条退役条件后退役该单元。
后者的 `released` 要求正式 `v*` tag，而最新 tag v1.9.1 早于这些迁移。

**维护者决定（2026-10-16）：冻结进 `tests/reference/`。** 四期各切片删除被参考引用的生产 C++ 时：

1. 把参考实际用到的旧实现（头文件及其依赖的最小实现）复制进对应单元的 `tests/reference/<unit>/`，
   参考改为包含这份冻结副本，不再包含生产头；
2. 在 `tests/reference/retention.json` 登记新文件与 SHA-256，`check-reference-retention.py` 校验；
3. 只做使其脱离生产头所需的机械改动，不改被比较的算法；改动写进该切片的记录；
4. 冻结副本计入 `reference` 区域，不计入生产 C/C++ 棘轮；参考退役时随单元一起删除。

不为此安排正式发布；退役仍按原有三条条件另行审查。

4.6：Triple Balance 的 `panner` / `size_panner` / `size_processor` 早有冻结副本（`tests/reference/triple_balance/`），参考包含的是同目录的副本，它用到的 `speaker_pcm.h` 由 `tests/support/legacy_speaker_pcm` 的垫片提供（4.6 B）；生产中的这几份只剩测试使用，连同 `layout_222`、`bed` 移到 `tests/support/triple_balance/`，不计入生产棘轮。

## 与 PCM 一致性审计的关系

- [PCM 一致性审计](PCM_CONSISTENCY_AUDIT.md) 维护数值条目、门禁与各期切片；本文维护语言归属与模块去向。
- 一期至三期统一了 PCM 写出为止的数值规则。四期起的结构迁移要求逐位不变，有意的数值变化单独成片。
