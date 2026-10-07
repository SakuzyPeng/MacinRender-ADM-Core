# ADR 0015：RustFFT 固定使用标量路径

> 状态：已接受。日期：2026-10-07。
> 数值一致性二期的第二个切片，接续 ADR 0014 与 [Rust 二期基线](../architecture/RUST_PHASE2_BASELINE.md)。

## 问题与依据

二期基线对 256–4096 点 `RealFft` 使用相同输入，macOS arm64 与 Linux/Windows x64 的频谱和逆变换
均有位差，两个 x64 平台之间相同。49 份 PCM 差异中，双耳与 Scene 双耳用例最早都在 HRTF
预处理 FFT（`binaural.03-hrtf.c32`）处观测到分歧。

workspace 原先保留 rustfft 默认特性（`avx`、`sse`、`neon`）。`FftPlanner` 在运行时依次尝试
AVX+FMA、SSE4.1、NEON，最后才是标量规划器；SIMD 后端对 2 的幂长度采用不同分解，AVX 与 NEON
还使用 FMA。realfft 只能经 `FftPlanner` 构造，无法单独指定内部规划器。

标量规划器只按长度选择算法，Rust 不自动收缩乘加，自动向量化也不重排浮点运算。剩余的
平台输入只有 twiddle：rustfft 与 realfft 用 f64 `cos`/`sin`（平台 libm）计算后转换为目标精度。

## 决策

- workspace 的 `rustfft` 与 `realfft` 依赖关闭默认特性，不启用任何 SIMD 特性。`FftPlanner`
  因此在所有平台落到标量规划器；不修改调用代码，不引入自研 FFT，不新增运行时后端或开关。
  影响范围：`mradm-dsp::fft::RealFft` 的全部使用者（双耳卷积、HRTF 预处理、EAR 后处理、
  afSTFT/OM spreader）、EAR 去相关滤波器的 f64 复数 FFT，以及私有 `mradm_dsp_fft_*`。
- 二期构建记录新增 `rust.fft_features`（cargo metadata 解析后的 rustfft/realfft 特性）。比较器
  拒绝任何含 `default`/`avx`/`sse`/`neon`/`wasm_simd` 的构建，防止依赖合并静默恢复 SIMD。
- 二期内核探针记录 rustfft/realfft 可能生成的全部 twiddle（每个 2 的幂长度 ≤ 32768 的每个
  索引），分别保存 f64 libm 值与转换后的 f32 值，并把 FFT 覆盖扩展到 32768 点。
- 一致性比较器新增显式位相等门禁（`scripts/consistency/phase2-gates.json`）。首批门禁为
  f32 twiddle 表、全部 `RealFft` 正/逆变换内核和 EAR 去相关 FIR；其余差异继续只做测量。

## 代价

标量 FFT 在 x64 上明显慢于 AVX 路径，端到端影响见
[二期 FFT 收敛记录](../architecture/RUST_PHASE2_FFT.md)的性能数据。本决策以跨平台确定性优先；
若后续引入向量化 FFT，必须在三个平台使用同一分解与舍入顺序、不使用 FMA，并重新通过同一门禁。

## 影响与阶段边界

x64 与 arm64 的 FFT 输出都会改变最低位，双耳、EAR 后处理和 spreader 的 PCM 随之变化，属于
本切片明确的算术规则更新。公开 C ABI、CLI、格式和分配约束不变。

twiddle 仍来自平台 libm。f32 表的三平台位相等由门禁持续验证，而不是假设；若某个 runner
镜像的 libm 改变导致门禁失败，需要改为固定表或项目自有的三角函数，而不是放宽门禁。

本切片不处理重采样（rubato 自带 SIMD 与 libm 窗函数）、spreader 分组/归约、HRTF 插值的
`hypot` 和 EAR f64 中间值差异；这些仍按二期基线记录的顺序逐项处理，不把 FFT 门禁扩展为
完整 PCM 的跨平台逐位一致承诺。
