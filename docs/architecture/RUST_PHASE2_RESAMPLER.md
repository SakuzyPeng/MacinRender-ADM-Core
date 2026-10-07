# Rust 二期：重采样收敛

> 决策见 [ADR 0016](../adr/0016-deterministic-resampling.md)。前序：[Rust 二期 FFT 收敛](RUST_PHASE2_FFT.md)、
> [Rust 二期基线](RUST_PHASE2_BASELINE.md)。

FFT 切片后剩余 26 个三平台 PCM 差异中，24 个是变采样 Scene 用例（44.1→48 kHz 在重采样后的 HRIR、
48→44.1 kHz 在输出重采样处最早分歧），重采样内核本身在三个平台对上都不同。rubato 5.0.1 在运行时
选择 SIMD 点积，并用平台 libm 生成 sinc 与窗函数表；两者都没有对外开关。本切片用 vendored 补丁
固定标量点积，并把三角函数换成项目内可移植实现。

## 改动

- `rust/crates/mradm-math`（新，`#![forbid(unsafe_code)]`，无依赖）：musl 1.2.5 `__sin`/`__cos`
  内核与 `__rem_pio2` 中等区间约简的逐行移植（源自 FreeBSD msun；`NOTICE.txt` 保留 Sun 声明，
  `PROVENANCE.json` 记录上游文件 SHA-256）。只用 IEEE 加减乘，常量以位模式写入。
  支持 |x| < 2^20·π/2，NaN/∞ 返回 NaN，超范围 panic。
  测试：约 10 万个点上与平台 libm 相差 ≤ 2 ULP；±0、π/2 倍数邻域；常量与 musl 十进制字面量一致；
  12 个输入的输出位模式固定，三平台默认 CI 都会执行。
- `rust/vendor/rubato`（crates.io 5.0.1，归档 SHA-256 `cc1e951b…0930`）经 `[patch.crates-io]` 替换，
  补丁见 `PATCHES.md`：
  - `make_interpolator` 固定返回标量插值器（8 个累加器、固定的最终求和顺序）；
  - `Sample::sin/cos` 使用 `mradm_math`（f32 时 f64 结果只舍入一次），即 sinc 表的正弦；
  - Blackman/Blackman-Harris/Hann 窗按 `windowfunctions` 0.1.1 的周期余弦和公式与 f64 运算顺序
    在本地计算，只把 `cos` 换成 `mradm_math::cos`；移除 `windowfunctions` 依赖。
  - 未选用的 SIMD 模块保留在源码中，`lib.rs` 允许 `dead_code`/`unused_imports`。
- `mradm_dsp::resampler` 的参数、延迟约定、API 和分配约束不变。
- 依赖登记：许可证清单删除 `windowfunctions`，rubato 条目注明本地补丁，新增本地组件
  `musl-derived-rust-math`；SBOM、依赖表与许可证原文 bundle 已重新生成，`check_cargo.py` 把
  `mradm-math` 视为项目内 crate。
- 二期探针新增 `trig.10-input.f64`、`trig.20-sin.f64`、`trig.30-cos.f64`（窗参数 ≤ 8π、sinc
  参数约 380 以内及更大范围、π/2 倍数），内核清单为 63 项。门禁新增 `trig.*` 与 `resampler-*`。

## Linux x64 本地验收（2026-10-07）

- 默认 Debug CTest 69/69（含 `mradm-math` 位模式与精度测试、`mradm-dsp` 重采样阈值与分配计数测试）；
  `mr_adm_rust_quality`（含 diagnostics 特性的 example clippy）、FFI 头校验（209 函数 / 65 结构体）、
  许可证与冻结参考校验通过。
- Release `MR_ADM_BUILD_SAMPLERATE_REFERENCE_TESTS=ON`：`mr_adm_resampler_reference_tests` 对
  libsamplerate 的通带、阻带、短 HRIR 与多声道对照仍在原阈值内。
- 二期 A 组采集（GCC 13 Release，SOFA/IAMF OFF，vendored FLAC/Opus），对比 `2baaf94`：
  - 内核只有 `resampler-48000-44100` 与 `resampler-44100-48000` 输出改变（最大绝对差 3.6e-7），
    其余 60 项不变；新增 3 个 `trig.*` 内核。
  - PCM 只有 24 个变采样 Scene 用例改变（最大绝对差 6.0e-8），其余 54 个逐位不变。
  - 同进程/新进程重复性、Scene 健康与帧数门禁通过。
- 离线 CLI 没有输出采样率转换，端到端代价只出现在 Scene/实时路径与 HRTF 准备，以下用重采样本身计时。

### 性能

同一程序同时链接 crates.io 版与补丁版 rubato，使用生产参数（Blackman–Harris 平方窗、过采样 128、
cubic、截止 0.94，48→44.1 kHz 为 280 taps、44.1→48 kHz 为 256 taps），处理 20 秒立体声，3 次取最好：

| 转换 | 原版（AVX+FMA）(s) | 补丁版（标量）(s) | 倍数 | 补丁版实时倍率 |
|---|---:|---:|---:|---:|
| 48→44.1 kHz | 0.116 | 0.214 | 1.84 | 约 93× |
| 44.1→48 kHz | 0.131 | 0.211 | 1.62 | 约 95× |

两版输出最大绝对差 2.4e-7（约 1–2 个 f32 ULP），差异来自点积归约顺序、FMA 与 libm 系数。
按 ADR 0011，重采样用于 Scene 空间渲染后的输出采样率转换与实时双耳准备期的 HRTF 转换；输入与输出采样率相同时直接复制，不经过 sinc 插值。

## 三平台验收

[Consistency run 37562888092](https://github.com/SakuzyPeng/MacinRender-ADM-Core/actions/runs/37562888092)
在 `0f6c187` 上完成三平台 A/B 及诊断采集，同一提交的 CI 与 Quality 均通过。四份报告结果一致，摘要、
报告哈希与剩余用例的首个分歧点见 [重采样验收记录](evidence/rust-phase2/resampler-validation.json)。

| 三平台共同逐位相同的 PCM | 用例数 |
|---|---:|
| 二期基线 `00a70e4` | 29/78 |
| FFT 切片 `6ec2dc6` | 52/78 |
| 本切片 `0f6c187` | 76/78 |

- 门禁 0 失败：`trig.*` 可移植三角函数、两个变采样 `resampler-*` 内核及此前的 FFT/EAR FIR 门禁在
  三个平台对上均逐位相同。63 个内核中只剩平台 libm 计算的 `fft-twiddles.10-libm.f64` 仍有 1 ULP
  差异（不设门禁，仅用于观察 libm）。
- 新增三平台相同的 24 个 PCM 用例正是全部变采样 Scene：双耳 cloud0/cloud1 与立体声 VBAP 的
  48→44.1、44.1→48 kHz，fixed/fragmented 分块，两个 epoch。
- 每个平台内部 A 对 B 仍为 78/78；同进程/新进程重复性、Scene 健康、帧数与诊断无扰动门禁通过。

### PCM 门禁

这 24 个用例按精确 id 加入 `phase2-gates.json`（来源注明 ADR 0016），门禁共 5 项内核与 76 个 PCM
用例。单元测试仍禁止把 OM spreader 用例列入门禁。

## 剩余分歧

剩余 2 个差异均为 OM spreader（`binaural-extent-spreader`、`-multi`），三个平台对都最早于
`spreader/s0-g0.50-left.f32` 分歧，属于下一个切片：固定与硬件线程数无关的分组和归约顺序。
