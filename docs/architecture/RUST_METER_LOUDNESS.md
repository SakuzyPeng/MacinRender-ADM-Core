# 响度计量（二期 2.2）

> 对应 [PCM 一致性审计](PCM_CONSISTENCY_AUDIT.md)二期 2.2，审计项 N13 与 S01。
> True Peak 部分见 [2.1 记录](RUST_METER_TRUE_PEAK.md)，计量的整体迁移见 [Rust Meter 迁移记录](RUST_METER_MIGRATION.md)。

## 范围

生产计量使用 vendored ebur128 0.1.10（`rust/vendor/ebur128`）。项目只用 momentary、short-term 和 integrated，
不开启直方图模式与 loudness range。2.1 后，响度链路仍有四处调用平台数学库，并有一处与架构有关的状态处理：

| 位置 | 原实现 | 2.2 |
| --- | --- | --- |
| `filter_coefficients`：K-weighting 高架与高通 | `f64::tan`、`f64::powf` | `libm::tan`、`libm::pow` |
| `history.rs`：−20 dB 与 −10 dB 门控系数 | `f64::powf(10, x)` | `libm::pow` |
| `energy_to_loudness`：能量转 LUFS | `f64::log10` | `libm::log10` |
| `Filter::process` / `seed` 的状态清理（S01） | x86/SSE2 在处理期间打开 MXCSR flush-to-zero，不清理；其他架构不开 FTZ，每次调用后把绝对值低于 `f64::EPSILON` 的滤波器状态清零 | 所有架构都不开 FTZ，都在每次调用后清零 |

直方图分档表、块能量求和顺序、门控顺序和 True Peak 都不变。补丁细节记录在 `rust/vendor/ebur128/PATCHES.md`。

**为什么统一到清零而不是 FTZ。** 清零规则只用 IEEE 比较与赋值，在三个平台上天然一致；FTZ 要改线程浮点控制寄存器，
ARM 上没有对应的等价设置。去掉 FTZ 后，非正规数在三个平台上都按 IEEE 计算。代价是清零发生在每次 `add_frames`
调用之后，极低电平下的结果依赖送入的分块；这与 ARM 原有行为相同，生产与探针的分块在各平台上一致。

## 数值影响

只有极低电平受清零规则影响：滤波器状态低于 `f64::EPSILON`（约 2.2e-16）时，对应的输出远低于 −70 LUFS 的绝对门限，
不进入 integrated，也不进入响度归一化的增益。momentary 与 short-term 读数在这种电平下会改变，但只用于显示。

`libm` 与平台库给出不同结果时，系数或 LUFS 会差最后几位。响度归一化把 integrated 送入 C++ 增益计算（N15，属于 2.3），
所以这一层的差异理论上可以进入 PCM；本切片的验证给出实际影响。

## 门禁

内核探针的计量项（2.1 引入）做了两处调整：

- 每个采样率新增 `meter-{rate}.05-k-weighting.f64`：生产代码在该采样率下使用的 5 个分子与 5 个分母系数。
  为此 vendored ebur128 导出 `k_weighting_coefficients`，`mradm-dsp` 以 `meter::k_weighting` 转出，不改变数值；
- 节目的末段由 0.5 秒静音改为 1 秒 2^-80 的直流，momentary 与 short-term 在响亮段末和节目末各读一次。
  直流使 K-weighting 状态稳定在 `f64::EPSILON` 以下：按块清零时每块都重新产生一次阶跃响应，不清零时输出趋于稳定。
  旧实现在 Linux x64 上末段 momentary 约为 −610 至 −665 LUFS，统一清零后约为 −490 LUFS。

| 文件 | 内容 |
| --- | --- |
| `05-k-weighting.f64` | 分子 b0–b4、分母 a0–a4 |
| `10-parameters.i32` | 采样率、声道数、帧数、分块 |
| `20-true-peak.f64` | 每声道 True Peak |
| `30-loudness.f64` | 响亮段末与节目末的 momentary、short-term，以及两种模式的 integrated |

全部计量文件设为门禁（模式 `meter-*`）。内核文件由 184 个增至 188 个，门禁由 171 个增至 183 个；
其余 5 个观察项为 EAR 布局 3 个与 Scene 2 个。

## 验证

| 提交 | 内容 | Linux x64 config A 相对 2.1 |
| --- | --- | --- |
| `c4fa475`、`df34ded` | 内核探针新增 K-weighting 检查点，末段改为 2^-80 直流；生产数值不变 | — |
| `56f1ded` | 响度链路改用 `libm`，统一按块清零，计量文件全部设为门禁 | 196 个 PCM id 与 2.1 逐位相同；True Peak 与其余内核文件不变，只有计量的帧数与响度读数变化 |

本机发布配置 ctest 78/78 通过；`cargo fmt --check`、clippy `-D warnings` 与许可证校验通过。

**本机定位。** Linux x64 上，`libm` 与 glibc 的 `tan`/`pow` 给出相同的 K-weighting 系数；改用 `libm` 后只有 3 份响度文件中的
4 个读数变化 1 ulp，其中包括不经过门控的 short-term，因此来自 `log10`。末段 momentary 从约 −610 至 −665 LUFS（FTZ 路径，不清零）变为约 −490 LUFS（按块清零），
说明探针确实覆盖了 S01。

**三平台。** `56f1ded` 的 Consistency 38742360010 与 CI 38742364325 通过：

- A/B 两种配置下 196 个 PCM id 三平台相同，208 项门禁无失败；
- 188 个内核文件全部三平台相同，包括 28 个计量文件和 5 个观察项；
- 诊断运行每对平台比较 19518 个 Scene 检查点，全部相同。
