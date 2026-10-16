# HOA 可移植数学函数与文件流式重放（三期 3.3）

> 对应 [PCM 一致性审计](PCM_CONSISTENCY_AUDIT.md)三期 3.3，审计项 N07，以及 N03 / S04 中 HOA 实时覆盖的部分。

## 范围

| 位置 | 原实现 | 3.3 |
| --- | --- | --- |
| `mradm-dsp` `hoa::coefficients::polar`：HOA3 编码方向 | `f32::sin/cos` | `libm::sinf/cosf` |
| `hoa::coefficients::extent`：extent 圆盘采样的偏移 | `f32::tan` | `libm::tanf` |
| `mradm-ear` `hoa::harmonic`：AllRAD 球谐函数 | `f64::sin/cos` | `libm::sin/cos` |
| `hoa::Decoder::new`：5200 点 t-design 的方向与极角 | `f64::sin/cos/atan2/hypot` | `libm::sin/cos/atan2/hypot` |
| `hoa::Decoder::calculate`：虚拟扬声器增益 × 采样矩阵、能量归一化 | nalgebra `DMatrix` 乘法与 `norm` | 固定顺序的标量乘积与同一累加顺序的范数 |

### AllRAD 矩阵乘积

nalgebra 打开 `std` 特性时，两个维度都大于 5 的动态矩阵乘积改调 `matrixmultiply::dgemm`。matrixmultiply 在运行时按 CPU
特性选择微内核（x86-64 的 AVX/FMA 或 SSE2、AArch64 的 NEON），累加顺序与是否融合乘加都随主机变化。EAR HOA 解码器的
`virtual_gains * Yᵀ` 与 `D * Y` 在 HOA 阶数 ≥ 2（≥ 9 个分量）且布局多于 5 个非 LFE 扬声器时进入这条路径；一阶输入（4 个分量）
与 0+5+0 仍走 nalgebra 自己的标量循环。

3.3 去掉 `hoa.rs` 中的动态矩阵：两次乘积按 nalgebra 标量 gemm 的顺序逐元素计算（对内维升序累加，每次乘法与加法分别舍入），
能量归一化按 nalgebra `dotc` 的顺序（每列 8 个累加器、两两合并、余项顺序相加，再逐列相加）。因此原来走标量循环的解码器逐位不变，
原来走 matrixmultiply 的解码器改为与主机无关的结果。`tests/portable_math.rs` 扫描 `mradm-ear` 的全部源文件，
平台初等函数或 `DMatrix` / `DVector` / `OMatrix` / `Dyn` 重新出现即失败；`mradm-dsp` 的 `tests/hoa.rs` 对 HOA 编码做同样的函数检查。

HOA 输出的响度测量使用仓库内固定的 7.1.4 解码矩阵（`hoa3_714_sn3d.f32le`），不依赖上述计算。

### HOA 实时覆盖

`HoaStream::set_overrides` 原先只按对象 id 建表：同一对象出现多条覆盖时最后一条生效，`speaker_label` 被忽略。
DirectSpeakers 床的单声道覆盖因此会作用到整个对象，标签指向其他声道的静音也会把这个声道一起静音。
3.3 改用其他后端共用的 `resolve_live_channel_gain`：按声道记录规范化的扬声器标签，匹配标签的覆盖优先于整体覆盖，与顺序无关。
`mr_adm_hoa_encode_fixture_tests` 新增回归，旧实现在“标签在前”和“其他声道静音”两项上失败。

## 数值影响（Linux x64）

内核探针分两步测量：

1. 只替换矩阵乘积：一阶解码器与 0+5+0 的全部解码器逐位不变；4+5+0、4+7+0、9+10+3 上二、三阶及稀疏混合阶的值大多改变，
   最大差 7.8e-16，即本机 matrixmultiply 内核与标量顺序的差别。
2. 再换成 `libm`：解码矩阵 4,836 个值中 2,338 个改变（最大差 2.2e-16）；HOA 编码中 extent 的 2,432 个值中 186 个改变
   （`tanf`，最大差 3.0e-8），点声源与 DirectSpeakers 编码不变。

