# Scene 流式渲染的 C++ 数值残留迁移（一期 1.2）

> 对应 [PCM 一致性审计](PCM_CONSISTENCY_AUDIT.md)一期 1.2，规则见 [ADR 0018](../adr/0018-scene-numeric-replay-rules.md)（提议）。
> 证据在 [`evidence/pcm-consistency-1.2/`](evidence/pcm-consistency-1.2/)。

## 范围与结果

1.2 把 Scene 流式渲染中仍由 C++ 计算的三类数值移入 Rust。文件流式渲染和离线渲染调用的是同一组共享函数，因此同步改用 Rust 实现。

| 部分 | 迁入 Rust 的运算 | C++ 保留 |
| --- | --- | --- |
| 语义策略数值求值（`mradm-dsp` `semantic_policy`） | 增益 scale 与 dB 换算；diffuse/extent/divergence 的乘积和钳位；位置的绝对值、偏移、锁定、方位回绕和距离下限；插值时长上限；Scene 最终电平乘积，以及极坐标到 Cartesian 的换算 | 身份匹配、规则合并、DirectSpeakers 标签/LFE 过滤，以及所有布尔字段（mute、headLocked、channelLock、jumpPosition） |
| Live VBAP 增益合成（`vbap::Panner::source_gains`） | 每个源的极坐标换算、0.4 距离下限、extent spread、面板表乘以源增益，并按源顺序累加 | 发散子源展开调用、声道映射（跳过 LFE）、LFE 路由 |
| Triple Balance 床系数（`triple_balance::bed`） | RC 7.1.2 床到 7.1.4 / 9.1.6 / 22.2 的路由、7.1.4 顶层等功率折叠、22.2 分功率 LFE | 识别床声道和布局 |

每次求值只跨一次 FFI，传入打包好的规则和状态结构，不逐个标量调用。新的 Rust 入口在准备完成后不分配内存，由 `realtime_allocations.rs` 约束。

## 提交与验证

| 提交 | 内容 | 性质 |
| --- | --- | --- |
| `8a73dc9` | 语义策略数值求值迁入 Rust | 结构迁移，位不变 |
| `4d01d0e` | Live VBAP 源增益合成迁入 Rust | 结构迁移，位不变 |
| `1ad7641` | Triple Balance 床路由迁入 Rust | 结构迁移，位不变 |
| `d476f1a` | 策略 dB 换算改用 `libm` 0.2.16 `powf` | 数值规则变更（ADR 0018 §3） |
| `4bede24` | Scene 回放的策略更新加入 `gain_db`、extent、diffuse 和 divergence | 回放输入变更 |

**结构迁移。** 以 Linux x64 Release、config A 在切片前（`22e6a3b`）和切片后各采集一次普通运行与诊断运行，逐文件比较：

- 普通运行 431 个文件全部相同，包括 262 份 PCM；
- 诊断运行 23946 个文件全部相同，包括全部 Scene 检查点和 PCM；
- 诊断构建对 PCM 无扰动。

锁定矩阵只覆盖策略增益 scale，所以另加两层对照：

- `mr_adm_semantic_policy_tests` 冻结了迁移前的 C++ 表达式，逐位比较 88 个策略用例，覆盖 dB、偏移、锁定、钳位、−0.0 和 Cartesian/极坐标起点；
- Rust 测试按旧的逐源组合方式，逐位复现 VBAP 源增益合成（含与不含 spread），并确认单源不累加时保留 −0.0。

离线 CLI 用一份不含 `gain_db` 的完整策略渲染三个夹具（objects-extent → 5.1.4、directspeakers → 双耳、objects-cartesian → 7.1.4）。该策略包含位置偏移、extent、divergence、插值上限，以及 DirectSpeakers 的 scale 与重新定向。新旧二进制输出的 PCM 逐位相同。

三平台：结构迁移头 `1ad7641` 的 Consistency 38529813819、CI 38529817649、Quality 38529821931 均通过。

