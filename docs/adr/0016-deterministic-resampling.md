# ADR 0016：重采样固定标量插值与可移植三角函数

> 状态：已接受。日期：2026-10-07。
> 数值一致性二期的第三个切片，接续 [ADR 0015](0015-rust-fft-scalar-path.md) 与 [ADR 0011](0011-rust-fixed-rate-resampling.md)。

## 问题与依据

FFT 切片后，三平台剩余 26 个 PCM 差异中有 24 个是变采样 Scene 用例；二期内核探针里的
48→44.1 与 44.1→48 kHz 重采样输出在所有平台对上都不同，包括 Linux x64 与 Windows x64。
`rubato 5.0.1` 中有两个独立来源，均无对外开关：

1. `make_interpolator` 在运行时依次选择 AVX+FMA、SSE3、NEON 或标量 sinc 点积。各后端的归约
   顺序不同，AVX 与 NEON 还使用 FMA。
2. sinc 表在构造时由平台 libm 计算：sinc 用 `Sample::sin`（f32 时是 `sinf`），窗函数由
   `windowfunctions` crate 用 f64 `cos` 计算。glibc、UCRT 与 Apple libm 的结果相差 1 ULP，
   因此同为 AVX 的 Linux 与 Windows 也得到不同的表。

## 决策

- 照 `rust/vendor/sofar` 的先例，把 rubato 5.0.1 vendor 到 `rust/vendor/rubato`，经
  `[patch.crates-io]` 替换。补丁只改三处（见 `PATCHES.md`）：`make_interpolator` 固定返回标量
  插值器；`Sample::sin/cos` 改用 `mradm_math`；Blackman/Blackman-Harris/Hann 窗按
  `windowfunctions` 0.1.1 的周期余弦和公式与 f64 运算顺序在本地计算，只把 `cos` 换成
  `mradm_math::cos`，并移除 `windowfunctions` 依赖。算法、参数（Blackman–Harris 平方窗、
  256 基础 taps、0.94 截止、128 倍相位表、cubic 插值）、延迟约定与 `mradm_dsp::resampler`
  API 均不变。
- 新增项目内 crate `mradm-math`：musl 1.2.5 的 `sin`/`cos` 内核与中等区间 Cody–Waite
  约简（源自 FreeBSD msun），只用 IEEE 加减乘，Rust 不收缩为 FMA，因而各平台逐位一致。
  只支持 |x| < 2^20·π/2；更大参数 panic。重采样实际参数：窗函数 ≤ 8π；sinc 的 tap 数与截止频率
  随倍率反向缩放，参数上限约为 128·0.94·π ≈ 378，均远在范围内。单元测试固定 12 个输入的输出位模式，三平台默认 CI 都会校验。
- 二期内核探针新增 `trig.*`（覆盖窗/sinc 参数范围与 π/2 倍数），门禁加入 `trig.*` 与
  `resampler-*` 内核；三平台验收后按精确 id 加入变为相同的 24 个变采样 PCM 用例，三平台相同的 PCM
  达到 76/78，只剩 OM spreader 两例。

不自研重采样器：rubato 的时序、延迟和质量契约已有 ADR 0011 的独立回归与 libsamplerate 对照，
补丁只改变求值方式，范围小且可逐行审阅。

## 代价

标量 sinc 点积替代 AVX+FMA。Linux x64 上 20 秒立体声的重采样本身慢 1.6–1.8 倍，仍约为
94 倍实时；端到端影响见[二期重采样收敛记录](../architecture/RUST_PHASE2_RESAMPLER.md)。

## 影响与边界

所有平台的重采样输出最低位改变（相对原 Linux AVX 路径最大绝对差 2.4e-7），Scene 输出转换、
实时双耳的 HRTF 准备随之变化，属于本切片明确的算术规则更新。ADR 0011 的时长、延迟、相位与
质量契约保持不变，并由原有测试与参考对照继续验证。

退出条件：上游提供强制标量的开关并允许注入数学函数后，删除补丁改回 crates.io 版本，
重新通过同一门禁。`mradm-math` 可供后续需要确定性系数的模块复用（如 FFT twiddle 改为固定表时），
但每次复用都需单独验证。OM spreader 的分组/归约仍是下一个切片。
