# Scene 状态与控制时序（一期 1.5）

> 对应 [PCM 一致性审计](PCM_CONSISTENCY_AUDIT.md)一期 1.5，规则见 [ADR 0018](../adr/0018-scene-numeric-replay-rules.md) §4、§5。
> 证据在 [`evidence/pcm-consistency-1.5/`](evidence/pcm-consistency-1.5/)。

## 范围

1.5 逐条核对 ADR 0018 的事件、时钟、分块、reset、EOS 与欠载规则，确认每条都有对应的实现位置和测试。
其中 1.2–1.4 已经补齐的规则只做核对；1.5 新增的工作有三项：

- 把宣称分块无关的部分变成门禁；
- 对不能分块无关的部分写明原因和块长条件；
- 修正设备侧唯一一处会随回调分块改变输出的控制时序。

| 提交 | 内容 | Linux x64 config A |
| --- | --- | --- |
| `f94d28e` | HpTF 淡化期间发布的目标改为在当前淡化结束的那一帧开始 | 普通 431 个、诊断 23946 个文件逐位不变 |
| `07030f5` | 分块无关门禁；重采样 2 的整数次幂比率逐位测试；设备欠载测试 | 无输出变化 |

**三平台。** `07030f5` 的 Consistency 38598585257 与 CI 38598590575 通过：
- A/B 两种配置下 188 个 PCM id 三平台相同，门禁无失败；160 个内核文件三平台相同。
- macOS、Linux、Windows 共 12 个采集作业（A/B × 普通/诊断）都执行了 11 个用例的分块无关检查，全部通过。
- 诊断运行中三平台仍有差异的只有三期的 `ear-hoa`、`ear-5_1-hoa-input` 检查点，与 1.4 相同。

## 规则与验证

| ADR 0018 规则 | 实现位置 | 验证 |
| --- | --- | --- |
| 元数据更新按输入样本位置，worker 切片后换算为切片内偏移 | `SceneStreamEngine` 切片循环 | `test_sample_accurate_ramp`；锁定矩阵中 `21-effective` 状态流在两种分块下全部相同（74/74） |
| worker 控制（策略、姿态、后端切换）在下一个切片开始前接纳 | `apply_available_controls`，每个切片前调用一次 | 回放器在控制位置强制切开切片并用 fence 固定先后；VBAP 与 Triple Balance 在两种分块下逐位相同 |
| 输出音量按已消费帧生效 | 峰值保护 `pop` 读取音量；无保护路径在 `pull` 末尾乘音量 | `test_scene_device_pull_partitioning` |
| HpTF 目标在设备侧处理时接纳；淡化期间只保留最新目标 | Rust `hptf::Processor`（`f94d28e` 起由它保存待处理目标） | `targets_during_a_transition_start_where_it_ends`；`test_scene_device_pull_partitioning` |
| 头追有效期 750 ms，按注入的单调时钟判定，决定切片上限 512 或 1024 帧 | `head_tracking_recent` | `mr_adm_phase2_scene --self-test`；`test_tracking_activation_during_startup`；矩阵在第 2048 帧设置姿态、第 10240 帧把虚拟时钟推进 1 s 使其过期 |
| epoch 清空处理历史，保留继承配置 | `begin_epoch`、`SceneOutputSession::begin_epoch` | `test_scene_hptf_snapshot_on_epoch`；回放第 2 个 epoch（seek 到 257，短尾到 290） |
| 输出帧数按有理数累加，EOS 余数补一帧，preroll 向上取整 | `allowed_output_frames` 累加器 | `test_rational_accumulator_many_frames`、`test_resampling_preroll_boundary`；回放器校验每个 epoch 的帧数等于 ⌈(end − target)·out/in⌉ |
| 生产完成且输出环为空才报告 EOS | `SceneStreamEngine::pull` | `test_pull_state_contract`；回放器要求 EOS 且帧数精确 |
| 欠载补的零不是数值输出 | `SceneStreamEngine::pull`；`SceneOutputSession::pull_stereo` 只把媒体帧送入 HpTF 与峰值保护 | `test_pull_state_contract`；新增 `test_scene_device_underrun_is_not_media`；回放器要求零欠载 |

## 分块条件

对每个用例，按切片顺序拼接各阶段检查点，再比较固定分块（512 帧）与碎片分块（1、7、127、511、1024 循环）。
逐用例的首个差异位置见 [`partition-stages.json`](evidence/pcm-consistency-1.5/partition-stages.json)。

