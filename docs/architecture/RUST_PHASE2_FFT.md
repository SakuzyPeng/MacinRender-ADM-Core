# Rust 二期：FFT 收敛

> 决策见 [ADR 0015](../adr/0015-rust-fft-scalar-path.md)。前序：[Rust 二期基线](RUST_PHASE2_BASELINE.md)。

二期基线确认 FFT 是第一个可独立收敛的分歧边界：5 种长度的 `RealFft` 探针输入相同，
macOS arm64 与 x64 的频谱/逆变换有位差，Linux 与 Windows x64 相同；双耳和 Scene 双耳用例
最早在 HRTF 预处理 FFT 处观测到分歧。本切片让 FFT 在三平台走同一条标量路径，并把它变成
一致性 CI 的硬门禁。

## 改动

- `rust/Cargo.toml`：`rustfft` 与 `realfft` 关闭默认特性。两者都要关闭，否则 realfft 的
  `default` 会经依赖合并重新打开 rustfft 的 `avx`/`sse`/`neon`。`FftPlanner` 因此只能落到
  `FftPlannerScalar`。版本、`Cargo.lock`、许可证清单和调用代码均不变。
- 作用范围：`mradm-dsp::fft::RealFft`（双耳 `LiveConvolver`/`OlaConvolver`、`hrtf_filters`、
  `ear_post`、afSTFT `filterbank` 与 OM spreader）、`mradm-ear` 去相关滤波器的 f64 复数 FFT、
  私有 `mradm_dsp_fft_*`。准备阶段之后仍不分配内存（`realtime_allocations` 测试不变）。
- 构建记录：`build_info.py` 新增 `rust.fft_features`（cargo metadata 解析后的 rustfft/realfft
  特性），`rust.fft_dispatch` 记为标量规划器。比较器要求两项特性集合都不含
  `default`/`avx`/`sse`/`neon`/`wasm_simd`，且三平台一致。
- 内核探针（`mr_adm_phase2_kernels`）：
  - `fft-twiddles.10-libm.f64` / `fft-twiddles.20-table.f32`：rustfft/realfft 用同一公式
    `cos/sin(-2π/len·k)` 可能生成的全部 twiddle，len 为 2–32768 的每个 2 的幂，k 取 0..len。
    标量 FFT 只由 IEEE 基本运算和这些常量决定；f64 列用于观察 libm 本身的差异。
  - FFT 正/逆变换扩展到 8192、16384、32768 点。
  - OM 循环结束后清空诊断 scope，重采样与 HpTF 不再记在 `om-3` 名下。
  - 内核清单由 `phase2_common.kernel_outputs()` 显式列出（60 项），比较器逐项核对，
    不再只数文件个数。
- 位相等门禁：`scripts/consistency/phase2-gates.json`（`mradm.phase2.gates.v1`）列出必须在
  每个平台对之间逐位相同的内核或 PCM 用例，支持通配符；门禁项有差异或一个都匹配不到时，
  `compare-rust-phase2.py` 写出报告后返回非零。首批门禁：
  - `fft-twiddles.20-table.f32`
  - `fft-[0-9]*`（全部 `RealFft` 输入、频谱和逆变换）
  - `ear-*.20-fir.f32`（同一依赖改动也作用于 EAR 去相关 FFT；基线中已三平台相同）

  其余内核与 PCM 差异继续只做测量。Consistency CI 的 `compare` job 先写完四份报告再按门禁失败。

## Linux x64 本地验收（2026-10-07）

GCC 13 Release，配置 A（`MR_ADM_STRICT_FP=OFF`，IAMF/SOFA OFF，vendored FLAC/Opus），
同一台机器分别采集 `e29389f`（自动 SIMD）与本改动：

- 两次采集均通过输入完整性、同进程/新进程重复性、Scene 健康和帧数门禁；本改动的
  `rust.fft_features` 为 `{"realfft": [], "rustfft": []}`。
- 内核：只有 5 种原有 FFT 长度的频谱与逆变换改变（AVX 改为标量，频谱最大绝对差
  1.9e-5，逆变换 5.4e-7）；Scene 数学、EAR FIR、OM、重采样、HpTF 内核逐位不变。
- PCM：78 份中 45 份改变，均是使用 FFT 的路径——双耳 offline 5 份、EAR 4 份（EAR 后处理 FFT）、
  Scene 双耳 24 份、Scene 设备 DSP 4 份、Scene 立体声 VBAP 的 epoch1 6 份（脚本在 8192 样本切到
  双耳）、`scene-binaural-cloud`/`-rotated` 2 份。除 spreader 外最大绝对差 7.2e-7；
  `binaural-extent-spreader` 与 `-multi` 分别为 0.013 和 0.033。OM spreader 对上游微小差异的
  放大与基线中 Linux/Windows 因分组不同出现的 0.015–0.028 差异同量级，属于后续 spreader 切片。
  不涉及 FFT 的 VBAP、HOA、Triple Balance 及其余 EAR 用例逐位不变。
- 默认 Debug CTest 69/69；`mr_adm_rust_quality`（含 diagnostics 特性的 example clippy）、
  `mr_adm_ffi_header_check`（209 函数 / 65 结构体）、许可证、冻结参考校验通过；
  `mr_adm_phase2_tool_tests` 新增特性拒收、内核清单与门禁判定用例。

### 性能

FFT 单独测量（Linux x64，AVX2/FMA/AVX-512 CPU，rustfft 6.4.1 复数正变换，f32，best-of-7）：

| 复数长度 | 自动 SIMD (ns) | 标量 (ns) | 倍数 |
|---:|---:|---:|---:|
| 128 | 76 | 423 | 5.6 |
| 512 | 387 | 2280 | 5.9 |
| 1024 | 934 | 5669 | 6.1 |
| 2048 | 2255 | 12787 | 5.7 |
| 4096 | 6208 | 28721 | 4.6 |
| 16384 | 37122 | 139783 | 3.8 |

`RealFft` N 点内部是 N/2 点复数 FFT 加线性前后处理，生产中最常见的 2048/4096 实数 FFT
对应 1024/2048 复数长度。端到端：把 `mr_adm_make_fixture` 生成的 1 秒 ADM fixture 的 PCM 重复为
60 秒（元数据为静态块），Release `mradm render` 输出 WAV，各 5 次取中位数：

| 用例 | 渲染器 | `e29389f` (s) | 本改动 (s) | 倍数 |
|---|---|---:|---:|---:|
| vbap-point（对照，不用 FFT） | VBAP 5.1.4 | 0.834 | 0.833 | 1.00 |
| ear-extent | EAR 5.1 | 0.632 | 0.733 | 1.16 |
| binaural-point | 双耳 | 0.359 | 0.432 | 1.20 |
| binaural-cloud | 双耳 cloud | 1.287 | 1.788 | 1.39 |
| binaural-spreader（3 轨） | 双耳 OM spreader | 2.065 | 2.115 | 1.02 |

最慢的双耳 cloud 仍约为 33 倍实时。arm64 原 NEON 路径的代价需由 macOS 实测补充。

## 三平台验收

（待 Consistency CI 结果补充：三平台 FFT/twiddle 门禁、PCM 变为相同的用例，以及据此新增的 PCM 门禁。）

## 剩余分歧

本切片不改变：重采样（rubato 自带运行时 SIMD 与 libm 窗函数，Linux/Windows 也不同）、
OM spreader 的分组/归约与对上游差异的放大、HRTF 插值中的 `hypot`、EAR 去相关 f64 中间值
（最终 f32 FIR 相同）。twiddle 仍来自平台 libm，三平台位相等由门禁持续验证。
