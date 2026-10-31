# 文件流式 seek 与对象平滑（四期 4.9）

> 对应 [PCM 一致性审计](PCM_CONSISTENCY_AUDIT.md#四期-pcm-写出为止的数据链路迁入-rust)四期 4.9，收尾审计项 S03 中文件流式的部分。
> 本切片是有意的数值变化：扬声器文件流式在 seek 之后改为与同一位置的离线窗口逐位相同。离线渲染的输出不变。
> 变化的范围与原因见[发布说明](#发布说明)。

## 范围与原因

审计 4.9 的原文是：文件流式对象平滑改为按绝对样本网格的块内插值，与离线一致、与请求分块无关。这一行写于 4.5 之前。4.5 把扬声器文件流式迁入 Rust 后（`mradm-render` `stream::FileStream`，VBAP、HOA、EAR 共用），流已经按 `max(1024, object_smoothing_frames)` 帧的固定块渲染进 FIFO：请求怎样分块都不影响输出，从第 0 帧起也与离线逐位相同。

剩下的偏差都在 seek。旧实现在 seek 时把块的起点放在目标帧，并在目标帧重置渲染状态：
- **块网格。** 开启对象平滑时，`Mixer::speaker/ear` 与 HOA 编码器在每块内按块首、块尾的增益插值。离线窗口的块在绝对网格上（块长的整数倍），seek 之后的块不在，插值端点就与离线窗口不同。
- **HOA / EAR 的状态。** 离线窗口在目标帧不小于一个块长时，会先预滚一个对齐的块，重新填满 HOA 的 1024 帧 diffuse 延迟线，以及 EAR 的去相关重叠与 255 帧补偿延时。流在目标帧直接清空这些状态，seek 之后的开头一段因此与离线窗口不同。

两者都违反 `IRenderStream::seek` 的约定（`include/adm/render.h`）：seek 之后的输出应等于同一位置的离线窗口渲染。双耳文件流式（4.7 C）早已按离线窗口的预滚 seek；Triple Balance 拒绝对象平滑；room 不用对象平滑。这三者不在本切片的改动范围内。

## 修法

- **共用窗口起点。** 离线驱动计算窗口起点的规则提成 `offline::window_start`，离线与流共用：扬声器混音在块边界上无状态，起点为目标所在的块；HOA / EAR 在目标不小于一个块长时再往前一块，否则从 0 开始；有状态的 Triple Balance 阶段从 0 开始。离线输出不变。
- **seek。** reader 定位到 `window_start(target)`，渲染状态在那里重置。从起点到目标之间的帧在下一次 `process` 里渲染后丢弃，可能跨两块。seek 本身仍只做定位，所以 `seek_requires_preroll()` 仍为 false，Monitor 切换后端的路径不变。
- **live gain。** seek 时把 20 ms 的去拉链重置到当前目标（与双耳流式相同），seek 之前没走完的 ramp 不延续，被丢弃的帧也不消耗 ramp。

### 用户决定

2026-10-31：
- seek 完全照离线窗口，包括 HOA / EAR 的预滚块；为此改变的现有门禁用例按有意变化处理。只修开平滑情形的方案没有采用：那样 HOA / EAR 在 seek 之后仍与离线窗口不同。
- 新增开平滑的文件流式重放，覆盖 VBAP、HOA、EAR 与双耳，设为门禁。

## 发布说明

1. **扬声器文件流式的 seek。** VBAP、HOA、EAR 的文件流式在 seek 之后与同一位置的离线窗口逐位相同，不论是否开启对象平滑。HOA / EAR 在 seek 之后会多渲染一个预滚块，所以实时监听中拖动进度后的 HOA / EAR 输出与旧版不同；不开平滑的 VBAP 输出不变。
2. **live 覆盖的生效点。** live 覆盖（增益、静音）照旧从下一个渲染块开始生效；seek 之后的块边界现在落在绝对网格上，生效点随之移动。例如 `stream-hoa-bed-*` 中 40000 帧的静音由 40216 帧移到 40536 帧开始。
3. **seek 重启 live gain ramp。** 原来 seek 不重置 20 ms 的去拉链，seek 之前没走完的 ramp 会延续到 seek 之后；现在 seek 之后立即为目标增益。
4. **一致性工具。** `mr_adm_phase2_stream` 接受 version 5 的用例（`object_smoothing_frames`）与 `saf`（VBAP）渲染器，version 3 / 4 不变。

## 新增用例与门禁

8 个开平滑的文件流式重放，沿用 4.2 离线 `smoothing-*` 用例的 `objects-room` 夹具（四个移动块）与 2048 帧平滑，事件为 live 增益、静音与输出第 31000 帧处到 4800 帧的 seek（不在 2048 帧网格上）：

| 用例 | 渲染器 | 布局 |
| --- | --- | --- |
| `stream-smoothing-saf-714-{fixed,fragmented}` | `saf`（VBAP） | 7.1.4 |
| `stream-smoothing-hoa3-{fixed,fragmented}` | `hoa` | hoa3 |
| `stream-smoothing-ear-714-{fixed,fragmented}` | `ear` | 7.1.4 |
| `stream-smoothing-binaural-{fixed,fragmented}` | `saf-binaural`（cloud） | binaural |

全部设为三平台逐位相同的门禁（`scripts/consistency/phase2-gates.json`），固定与碎片分块也要求逐位相同。PCM id 由 283 增至 291，文件流式重放由 28 增至 36。

S03 的其余部分不变：Scene 流式的双耳卷积与分数比率重采样依赖分块，仍按分块配置分别门禁（见 [1.5 记录](RUST_SCENE_TIMING.md)）。

## 验证

**Rust 测试。** `mradm-render` 的 `tests/vbap.rs`、`tests/hoa.rs`、`tests/ear.rs`（含 `wav71`）各加一个测试：流先播放一段，再 seek 到 8 个目标（0、511、块长 −1、块长、块长 +476、两倍块长 +1、末尾前 7 帧、末尾），在不开平滑与平滑 1024 / 1500 / 2048 帧、两种拉取分块下，与 `Driver` 的离线窗口逐位相同；seek 之前没走完的 ramp 不留痕迹。植入 4 处缺陷（旧的 seek、网格对齐但不预滚、丢弃帧数差一、seek 不重启去拉链），三个后端都能抓到。

**新旧对照。** 基线 `9e52bcb`，候选矩阵子集包含 79 个离线行与 36 个文件流式行，基线支持其中 28 个文件流式行。普通与诊断构建的对照结果：
- 全部 79 个离线行与 16 个文件流式重放相同；
- 12 个 EAR / HOA 文件流式重放不同，即 `stream-{ear-point,ear-extent,ear-bed,ear-hoa3,hoa-extent,hoa-bed}-{fixed,fragmented}`。它们都从 seek 那一帧（输出第 31000 帧）开始不同；`hoa-bed` 没有 diffuse 状态，从 40217 帧开始，原因见发布说明第 2 项；
- 8 个新用例只有候选能跑（基线工具不认识 version 5）；
- 36 个重放的固定与碎片分块都逐位相同。

逐字节对照的例外只有上述 12 个有意变化的 id。ctest 81/81 连跑 3 次；fmt、clippy、棘轮、version、retention 通过；生产 C/C++ 行数不变（只改了测试工具）。

**维护者本机验证。** 维护者本机全量对照后合入（`ecb2fb5`）。

**三平台。**

| 运行 | 提交 | 结果 |
| --- | --- | --- |
| [41159930885](https://github.com/SakuzyPeng/MacinRender-ADM-Core/actions/runs/41159930885) | `ecb2fb5` | 通过；config A、B 的普通与诊断四份报告均为 291 个 PCM id 相同、0 个不同、平台集完整、门禁失败 0 |
