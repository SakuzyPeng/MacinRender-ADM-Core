# Rust 二期：OM spreader 收敛

> 决策见 [ADR 0017](../adr/0017-deterministic-spreader.md)。前序：[Rust 二期重采样收敛](RUST_PHASE2_RESAMPLER.md)、
> [Rust 二期 FFT 收敛](RUST_PHASE2_FFT.md)、[Rust 二期基线](RUST_PHASE2_BASELINE.md)。

重采样切片后，三平台 76/78 个 PCM 用例逐位相同。剩余两例 `binaural-extent-spreader`、`-multi`
在三个平台对上都最早于 `spreader/s0-g0.50-left.f32` 分歧。本切片让分组与硬件无关，并把 spreader
与 nalgebra SVD 用到的数学函数全部换成纯 Rust 实现。

## 改动

- `src/adm_render_binaural/binaural_renderer.cpp`：spreader 分组预算由 `hardware_concurrency()`
  改为固定的 `k_spreader_group_budget = 8`；worker 数仍按硬件，诊断覆盖
  `MR_ADM_DIAGNOSTIC_GROUP_BUDGET` / `MR_ADM_DIAGNOSTIC_WORKERS` 保留。
- `rust/Cargo.toml`：nalgebra 启用 `libm-force`（simba `libm_force`），新增 workspace 依赖
  `libm = "=0.2.16"`，供 `mradm-dsp` 直接使用。
- `mradm-dsp`：
  - `geometry::portable_direction(s)`（`libm::sincos`）供 spreader 使用，原 `direction(s)` 不变；
  - `voronoi_weights` 的两处 `atan2` 改为 `libm::atan2`（只有 spreader 调用）；
  - `Spreader::set_source` 的锥角阈值改为 `libm::cos`；
  - `filterbank::fir_coefficients` 的 `from_polar(gain, arg())` 改为 `libm::atan2f` / `cosf` / `sinf`，
    公式与运算顺序不变；
  - `mixing.rs` 注明 `num_complex` 的 `norm`/`sqrt` 只作用于虚部恰为 0 的协方差对角元，结果与 libm 无关。
- 依赖登记：许可证清单新增 `rust-libm-0.2.16`（MIT），SBOM、依赖表与许可证原文 bundle 已重新生成。
- 二期探针新增 4 个内核，清单为 67 项：
  - `spreader.10-input.f32`：26 方向网格、64 tap 合成 HRIR 与两路输入 PCM；
  - `spreader.20-voronoi.f32`：球面 Voronoi 权重；
  - `spreader.30-fir.c32`：133 频带 × 2 耳 × 26 方向的频带系数；
  - `spreader.40-output.f32`：两个声源在 6 块内移动并把扩散从 15°/120° 扩到 165°/345°。
  门禁新增 `om-*` 与 `spreader.*` 内核。

## Linux x64 本地验收（2026-10-07）

- 默认 Debug CTest 69/69；`mr_adm_rust_quality`、FFI 头校验（209 函数 / 65 结构体）、许可证与冻结参考校验通过。
- Release `MR_ADM_BUILD_LIBEAR_REFERENCE_TESTS=ON`：`mr_adm_libear_reference_tests`、EAR fixture/post 测试，
  以及 SAF、重采样、ebur128、libbw64 参考对照仍在原阈值内。
- 二期 A 组采集（GCC 13 Release，SOFA/IAMF OFF，vendored FLAC/Opus），对比 `a01b285`：
  - 内核：原有 63 项逐位不变（包括 `om-*` 与全部 EAR FIR），新增 4 个 `spreader.*`；
  - PCM：只有两个 spreader 用例改变（最大绝对差 0.0068 和 0.0147），其余 76 个逐位不变，EAR/HOA
    用例不受 `libm-force` 影响；
  - 同进程/新进程重复性、Scene 健康与帧数门禁通过。
- 端到端（60 秒 fixture，Release `mradm render`，5 次中位数，`a01b285` 对本改动）：VBAP 0.97×、EAR extent 1.03×、
  双耳 point 1.06×、双耳 cloud 1.02×、双耳 OM spreader（3 轨）0.99×，均在噪声范围内。

## 三平台验收

[Consistency run 37569897598](https://github.com/SakuzyPeng/MacinRender-ADM-Core/actions/runs/37569897598)
在 `0d62eeb` 上完成三平台 A/B 及诊断采集，同一提交的 CI 通过。四份报告结果一致，摘要与报告哈希见
[OM spreader 验收记录](evidence/rust-phase2/spreader-validation.json)。

| 三平台共同逐位相同的 PCM | 用例数 |
|---|---:|
| 二期基线 `00a70e4` | 29/78 |
| FFT 切片 `6ec2dc6` | 52/78 |
| 重采样切片 `0f6c187` | 76/78 |
| 本切片 `0d62eeb` | 78/78 |

- 门禁 0 失败：新增的 `om-*`、`spreader.*` 内核及此前全部门禁在三个平台对上逐位相同。
  67 个内核中只剩平台 libm 计算的 `fft-twiddles.10-libm.f64` 有 1 ULP 差异（4240–6677 个字，不设门禁）。
- 两个 spreader 用例在 macOS arm64、Linux x64、Windows x64 上逐位相同；EAR/HOA 等使用 nalgebra 的用例在
  `libm-force` 后仍全部相同。
- 每个平台内部 A 对 B 为 78/78；同进程/新进程重复性、Scene 健康、帧数、诊断无扰动与 worker/分组实验门禁通过。

### PCM 门禁

两个 spreader 用例按精确 id 加入 `phase2-gates.json`（来源注明 ADR 0017），门禁共 7 项内核与全部 78 个
PCM 用例。`phase2_tools_test.py` 现在要求 case 门禁恰好等于全部用例 id：新增一致性用例必须在同一改动中
加入门禁（或在测试中写明例外），已达成的完整位相等不会被静默绕过。
