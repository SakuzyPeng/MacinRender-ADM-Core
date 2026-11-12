# 五期结项（五期 5.9）

> 状态：已完成（2026-11-11）。Consistency、CI、Quality、Release Build 与 Reference tests 都已在 main 上通过。
> 五期按 [ADR 0021](../adr/0021-rust-primary-project.md) 把编排与实时调度迁入 Rust，切片与验证要求见
> [PCM 一致性审计的五期一节](PCM_CONSISTENCY_AUDIT.md#五期编排与实时调度迁入-rust)，驱动面清单与规模基线见 [5.1 记录](RUST_ORCHESTRATION_BASELINE.md)。

## 验收结论

验收提交为 main `5b436f8`（5.8 合入 `7d111ba` 之后，只多维护者的两个 CI 依赖提交）。按 5.9 的范围，三平台由 CI 运行，
不再另安排 Linux 主机验证；macOS arm64 与 Windows x64 的本机验证由维护者在各切片合并前完成。
Release Build 与 Reference tests 在 main `2207538` 上运行，它比验收提交只多本记录的文档提交。

| 工作流 | 运行 | 结果 |
| --- | --- | --- |
| Consistency | 43058940532 | 通过；config A、B 的普通与诊断四份报告都是 294 个 PCM id 相同、0 个不同、平台集完整，门禁失败 0 |
| CI | 43058944168 | 通过 |
| Quality | 43058944517 | 通过 |
| Release Build | 43120180578 | 通过；macOS / Windows CLI、Linux AppImage、macOS / Windows GUI 五个打包 job 成功（Publish 只在 tag push 时运行，已跳过） |
| Reference tests | 43120184482 | 通过；7 个单元全部成功 |

Release Build 在三个平台上构建发布配置，跑许可证全量校验、C ABI 导出校验、CLI 与打包 smoke，并打包 macOS / Windows GUI；
Reference tests 在发布配置下逐个打开 7 个冻结的第三方对照（SAF 在 macOS，libear、libadm、libbw64、
libsamplerate、libebur128、dr_wav 在 Linux）。两者都是手动触发，这是它们在本仓库的第一次运行。

五期的退出条件——一致性工具改为 Rust 并在同一构建上证明新旧工具逐位相同、C++ 的 `AdmScene` 与适配壳删除——已满足：
`mr_adm_consistency_{render,stream,scene}` 分别在 5.2 D、5.3 B、5.4 D 改为 Rust，切换前都在同一构建上与旧工具逐字节对照；
`AdmScene` 与后端适配壳在 5.6 删出生产。

## 切片

| 切片 | 内容 | PCM 门禁 | 生产 C/C++（行） | 记录 |
| --- | --- | ---: | ---: | --- |
| 5.1 | 方向与基线：驱动面与岛接口清单、规模基线、工具切换规则与 `compare-collections.py` | 291 | 32,551 | [5.1](RUST_ORCHESTRATION_BASELINE.md) |
| 5.2 | 离线编排：能力表与发现文档、请求判定、后端准备与离线驱动、`RenderService` 状态机，Rust 版离线重放工具 | 291 | 29,805 | [5.2](RUST_ORCHESTRATION_OFFLINE.md) |
| 5.3 | 文件流式与监听的打开，Rust 版文件流式重放工具 | 291 | 29,297 | [5.3](RUST_ORCHESTRATION_STREAM.md) |
| 5.7 | room 默认 ramp 按房间采样率换算（有意的数值变化，提前到 5.4 之前） | 294 | 29,260 | [5.7](RUST_ROOM_DEFAULT_RAMP.md) |
| 5.4 | Scene 流式引擎、输出会话与 HpTF 运行时，Rust 版 Scene 重放工具，五个怪癖的修正 | 294 | 26,363 | [5.4](RUST_ORCHESTRATION_SCENE.md) |
| 5.5 | Monitor 调度、监听会话与 OSC 接收器 | 294 | 24,409 | [5.5](RUST_ORCHESTRATION_MONITOR.md) |
| 5.6 | 后端适配壳、Apple 总线计划、C++ `AdmScene` 与测试专用路由 FFI 删出生产 | 294 | 21,776 | [5.6](RUST_ORCHESTRATION_SHELLS.md) |
| 5.8 | 一致性工具、C++ 包装、测试目标与生产注释按功能命名 | 294 | 21,772 | [5.8](RUST_ORCHESTRATION_NAMING.md) |

行数是各切片最后一次合并后 `language-surface-baseline.json` 的值；5.3 与 5.7 之间少的 37 行来自维护者的 `7b08527`（FLAC 读取校验）。
内核测量 255 份，全部设为门禁，五期未变。有意的数值变化只有两处，各自单独记录：5.7 的 room 默认 ramp（3 个新 id），
5.4 的尾声长度修正（36 个变采样双耳 Scene 用例末尾的重采样 lookahead 区间）。其余切片都是结构迁移，以删除前的 C++ 生成的
黄金数据、同构建新旧工具或新旧探针对照与植入缺陷验收。

## 规模

生产 C/C++ 由 32,551 行降到 21,772 行，减少 10,779 行（33.1%）；Apple / Windows 岛由 3,213 行降到 3,045 行
（Apple 总线计划移入 Rust，5.6 B），CUDA 边界 512 行不变。同期 Rust crate 由 98,527 行增到 126,938 行（新 crate
`mradm-engine` 20,329 行），C++ 测试由 48,690 行增到 53,071 行（迁出生产的适配壳、`AdmScene` 与测试支持），冻结参考 12,026 行。

| 模块 | 5.1 | 结项 | 剩下的内容 |
| --- | ---: | ---: | --- |
| `src/adm_realtime` | 5,437 | 1,366 | `SceneStreamEngine` / OSC 接收器薄壳、岛流表、miniaudio 设备（C 岛） |
| `src/adm_engine` | 3,934 | 1,857 | `RenderService` 循环壳与岛请求、`MonitorSession` / `SceneOutputSession` 薄壳、平台事实 |
| `src/adm_render_common` | 3,490 | 2,629 | 引擎记录、能力表记录、HpTF 控制、布局表视图、实时增益与 `SerialWorker`、私有 FFI 头 |
| 后端目录（binaural / ear / hoa / triple_balance / vbap） | 1,416 | 0 | 5.6 A 删除 |
| `src/adm_render_room` | 547 | 283 | `RoomProfile` 与 `IRoomControl` 薄壳、Scene 渲染器请求 |
| `src/adm_renderer_factory` | 387 | 46 | 布局别名 |
| `src/adm_core` / `src/adm_metadata` / `src/adm_io` | 356 / 765 / 429 | 200 / 386 / 363 | 领域模型、私有场景 FFI 头、`io::RenderScene` |
| `src/adm_dsp` | 1,994 | 1,535 | 私有 FFI 头 |
| `include/adm` | 4,426 | 3,983 | `c_api.h`（2,008）与内部 C++ 接口（`render.h` 547、`audio_io.h` 412 等） |
| `src/adm_audio` | 4,184 | 4,169 | 编码器岛与 `audio_io.h` 薄包装 |
| `src/adm_c_api` / `src/adm_cli` | 3,544 / 1,642 | 3,545 / 1,410 | 公开 C ABI 与 CLI（六期） |

[5.1 记录](RUST_ORCHESTRATION_BASELINE.md)预计五期结束时生产 C/C++ 只剩 `c_api.h` 与 C ABI 实现、CLI、编码器与 miniaudio 的
窄接口。实际另有约 1.1 万行：`RenderService` / `PreviewSession` / 会话的 C++ 外壳（`PreviewSession` 按用户决定保留，5.3）、
`include/adm` 的内部 C++ 接口（`IRenderer` / `IRenderStream` 留给 Apple 岛与 `EngineStream`）、`src/adm_render_common` 与
`src/adm_dsp` 的私有 FFI 头。它们都是 C ABI 与 CLI 的调用面，随六期把 C ABI 与 CLI 改为 Rust 一并删除。

## 开放问题（移交六期）

- **BINAURAL-001。** Windows cloud 离线端到端耗时回退，只涉及性能，继续作为独立任务跟踪。
  见 [4.7 记录](RUST_INVERSION_BINAURAL.md#binaural-001windows-cloud-离线端到端耗时回退p2开放)。
- **物理头追踪。** 维护者暂不接入物理头追踪设备，OSC 接收的实机采样、头动与听感确认暂缓；GUI 尚无 PoseBridge OSC 输入入口
  （见 [5.5 记录](RUST_ORCHESTRATION_MONITOR.md)）。
- **GUI。** 按维护者决定，GUI 在迁移完成后再考虑，可能替换 Avalonia；五期未改 GUI 行为。
- **格式基线。** `tests/unit/binaural_render_fixture_test.cpp` 在不同 clang-format 版本下的格式结果不同，`format.sh --check`
  以 CI 的版本为准。

## 六期（初拟）

六期按[逆转路线](RUST_INVERSION_ROADMAP.md)把边界迁入 Rust：C ABI 由 Rust cdylib 实现（`mradm_capi`、同一 `c_api.h`、
`SOVERSION 1`、只导出 `adm_*`），CLI 改为 Rust，删除 C++ SDK 与安装规则，构建以 Cargo 为主；退出条件是 ABI 导出、GUI P/Invoke、
CLI 输出与退出码回归通过。切片与用户决定（2026-11-12）见[审计六期一节](PCM_CONSISTENCY_AUDIT.md#六期边界迁入-rust)。

## 维护与边界

`Consistency` 继续在 push main 和手动触发时运行，`scripts/consistency/gates.json` 是门禁的唯一清单；新增 PCM 用例必须同步加入门禁
（`collection_tools_test.py` 强制）。门禁只约束锁定的矩阵、构建条件与三个平台（macOS arm64、Linux x64、Windows x64），
不构成对任意输入或工具链的全局承诺。FLAC、Opus、APAC、IAMF 编码与 Apple AUSpatialMixer 后端不在范围内；Linux arm64 不进矩阵。
冻结参考按[参考实现保留与退役](RUST_REFERENCE_RETENTION.md)维护，Reference tests 需在改动参考或其依赖时手动运行。
