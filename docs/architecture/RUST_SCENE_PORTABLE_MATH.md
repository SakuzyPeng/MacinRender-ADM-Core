# Scene 空间计算与 FFT 系数的可移植数学函数（一期 1.3）

> 对应 [PCM 一致性审计](PCM_CONSISTENCY_AUDIT.md)一期 1.3，规则见 [ADR 0018](../adr/0018-scene-numeric-replay-rules.md) §3。
> 证据在 [`evidence/pcm-consistency-1.3/`](evidence/pcm-consistency-1.3/)。

## 范围

1.3 统一 Scene 流式渲染所用的空间计算和卷积准备。目标是这些计算不再依赖各平台自带的 C 数学库。
共享实现同时服务于文件流式渲染和离线渲染，因此这些路径一并改变。

| 位置 | 改动 | 精度 |
| --- | --- | --- |
| `scene_math`：Cartesian→极坐标、方向向量、双耳与非双耳极坐标 | `atan2`、`hypot`、`sin`、`cos` 改用 `libm` | 前三者 f64；非双耳极坐标仍为 f32（`atan2f`、`hypotf`） |
| `scene_math`：extent 云点 | 正切改为 `libm::tanf` | f32 |
| `scene_math`：divergence 角、头部旋转、PoseBridge 姿态 | `atan2`、`asin`、`sin`、`cos` 改用 `libm` | f64 |
| `geometry::direction`（VBAP 面板与 HRTF 网格共用） | 改用 `libm::sincos`，与 OM spreader 已用的 `portable_direction` 合一 | f64 |
| VBAP spread 环 | 改用 `libm::sincos` | f64 |
| HRTF 幅度插值 | `hypot` 改为 `libm::hypotf` | f32 |
| Live 双耳 divergence 角 | 改用 `libm::atan2` | f64 |
| SOFA Cartesian 坐标转换 | 改用 `libm::atan2` | f64 |
| RustFFT `compute_twiddle`、RealFFT 后处理 twiddle | f64 `cos`/`sin` 改用 `libm` | f64，再按原方式转为 FFT 精度 |

迁移前 `scene_math.cpp` 还在 C++ 中计算两处数值，现已移入 Rust：

- channelLock 的最近扬声器选择、最大距离容差（`+1e-4`）和锁定后的极坐标；
- divergence 阈值的钳位与比较（`<= 1e-4`）。

RealFFT 3.5.0 为这一处改动纳入 `rust/vendor/realfft`，做法与 RustFFT、rubato 相同：

- `PATCHES.md` 和 `PROVENANCE.json` 记录归档哈希和每个未改动文件；
- 许可证清单、SBOM 和第三方依赖表都注明了这两处补丁。

## 提交与验证

| 提交 | 内容 | Linux x64 config A 相对前一提交 |
| --- | --- | --- |
| `c1a9c51` | channelLock 与 divergence 阈值移入 Rust | 普通 431 个文件、诊断 23946 个文件逐位相同 |
| `8d260b8` | Scene 空间数学改用 `libm` | 262 份 PCM 中 98 份改变（见下），另有 14 份 HRTF/SOFA 内核文件改变 |
| `e432729` | FFT twiddle 改用 `libm`，纳入 RealFFT | 全部 PCM 与 f32 内核不变；只有 f64 值改变（twiddle 观察列及 42 份 EAR f64 逆变换检查点） |
| `4343c60` | 冻结参考测试中，旋转后的极点按方向比较 | 仅测试 |
| `860abbf` | VBAP 与双耳 Scene 回放的策略更新加入极坐标位置偏移 | 58 个 VBAP/双耳 Scene epoch-1 PCM 因输入改变；其余不变 |

**`8d260b8` 改变的 PCM**

- 改变：双耳与 SOFA 用例、Scene 双耳/SOFA/设备用例、12 个 Scene VBAP extent 云用例，以及两个离线双耳 extent 用例。
- 不变：EAR、HOA 和 Triple Balance 的输出。

Linux 上 glibc 与 `libm` 的 f64 三角函数结果并不总是相同，因此 Linux 本身的输出也会变。

**冻结参考测试的调整。** `mr_adm_scene_numeric_rust_tests` 的冻结参考仍用平台数学库，原有的逐角度 2e-6° 容差在以下点上不再适用：

- 方位角未定义的极点；
- 近相消后只剩 1 个 f32 ulp 的分量；
- ±180° 的回绕边界。

因此 extent 云点和旋转后的极点改为按单位方向的夹角比较，上限 2e-5°，约为单位向量的几个 f32 ulp。

- Linux 实测最大夹角 1.5e-5°。
- Windows CI 上该测试曾因 MSVC 数学库在极点给出不同但等价的方位角而失败（`8d260b8`），已由 `4343c60` 修复。

**三平台验收**

- `8d260b8`：Consistency 运行 38545398947 通过，即改用 `libm` 后三平台逐位相同。
- `4343c60`（含 FFT twiddle）：Consistency 运行 38548940341 与 CI 38548943485 通过。A/B 两种配置下：
  - 188 个 PCM id 三平台相同，197 项门禁无失败；
  - 全部 160 个内核文件三平台相同，原先唯一不同的 f64 twiddle 观察列也已一致，因此设为门禁（第 151 份）。
- 仍不同的只有离线 EAR 的检查点：f64 去相关 FIR 频谱/逆变换和 HOA 解码行。twiddle 统一后它们仍不同，
  差异来自 `mradm-ear` 自身的数学函数，属于三期；最终 FIR 与 PCM 早已逐位相同。

**回放覆盖。** `scene_math` 可移植后，`860abbf` 让 VBAP 与双耳回放在样本 6144 的策略更新加入
`position.offset`（方位 12.5°、仰角 −4°），三平台门禁随之覆盖策略中的 Cartesian→极坐标→Cartesian 换算。
Triple Balance 只接受房间立方体内的 Cartesian 位置：极坐标偏移会把距离大于 1 的回放位置移出立方体，
渲染器按设计拒绝，因此不加该项。

## 性能

在共享 4 核 Linux 虚拟机上比较切片前（`528a0d5`）与 `e432729` 的 Release 构建。离线交替运行 7 轮。

| 用例 | 耗时比 | RSS 比 | PCM |
| --- | --- | --- | --- |
| ear-extent | 1.001 | 1.010 | 相同 |
| vbap-point | 0.984 | 1.001 | 相同 |
| binaural-point | 0.991 | 1.003 | 相同 |
| binaural-cloud | 0.894 | 1.000 | 按规则改变 |
| binaural-spreader | 0.993 | 0.998 | 相同 |
| vbap-policy | 0.994 | 0.985 | 相同 |
| binaural-policy-ds | 1.001 | 1.008 | 按规则改变 |

Scene worker 延迟用 `mr_adm_scene_stream_stress` 测量，每块都更新位置。5 轮 p99 中位数：

- VBAP：124 µs → 123 µs；
- 双耳：884 µs → 824 µs。

预算为 10000 µs。没有观察到 `libm` 带来的退化。

## 剩余

- 1.3 后，Scene 空间与卷积准备链路上不再有平台数学库调用。
- HpTF 设计与 StereoPeakGuard 的释放系数仍使用 std 超越函数，属于 1.4。
- 文件流式/监听的 dB 入口属于三期。