| 环节 | 最大块长 | 分块无关？ | 依据 |
| --- | --- | --- | --- |
| 输入与语义状态（`10-input`、`21-effective`） | 提交帧任意长 | 是 | 37 对用例 × 2 个 epoch 全部相同 |
| VBAP 渲染（立体声与多声道） | worker 切片 ≤ 1024 帧（头追有效时 ≤ 512） | 是 | 增益按绝对样本位置插值；矩阵中切换到双耳之前全部相同 |
| Triple Balance 渲染 | 同上 | 是 | 7 个同采样率用例渲染、切换与 PCM 全部相同；Rust 测试 `arbitrary_partitions_and_instances_are_exact` |
| 双耳渲染 | 同上；卷积块 1024 帧 | **否** | `LiveConvolver` 每次调用对“历史 + 本块输入”做一次 FFT，块边界改变 FFT 舍入；HRTF 目标渐变长度取片段长度。矩阵中从第 0 个样本起不同 |
| 重采样，比率为 2 的整数次幂 | 每次调用 ≤ 1024 输入帧，输出缓冲 4096 帧 | 是 | 插值位置步长精确可表示；新增逐位测试覆盖 ×2、×4、×½、×¼ |
| 重采样，其他比率（44.1↔48 kHz） | 同上 | **否** | rubato 以 f64 累加插值位置，每块末减去已消耗长度，舍入随分块变化；`9.1.6-size1-48000-44100` 渲染相同，重采样后才不同。既有测试只要求误差 < 2e-6 |
| 后端切换过渡 | 2048 个输出帧 | 是（输入相同时） | 按输出位置计算；矩阵中首个差异位置总是与重采样输出相同 |
| 设备侧：HpTF、峰值保护、音量 | HpTF 内部 1024 帧网格，淡化 2048 帧；峰值保护前瞻 rate/200 帧，单次补充 ≤ 4096 帧 | 是（控制按已消费帧注入、流环有数据时） | `test_scene_device_pull_partitioning`：5 种回调分块逐位相同，其中含淡化中排队并被替换的目标 |

据此，`phase2_common.partition_invariant` 选出 VBAP / Triple Balance 后端、不切换到双耳、输入输出采样率相同的 11 个回放用例。
每个平台的采集要求它们固定与碎片分块的两个 epoch PCM 逐位相同，否则采集失败。
其余 26 对用例继续作为独立的精确门禁，分块比较只作为证据记录。

**实时运行时的落点。** 分块无关只说明：事件位置相同时，输出与分块方式无关。实时运行时，事件落点本身还取决于调度：

- worker 控制在其到达后的下一个切片起点生效，切片边界随提交分块和头追状态变化；
- HpTF 目标在发布后首次补充峰值保护缓存时生效。缓存已满时，这个媒体帧等于“已消费帧 + 前瞻帧数”；
  若补充时流环数据不足，会更早处理较少的帧。

所以精确比较仍只用回放器注入的事件位置和虚拟时钟；按墙钟操作的实时会话不在逐位承诺之内。

## HpTF 排队目标的修正

1.4 之前，C++ 信箱在淡化期间保留最新目标，要等淡化完成后的**下一次** `process` 调用才开始。
淡化结束的帧是固定的，但下一次调用的起点随回调分块变化，所以连续两次快速调整 HpTF 时，第二段淡化的起点和输出都依赖分块。

`f94d28e` 把待处理目标交给 Rust `Processor` 保存：

- 淡化期间收到的目标替换此前排队的目标，并在当前淡化到达 `BLEND_FRAMES` 的那一帧开始，必要时就在同一次调用内开始；
- reset 的接纳优先级为：显式目标 > 排队目标 > 正在淡入的目标，与原先 C++ 信箱的结果一致。

新的设备侧测试以 512 帧分块为参照，其余 4 种分块在旧实现上全部失败。锁定矩阵的 HpTF 更新相隔正好 2048 帧，不会出现排队，因此输出不变。
文件流式渲染的实时监听共用 `HptfProcessor`，同样适用。

## 剩余

- 双耳渲染和分数比率重采样的分块依赖是结构性的，本切片不改变。要消除它们：
  - 双耳需要在内部按固定块缓冲并引入延迟；
  - 重采样需要把 rubato 的插值位置改为精确有理数，这会改变所有分数比率重采样的输出，包括离线渲染。

  两者都要另行决定。当前规则是把分块作为这些用例的比较配置。
- 1.6 在三平台上验收 Release 候选，覆盖内置 HRTF、外部 SOFA、变采样、HpTF 切换、峰值保护和多声道输出，见 [1.6 记录](RUST_SCENE_RELEASE_ACCEPTANCE.md)。