**dB 规则。** 平台 `powf` 与 `libm::powf` 并不总是相同。在 Linux x64 上，以 0.001 dB 步长扫描 −144 至 +24 dB，共 168001 个值，其中 16451 个相差 1 ulp（例如 −29.845 dB、−4.6 dB）。

- 改用 `libm` 后，所有平台得到同一结果，代价是带 `gain_db` 的策略在每个平台上都可能改变最低位。
- Rust 和 C++ 测试固定了 `libm` 的位模式。
- 锁定矩阵此前没有 `gain_db`，`d476f1a` 前后的 config A 普通与诊断运行仍逐位相同。
- 文件流式与监听中的其他 dB 入口（`resolve_live_channel_gain`、HOA override）不在 Scene 链路上，留在三期。

**回放覆盖。** `4bede24` 把 6144 样本处的策略更新改为：

```json
{"global":{"gain":{"scale":0.5,"gain_db":-4.6},"extent":{"scale":1.5,"max":0.6},
 "diffuse":{"scale":0.5},"divergence":{"range_scale":0.5}}}
```

之后三平台门禁会实际执行迁入 Rust 的策略运算。由于输入变了，Linux 上 74 个 Scene epoch-1 PCM id 全部改变；epoch-2、离线和内核输出不变，诊断仍无扰动。

位置编辑会经过 `scene_math` 中仍调用平台三角函数的极坐标换算，要等 1.3 统一后再加入回放。

## 性能

在共享 4 核 Linux 虚拟机上比较切片前（`22e6a3b`）和切片后的 Release 构建。离线比较交替运行、带预热；中位数比超过 1.05 时复测。

| 用例 | 耗时比 | RSS 比 | PCM |
| --- | --- | --- | --- |
| ear-extent | 1.043（9 轮复测 0.997） | 1.005 | 相同 |
| vbap-point | 0.986 | 1.012 | 相同 |
| binaural-point | 0.856 | 1.001 | 相同 |
| binaural-cloud | 1.004 | 1.000 | 相同 |
| binaural-spreader | 1.171（9 轮复测 0.999） | 1.003 | 相同 |
| vbap-policy（新增） | 0.987 | 1.011 | 仅因 dB 规则不同 |
| binaural-policy-ds（新增） | 1.002 | 1.003 | 仅因 dB 规则不同 |

`benchmark-dsp.py` 新增两个策略用例和 `--case` 选项，策略内容写入结果 JSON。首轮的 1.171 来自运行噪声：这条离线路径不经过任何改动代码，9 轮复测为 0.999。

Scene worker 延迟用 `mr_adm_scene_stream_stress` 测量：每个 10 ms 输入块都带一次位置与增益更新，因此每块都会重新计算 VBAP 增益。

| 渲染器 | 5 轮 p99 中位数（切片前 → 切片后） | 预算 |
| --- | --- | --- |
| VBAP | 163 µs → 138 µs | 10000 µs |
| 双耳 | 1129 µs → 1029 µs | 10000 µs |

单轮 p99 波动大（例如双耳某一轮为 1728 µs），但没有观察到退化。该基准不设语义策略，策略求值的成本由离线策略用例覆盖。

Triple Balance 没有可用的 7.1.2 床 CLI 夹具，床系数只在 generation 配置时计算一次，未单独计时。Windows 未测 RSS。

## 剩余与后续

1.2 后，`apply_policy_to_state`、Live VBAP 增益合成和床路由中不再有直接决定 Scene 样本值的 C++ 浮点运算。尚留在 C++ 的数值点：

- `scene_math.cpp` 中 channelLock 的阈值相加（`max_distance + 1e-4`）和最近扬声器比较，以及 `expand_object_divergence` 的发散阈值比较。它们与空间数学一起在 1.3 迁移。
- LFE 分功率路由仍写成 `gain * k_lfe_split_power_gain`，但调用方传入的增益恒为 1，结果就是常数本身，不引入舍入。
- 策略 JSON 中的数值由 nlohmann 经 `strtod` 解析再转为 float（审计 N01），本切片未改动。

`scene_math` 的三角函数与 VBAP/HRTF 几何仍使用平台数学库，属于 1.3。
