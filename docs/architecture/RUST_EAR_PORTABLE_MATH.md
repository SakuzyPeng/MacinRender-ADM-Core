# EAR 可移植数学函数与文件流式重放（三期 3.2）

> 对应 [PCM 一致性审计](PCM_CONSISTENCY_AUDIT.md)三期 3.2，审计项 N06、N08 剩余部分与 S07（EAR 部分）。

## 范围

`mradm-ear` 移植自 libear，几何、extent 权重、立体声 panner 与去相关随机相位直接调用 Rust 固有方法
`f64::sin` / `cos` / `atan2` / `hypot` / `powf` 与 `f32::acos` / `asin`。这些方法调用平台 C 库；工作区给 nalgebra
打开的 `libm-force` 只改写 nalgebra/simba 内部的标量函数，不改写源码中的固有方法（N06、N08）。3.2 把它们全部换成
`libm` 0.2.16 的纯 Rust 实现：

| 位置 | 原实现 | 3.2 |
| --- | --- | --- |
| `geom::cart`：ADM 极坐标转方向（扬声器、对象、extent 采样网格与权重基） | `f64::sin/cos` | `libm::sin/cos` |
| `geom::azimuth/elevation`：方向转极坐标（extent 权重） | `f64::atan2/hypot` | `libm::atan2/hypot` |
| `geom::vertex_order`：quad 与虚拟扬声器多边形的顶点排序 | `f64::atan2` | `libm::atan2` |
| `Extent::new`：每行 extent 采样点数 | `f64::cos` | `libm::cos` |
| `extent_mod`：按距离修正 extent | `f64::atan2` | `libm::atan2` |
| `Weight::new/cosine/weight`：extent 权重的起止角与边缘衰减 | `f64::sin/cos`、`f32::acos/asin` | `libm::sin/cos`、`libm::acosf/asinf` |
| `Panner::gains`：0+2+0 由 0+5+0 下混时的前后衰减 | `f64::powf` | `libm::pow` |
| `decorrelate::filters`：去相关 FIR 的随机相位 | `f64::cos/sin` | `libm::cos/sin` |

`sqrt` 在 IEEE 754 中要求正确舍入，各平台结果相同，保持不变；Rust 不会把 `a * b + c` 合成乘加，
原有算术顺序也不变。EAR HOA 解码（`hoa.rs` 的球谐函数与采样方向）属于审计项 N07，留到 3.3。
`tests/portable_math.rs` 扫描上述源文件，若重新出现平台初等函数则失败。

## C++ 侧

EAR 渲染器在 C++ 中剩下的增益运算都是单独的一次加法或乘法：多个 divergence 源的增益逐通道相加
（`bg.gains[out] += direct[out]`），DirectSpeakers 与 HOA 解码行乘以块增益，对象增益为两个 f32 转 f64 后相乘（结果精确）。
这些表达式没有可融合的 `a * b + c`，结果与编译器是否允许乘加融合无关，因此留在 C++。坐标、channelLock 与 divergence
预处理已在 Rust `scene_math`（一期 1.2、1.3）；混音、实时增益平滑与后处理在 Rust `PcmMixer`、`GainBank` 与 `EarPostProcessor`。

## 数值影响

在 Linux x64 上，`libm` 与 glibc 对本轮用到的输入大多给出相同结果，个别值不同：

- 内核探针：点声源增益 51,840 个值中 460 个改变，最大差 2.2e-16；extent 增益 6,720 个值中 498 个改变，最大差 8.4e-8
  （来自 f32 的 `acosf` / `asinf`）；DirectSpeakers 增益与三份去相关 FIR 不变。
- PCM：202 个原有 PCM id 中只有 3 个使用 extent 的 EAR 离线用例改变（`ear-5_1-extent` 288,000 个样本中 44,180 个、
  `ear-extent` 768,000 个中 79,767 个、`ear-window` 89,856 个中 6,761 个，最大差 1.04e-7，峰值约 2.1）；
  点声源、DirectSpeakers、HOA 输入与其余后端的 199 个用例逐位不变。EAR 文件流式重放中 point 与 bed 在迁移前后逐位相同，
  extent 的 1,187,200 个样本中 119,218 个改变，最大差 8.9e-8。

macOS arm64 与 Windows x64 的平台数学库此前可能在其他输入上与 glibc 不同；三平台原有 EAR 用例此前已经逐位相同，
3.2 之后结果不再依赖平台库。

## 文件流式重放

重放工具 `mr_adm_phase2_stream` 改经渲染器工厂（`resolve_renderer` / `normalize_output_layout`，与 `RenderService`
和 Monitor 共用）创建后端，用例版本 3 记录渲染器、请求的输出布局与声道数。6 个双耳用例的输入、事件与输出不变。
新增 6 个 EAR 用例：

| 用例 | 输入 | 输出布局 | 事件 |
| --- | --- | --- | --- |
| `stream-ear-point-*` | `objects-point` | 7.1.4（12 声道） | 对象增益 −3 / −14.5 / +2.25 / −6 dB、seek、静音、清空 |
| `stream-ear-extent-*` | `objects-extent-multi` | 9.1.6（16 声道） | 同上 |
| `stream-ear-bed-*` | `directspeakers`（5.1） | 7.1.4（12 声道） | 整体增益，`M+030` / `M+000` / `M-030` 单声道增益与 `M-110` 静音（按扬声器标签覆盖，优先于整体覆盖）、seek、整体静音、清空 |

固定与碎片分块分别成例。事件落在固定输出帧，seek 位于第 31,000 帧并回到第 4,800 帧，每个用例交付 74,200 帧；
同进程两遍、新进程与两种分块的结果都要求逐位相同。PCM 门禁由 202 个增至 208 个。

## 内核门禁

内核探针新增 `ear-gains` 模块（f64）：

| 文件 | 内容 |
| --- | --- |
| `10-point.f64` | 0+2+0、0+5+0、4+5+0、4+7+0、9+10+3 五种布局，方位每 3.75°、十个仰角（含 ±90°）的点声源增益 |
| `20-extent.f64` | 0+5+0、4+7+0、9+10+3 上 5 个方向 × 2 个距离 × 8 组宽 / 高 / 深的 direct 与 diffuse 增益 |
| `30-direct.f64` | 同三种布局上无标签 DirectSpeakers 的位置匹配（无边界与 ±15° / ±10° 边界），含 panner 回退 |

3 个文件全部设为门禁（模式 `ear-gains.*`）。内核文件由 217 个增至 220 个，门禁由 212 个增至 215 个。

## 验证

**本机（Linux x64，config A）。** 与 3.1 的采集相比，202 个原有 PCM id 中 199 个逐位相同，3 个 extent 用例的变化见上文；
6 个双耳文件流式重放不变。原有 217 个内核文件全部不变，3 个 `ear-gains` 文件为新增。12 个文件流式重放的同进程两遍、
新进程与固定/碎片分块比较全部相同；EAR 重放的事件按预期生效（−3 dB 段的 RMS 比 0.708，`M+030` 在整体 −1.5 dB 上
单独 −12 dB 时为 0.253，`M-110` 静音后降至 0.018）。比较器自检 208 个 PCM id、223 项门禁无失败。ctest 79 项全部通过；
Rust fmt / clippy、`tests/portable_math.rs`、许可证校验（`NOTICE.txt` 已同步到 bundle）、clang-format 与 clang-tidy 无告警。
