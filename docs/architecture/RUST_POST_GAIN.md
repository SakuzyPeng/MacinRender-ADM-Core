# 离线后处理增益（二期 2.3）

> 对应 [PCM 一致性审计](PCM_CONSISTENCY_AUDIT.md)二期 2.3，审计项 N15。
> 计量本身见 [2.1 True Peak](RUST_METER_TRUE_PEAK.md) 与 [2.2 响度](RUST_METER_LOUDNESS.md)。

## 范围

离线渲染把计量结果反馈为一个增益，再整段重写输出文件。2.2 之后计量在三平台上逐位相同，
但由测量值得到增益的那一步仍在 C++，使用平台的 `std::log10` / `std::pow`：

| 位置 | 原实现 | 2.3 |
| --- | --- | --- |
| 各后端的 `measured_peak_dbtp`（EAR、HOA、扬声器 PCM、房间、双耳） | `20·std::log10(peak)` | `dsp::amplitude_to_db`（`libm::log10`），各后端原有的静音下限不变 |
| `RenderService::render` 的合成增益 | C++ 依次判断响度目标、峰值补偿、峰值上限与最终增益，`std::pow(10, dB/20)` | `dsp::render_gain`：判定与求和移入 Rust，`libm::pow` |
| 公开的 `apply_loudness_norm` | C++ 判断 0.1 LU 容差，`std::pow` | `dsp::loudness_gain` |
| 公开的 `apply_peak_limit` | C++ `std::log10` / `std::pow` 与 0.1 % 容差 | `dsp::peak_limit_gain` |
| 三处整段重写中的逐样本乘法（`apply_gain_to_file` 与上面两个 API） | C++ 循环 `buf[i] *= gain` | `mradm_dsp_apply_gain` |

Rust 实现在 `mradm-dsp` 的 `post_gain` 模块，私有 C 边界为 `src/adm_dsp/post_gain_ffi.h`，C++ 包装为 `post_gain.h`。
判定顺序、容差和边界取值逐项保留：

- 合成增益：响度目标偏差 ≥ 0.1 LU 才计入；峰值补偿 > 0.1 dB 才计入；峰值上限按 `min(0, 目标 − 峰值)`，
  < −0.1 dB 才计入；最终增益 |x| ≥ 1e-9 dB 计入且绕过峰值上限；总增益 |x| ≥ 0.01 dB 才重写文件；
- `min(0, x)` 与 `max(1e-10, peak)` 按 C++ `std::min` / `std::max` 的取值实现：NaN 与 −0 的结果与原来相同；
- 逐样本乘法不检查增益是否有限，保留原有对极端最终增益的行为；
- 日志文字与指标仍由 C++ 输出，报告的响度与峰值仍是测量值加 `gain_db`。

不在本切片内的 dB 换算：

- 对象增益覆盖与 Scene 增益的 `std::pow(10.0F, dB/20)`（`render_common.cpp`、`hoa_renderer.cpp` 与实时监听的有限性检查），
  属于离线与文件流式渲染的语义处理，留在三期；
- macOS AUSpatialMixer 后端的峰值换算，Apple 后端不在本轮范围内。

## 数值影响

Linux x64 上 glibc 与 `libm` 对锁定矩阵用到的换算给出相同结果，因此 196 个 PCM id 与 2.2 逐位相同。
锁定矩阵中走增益路径的用例：

- 原有 16 个离线用例默认启用 −1 dBTP 峰值上限，经过合成增益的峰值上限判断；
- `ear-loudness-minus23` 经过响度目标；
- `ear-peak-minus6` 经过峰值补偿与峰值上限，并在 2.1 起成为 True Peak 的端到端用例。

## 门禁

内核探针新增 `post-gain`，用固定的测量值网格调用生产代码：

| 文件 | 内容 |
| --- | --- |
| `10-conversions.f64` | 12 个幅度（含 1e-10 静音下限与 0 dBTP 附近）的 dB，14 个 dB 值（含容差边界）的幅度 |
| `20-render-plan.f64` / `21-render-gain.f32` | 4 个响度 × 4 个峰值 × 8 种阶段组合 × 3 个最终增益的合成计划与 f32 增益；测量值跨越 0.1 LU、0.1 dB 容差 |
| `30-standalone.f64` / `31-standalone-gain.f32` | `apply_loudness_norm` 的 5 个测量值，`apply_peak_limit` 的 12 个峰值 × 3 个目标 |

5 个文件全部设为门禁（模式 `post-gain.*`）。内核文件由 188 个增至 193 个，门禁由 183 个增至 188 个。

## 验证