PCM：208 个原有 PCM id 中只有 2 个改变，均来自 extent 的 `tanf`：`hoa-hoa3-cartesian`（768,000 个样本中 270,373 个，
最大差 6.0e-8）与 `scene-hoa-extent`（319,569 个，最大差 2.4e-7）；`hoa-hoa3-point`、一阶 HOA 输入的 EAR 用例
（`ear-hoa`、`ear-5_1-hoa-input`）与其余 204 个用例逐位不变。

## 新增用例

一致性夹具新增 `hoa3`（16 个分量的满三阶 HOA，48 kHz、48,000 帧）；原 `hoa` 夹具的生成结果逐字节不变。

| 用例 | 输入 | 输出 | 说明 |
| --- | --- | --- | --- |
| `ear-hoa3` | `hoa3` | EAR 7.1.4 | 三阶输入的 AllRAD 解码，原先在多数主机上走 matrixmultiply |
| `hoa-loudness-minus23` | `objects-extent-multi` | HOA3 | 经 7.1.4 测量解码的响度归一化 |
| `stream-hoa-extent-*` | `objects-extent-multi` | HOA3 | 对象增益 −3 / −14.5 / +2.25 / −6 dB、seek、静音、清空 |
| `stream-hoa-bed-*` | `directspeakers` | HOA3 | 整体与按扬声器标签的增益、静音，seek |
| `stream-ear-hoa3-*` | `hoa3` | EAR 7.1.4 | seek（HOA 包没有可供覆盖的 AudioObject） |

两个离线用例记在 `offline_cases()` 末尾并标注 `slice: 3.3`，不改变二期结项冻结的原始参数集。文件流式重放沿用 3.2 的规则：
固定与碎片分块分别成例，同进程两遍、新进程与两种分块逐位相同，每个用例交付 74,200 帧。PCM 门禁由 208 个增至 216 个。

## 内核门禁

| 文件 | 内容 |
| --- | --- |
| `hoa-encode.10-point.f32` | 方位每 7.5°、九个仰角的点声源对象编码（direct 与 3 组 diffuse） |
| `hoa-encode.20-extent.f32` | 极坐标与 Cartesian 位置 × 6 组宽 / 高 / 深、diffuse 0.25，以及三源 divergence |
| `hoa-encode.30-direct.f32` | 同一方向网格的 DirectSpeakers 编码 |
| `ear-hoa.10-decode.f64` | 0+5+0、4+5+0、4+7+0、9+10+3 上一至三阶 × SN3D / N3D / FuMa 的解码矩阵，以及稀疏混合阶 |

4 个文件全部设为门禁（模式 `hoa-encode.*`、`ear-hoa.*`）。内核文件由 220 个增至 224 个，门禁由 215 个增至 219 个。

## 验证

**本机（Linux x64，config A）。** 与 3.2 的采集相比，原有 208 个 PCM id 中 206 个逐位相同，2 个 HOA extent 用例的变化见上文；
原有 220 个内核文件全部不变，4 个 HOA 内核文件为新增；原 `hoa` 夹具逐字节不变。18 个文件流式重放的同进程两遍、新进程与
固定/碎片分块比较全部相同；HOA 重放的事件按预期生效（−3 dB 段 W / Y / X 的 RMS 比均为 0.708，按扬声器标签的覆盖使各球谐分量
按方向分别改变）。比较器自检 216 个 PCM id、233 项门禁无失败。ctest 79 项全部通过（含新增的 HOA 按声道覆盖回归）；
Rust fmt / clippy、两项源码扫描测试、许可证校验（`NOTICE.txt` 已同步）、clang-format 与 clang-tidy 无告警。

**三平台验收。** `5c00c47` 的 Consistency 38868898890、CI 38868901777 与 Quality 38868905213 通过：

- A/B 两种配置下 216 个 PCM id 三平台相同，包括 18 个文件流式重放（6 个 HOA）、`ear-hoa3` 与 `hoa-loudness-minus23`；233 项门禁无失败；
- 224 个内核文件全部三平台相同，包括 4 个 HOA 内核文件；
- 诊断检查点 74,376 个全部三平台相同：3.2 时仍不同的 22 个一阶 HOA 解码行 `ear.hoa-decode-row-*` 改用 `libm` 后不再不同，
  诊断运行首次没有任何不同的检查点。
