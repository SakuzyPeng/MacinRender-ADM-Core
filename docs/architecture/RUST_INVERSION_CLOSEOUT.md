# 四期结项（四期 4.10）

> 状态：已完成（2026-10-31）。四期按 [ADR 0021](../adr/0021-rust-primary-project.md) 把 PCM 写出为止的数据链路迁入 Rust，
> 切片与验证要求见 [PCM 一致性审计](PCM_CONSISTENCY_AUDIT.md#四期-pcm-写出为止的数据链路迁入-rust)。
> 五期的初拟切片见审计的[五期一节](PCM_CONSISTENCY_AUDIT.md#五期编排与实时调度迁入-rust)。

## 验收结论

结项提交为 main `221ce28`（4.9 文档合入后）。三平台运行：

| 工作流 | 运行 | 结果 |
| --- | --- | --- |
| Consistency | [41166081362](https://github.com/SakuzyPeng/MacinRender-ADM-Core/actions/runs/41166081362) | 通过；config A、B 的普通与诊断四份报告都是 291 个 PCM id 相同、0 个不同、平台集完整，316 条门禁失败 0 |
| CI | [41166081382](https://github.com/SakuzyPeng/MacinRender-ADM-Core/actions/runs/41166081382) | 通过 |
| Quality | [41166081381](https://github.com/SakuzyPeng/MacinRender-ADM-Core/actions/runs/41166081381) | 通过 |
| Release Build | [41183519975](https://github.com/SakuzyPeng/MacinRender-ADM-Core/actions/runs/41183519975) | 通过；macOS / Windows CLI、Linux AppImage、macOS / Windows GUI 五个打包 job 成功（Publish 只在 tag push 时运行，已跳过） |
| Reference tests | [41183521970](https://github.com/SakuzyPeng/MacinRender-ADM-Core/actions/runs/41183521970) | 通过；7 个单元全部成功 |

诊断报告中，每对平台的 255 份内核、201 个内核检查点、31,100 个用例检查点与 21,380 项 Scene 检查点比较全部相同。
Release Build 在三个平台上构建发布配置，跑许可证全量校验、C ABI 导出校验、CLI 与打包 smoke，并打包 macOS / Windows
GUI；Reference tests 在发布配置下逐个打开 7 个冻结的第三方对照（SAF 在 macOS，libear、libadm、libbw64、
libsamplerate、libebur128、dr_wav 在 Linux）。两者四期之前最后一次运行分别在 10-15 与 10-06。

## 切片

| 切片 | 内容 | PCM 门禁 | 生产 C/C++（行） | 记录 |
| --- | --- | ---: | ---: | --- |
| 4.1 | 方向与基线：ADR 0021、规模棘轮、Rust 检查点写出器、离线场景检查点 | 242 | 45,090 | [4.1](RUST_INVERSION_BASELINE.md) |
| 4.2 | 覆盖先行：`room-binaural` exact 档、策略矩阵、平滑与后置增益、5 份观察内核设门禁 | 283 | 45,026 | [4.2](RUST_INVERSION_COVERAGE.md) |
| 4.3 | Rust 渲染场景：输入导入、声道床、布局表、`05-import` | 283 | 44,632 | [4.3](RUST_INVERSION_SCENE.md) |
| 4.4 | 语义策略、Scene 逐更新应用、导出回写、DS 矩阵与屏幕 JSON、`mradm-json` | 283 | 42,406 | [4.4](RUST_INVERSION_POLICY.md) |
| 4.5 | 扬声器后端（VBAP、HOA、EAR、Scene VBAP）与公共渲染层，`mradm-render` | 283 | 39,747 | [4.5](RUST_INVERSION_SPEAKERS.md) |
| 4.6 | Triple Balance 与 room | 283 | 36,931 | [4.6](RUST_INVERSION_TRIPLE_ROOM.md) |
| 4.7 | 双耳 | 283 | 33,732 | [4.7](RUST_INVERSION_BINAURAL.md) |
| 4.8 | PCM 写出与后处理 IO | 283 | 32,551 | [4.8](RUST_INVERSION_OUTPUT.md) |
| 4.9 | S03：文件流式 seek 按离线窗口，开平滑的文件流式重放 | 291 | 32,551 | [4.9](RUST_INVERSION_SMOOTHING.md) |

内核测量 4.1 为 235 份（230 份门禁），4.2 起 237 份全部设为门禁，`3ca0d2c`（左右独立 HpTF）加到 255 份，仍全部设为门禁。
行数是各切片最后一次合并后 `language-surface-baseline.json` 的值；4.7 与 4.8 之间的 14 行来自维护者的 `3ca0d2c`。
每个结构迁移切片都以删除前的 C++ 生成的黄金数据和 Linux x64 本机新旧对照（普通与诊断构建）验收，有意的数值变化只有
4.4 的 extent 缺省上限与 4.9 的文件流式 seek，各自单独记录。

## 规模

生产 C/C++ 由 45,090 行降到 32,551 行，减少 12,539 行（27.8%）；Apple / Windows 岛 3,213 行与 CUDA 边界 512 行不变。
同期 Rust crate 由 57,413 行增到 98,789 行，C++ 测试由 47,649 行增到 48,698 行（一致性工具与测试支持），冻结参考 12,022 行。

| 模块 | 4.1 | 结项 | 剩下的内容 |
| --- | ---: | ---: | --- |
| `src/adm_realtime` | 5,402 | 5,437 | Scene 引擎、Monitor、ring buffer、OSC UDP、设备调度（五期） |
| `include/adm` | 4,598 | 4,426 | `c_api.h` 与内部 C++ 接口（五期、六期） |
| `src/adm_audio` | 4,909 | 4,184 | 编码器岛与 `audio_io.h` 薄包装 |
| `src/adm_engine` | 4,432 | 3,934 | `RenderService`、`PreviewSession`、能力表、`SceneOutputSession`（五期） |
| `src/adm_c_api` | 3,614 | 3,544 | 公开 C ABI（六期） |
| `src/adm_render_common` | 2,777 | 3,490 | 私有 FFI 头 `routing_ffi.h`、`live_rust_renderer` 与 Apple 岛用的薄包装 |
| `src/adm_dsp` | 3,475 | 1,994 | 私有 FFI 头 |
| `src/adm_cli` | 1,655 | 1,642 | CLI（六期） |
| `src/adm_metadata` / `src/adm_io` | 788 / 590 | 765 / 429 | 场景视图与文件编排 |
| `src/adm_render_binaural` / `room` / `triple_balance` | 3,486 / 1,344 / 2,055 | 599 / 547 / 334 | 适配壳与能力表 |
| `src/adm_render_vbap` / `ear` / `hoa` | 1,236 / 1,039 / 770 | 257 / 115 / 111 | 适配壳与能力表 |
| `src/adm_renderer_factory` | 387 | 387 | 后端选择（五期） |
| `src/adm_core` | 2,225 | 356 | 领域模型视图（五期） |
| `src/adm_peak` / `src/adm_loudness` | 143 / 165 | 0 | 4.8 A 删除 |

[逆转路线](RUST_INVERSION_ROADMAP.md)原估计四期删除 1.6–1.8 万行，实际 1.25 万行。差额主要有三处：一是新增的私有 FFI 头
（`routing_ffi.h` 一个文件就有约 760 行）与 `live_rust_renderer`，使 `src/adm_render_common` 反而增加；二是后端的适配壳与能力表要到五期
删除 `IRenderer` 一类接口时才能去掉；三是编排与实时调度（`src/adm_realtime`、`src/adm_engine`）按计划归五期。

## 四期目标的状态

- **数据链路迁入 Rust。** 导入投影、语义策略、全部自有后端、公共渲染层与 PCM 写出编排在 Rust（`mradm-scene`、`mradm-json`、
  `mradm-render`）。C++ 只剩适配壳、能力表、写出端与后处理的取消 / 进度循环壳，以及五期的编排与调度。
  退出条件“后端与公共渲染层的 C++ 只剩适配壳、锁定矩阵三平台逐位相同、规模按切片实测下降”已满足。
- **`room-binaural` exact 档进矩阵。** 4.2 加入离线、文件流式与 Scene 用例和 `room.*` 内核探针；realtime 档不纳入。
- **S03。** 文件流式的部分在 4.9 解决：seek 之后与离线窗口逐位相同，8 个开平滑的重放要求固定与碎片分块逐位相同。
  Scene 流式的双耳卷积与分数比率重采样仍依赖分块，按分块配置分别门禁。
- **观察内核。** 4.2 起 5 份观察内核设为门禁，内核测量此后全部设为门禁。
- **N01 的 JSON 残留。** 4.4 起 JSON 数字由 `mradm-json` 解析，不再经 C 运行时的 `strtod`。
- **覆盖缺口。** 策略矩阵、DirectSpeakers 矩阵、各后端离线平滑与后置增益在 4.2 补齐。
- **S04、S05、S07** 维持原状（运行时与构建条件），见审计。

## 开放问题（移交五期）

- **room 离线默认 ramp（4.6 已知问题）。** 对象块没有 `interpolationLength` 时，room 的离线与文件流式渲染用 `default_interp_ms × 48`
  帧作 ramp，96 kHz 下只有应有长度的一半。修复会改变 96 kHz room 的 PCM，按有意的数值变化单独成片（审计五期 5.7）。
  见 [4.6 记录](RUST_INVERSION_TRIPLE_ROOM.md)。
- **BINAURAL-001。** Windows cloud 离线端到端耗时回退，只涉及性能，作为独立任务跟踪。
  见 [4.7 记录的 B 合并审查](RUST_INVERSION_BINAURAL.md#b-合并审查2026-10-27)。

## 维护与边界

`Consistency` 继续在 push main 和手动触发时运行，`phase2-gates.json` 是门禁的唯一清单；新增 PCM 用例必须同步加入门禁
（`phase2_tools_test.py` 强制）。门禁只约束锁定的矩阵、构建条件与三个平台（macOS arm64、Linux x64、Windows x64），
不构成对任意输入或工具链的全局承诺。FLAC、Opus、APAC、IAMF 编码与 Apple AUSpatialMixer 后端不在范围内；
Linux arm64 不进矩阵。冻结参考按[参考实现保留与退役](RUST_REFERENCE_RETENTION.md)维护，Reference tests 需在改动参考或其依赖时手动运行。
