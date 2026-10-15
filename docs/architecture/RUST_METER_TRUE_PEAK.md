# True Peak 插值系数（二期 2.1）

> 对应 [PCM 一致性审计](PCM_CONSISTENCY_AUDIT.md)二期 2.1，审计项 N14。
> 计量的整体迁移见 [Rust Meter 迁移记录](RUST_METER_MIGRATION.md)。

## 范围

生产计量使用 ebur128 0.1.10。True Peak 在 96 kHz 以下做 4 倍过采样，192 kHz 以下做 2 倍，更高采样率直接取样本峰值。
两种过采样共用一个 48 抽头的插值滤波器，系数在 `InterpF::new` 中生成：

- Hanning 窗：`0.5·(1 − cos(2πj/48))`；
- sinc：`sin(mπ/F)/(mπ/F)`，`m = j − 24`，`F` 为过采样倍数；
- 以 f64 计算，转为 f32 保存。

改动前这两处调用平台 C 库的 `cos`/`sin`，是审计中的 N14。2.1 只改这一处：

| 位置 | 改动 |
| --- | --- |
| ebur128 `InterpF::new` | `f64::cos`/`f64::sin` 改为 `libm::cos`/`libm::sin`，其余表达式与 f32 转换不变 |
| 累加 | 不变：`*self += other * coeff`。Rust 默认不融合乘加，项目也不启用 `precision-true-peak` 特性（其中的 `mul_add` 不在生产路径） |
| 过采样选择、抽头数、峰值取绝对值与最大值 | 不变 |

ebur128 因此纳入 `rust/vendor/ebur128`，做法与 RealFFT、RustFFT、rubato 相同：

- `PATCHES.md` 说明改动，`PROVENANCE.json` 记录归档哈希、上游提交和每个未改动文件的规范化哈希；
- 许可证清单与生成的依赖表注明了补丁。

响度部分（K-weighting 的 `tan`/`powf`、LUFS 的 `log10`、门控和极小状态清理）属于 2.2，本切片不改。

## 数值影响

在 Linux x64 上，glibc 与 `libm` 对这 96 个系数（4 倍和 2 倍各 48 个）给出的 f64 值逐位相同，f32 自然也相同。
因此补丁在 Linux 上不改变任何计量结果或 PCM；macOS 与 Windows 是否原本就相同，由新增门禁在三平台上确认。

True Peak 只在以下情况影响 PCM：

- 离线峰值限制与峰值归一化（`apply_peak_limit`），锁定矩阵中为 `ear-5_1-point-postproc` 和 `ear-peak-minus6`；
- 响度归一化后的峰值检查。

峰值转 dB 与增益计算仍在 C++（N15），属于 2.3。

## 门禁

内核探针新增 `meter-{rate}-{channels}`，用生产 `Meter` 测量确定的二进有理数输入：

- 采样率 44.1、48、96、192 kHz，覆盖 4 倍、2 倍和不过采样；
- 2 声道走专用扫描器，12 声道走通用路径；
- 信号：1 秒峰值 0.25 的噪声，0.5 秒低 24 dB，0.5 秒静音。第 0 声道前半秒是 fs/4 序列 0.5, 0.5, −0.5, −0.5，
  理论 True Peak 为 0.5·√2，高于噪声，用来测试插值滤波器；
- 每 4800 帧送入一次。

| 文件 | 内容 | 门禁 |
| --- | --- | --- |
| `10-parameters.i32` | 采样率、声道数、帧数、分块 | 是 |
| `20-true-peak.f64` | 每声道 True Peak | 是 |
| `30-loudness.f64` | 响亮段末尾的 momentary、short-term，及两种模式的 integrated | 否，观察到 2.2 |

内核文件由 160 个增至 184 个，门禁由 155 个增至 171 个；观察项为 EAR 布局 3 个、Scene 2 个、响度 8 个。

## 验证

<!-- 待补 -->
