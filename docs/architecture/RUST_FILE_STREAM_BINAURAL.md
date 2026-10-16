# 文件流式双耳与监听适配（三期 3.1）

> 对应 [PCM 一致性审计](PCM_CONSISTENCY_AUDIT.md)三期 3.1，审计项 S07（双耳部分）、N18 与 N03（实时覆盖增益）。

## 范围

一期与二期之后，文件流式渲染和实时监听适配里仍有几处在 C++ 中直接决定样本值或滤波控制参数的浮点运算。
生产 C++ 不关闭乘加融合：macOS arm64 上的 clang 默认允许把同一表达式里的 `a * b + c` 合成一条 FMA，
Linux x64 与 Windows x64 的构建则分别舍入，因此这些表达式在三个平台上没有统一的算术规则（S07）。
另有两处使用平台的 `sinf` / `cosf` / `powf`。3.1 把它们移入 Rust：

| 位置 | 原实现 | 3.1 |
| --- | --- | --- |
| `BinauralStream::render_current_sources`：逐源乘以实时增益包络后求和 | C++ `output += sample * gain` | `dsp::accumulate_stereo`（`mradm-dsp` `stream_dsp`，分别舍入） |
| `BinauralStream::render_block`：拓扑切换时新旧两路的线性淡化 | C++ `old * (1 - mix) + new * mix` | `dsp::topology_crossfade`（同一公式，分别舍入） |
| `extent_spread_deg`：双耳 spreader 锥角与点源旁路门限（离线与文件流式共用） | C++ `width * 60 + depth * 20` 等 | `dsp::extent_spread_degrees`（同一公式与 `std::max` 取值，分别舍入） |
| `stereo_pan_for_azimuth`：Monitor 把扬声器布局折叠到立体声耳机的矩阵 | `std::sin` / `std::cos`（f32） | `monitor::stereo_fold`（`libm::sinf` / `cosf`） |
| `resolve_live_channel_gain`：实时覆盖增益的 dB 换算（EAR、VBAP、Triple Balance 与双耳文件流式共用） | `std::pow(10.0F, dB / 20)` | `dsp::live_gain`，即语义策略已使用的 `libm::powf`（一期 1.2） |

私有 C 边界为 `src/adm_dsp/stream_dsp_ffi.h`，C++ 包装为 `stream_dsp.h`。调用方保留缓冲区、源图重建、增益包络
（`GainBank`，已在 Rust）与生命周期；每块每个源跨一次 FFI，不逐样本调用。离线双耳 OLA 与 spreader 适配器中剩下的只有
固定顺序的单独加法和单独乘法，不受乘加融合影响，保持在 C++。

## 数值影响

- Linux x64：GCC 在 ISO C++ 模式下默认不做乘加融合，`glibc` 与 `libm` 在本轮用到的 dB 和角度上给出相同结果。
  新增的文件流式重放在迁移前后逐位相同，锁定矩阵的 196 个 PCM id 不变。
- macOS arm64：上述三处乘加表达式此前可能被合成为 FMA；迁移后与 x86 一样分别舍入。迁移前没有文件流式用例，
  这一可能的旧差异没有测量记录，新用例从三平台相同的结果开始门禁。
- 折叠矩阵和实时增益的数学函数改为 `libm` 后，个别输入可能在最后一位与平台库不同；内核探针记录了它们的取值。

## 文件流式重放

一致性采集新增工具 `mr_adm_phase2_stream` 与 6 个 PCM 用例（`phase2_common.stream_cases()`）。每个用例打开双耳
`IRenderStream`，按固定（512 帧）或碎片（1/7/127/511/1024 帧循环）的请求分块拉取，并在固定的输出帧施加事件：

| 输出帧 | 事件 |
| --- | --- |
| 0 | 对象增益 −3 dB |
| 7001 | 增益 −14.5 dB |
| 12345 | 增益 +2.25 dB，extent ×0.5、diffuse ×1.5（触发拓扑重建与淡化） |
| 20000 | 头部朝向 yaw 37°、pitch 12°、roll −9° |
| 26000 | 增益 −6 dB，宽/高/深 ×1.75/×0.25/×2，divergence ×0.5 |
| 31000 | seek 到第 4800 帧 |
| 40000 | 静音 |
| 44000 | 清空覆盖 |

三组输入：`objects-extent-multi`（cloud）、`objects-cartesian`（cloud，含 depth）与 `objects-point`（none）。
`saf_spreader` 模式不支持文件流式渲染，因此不在用例内。每个用例在同一进程渲染两遍、并在新进程再渲染一次，三者逐位相同；
`IRenderStream` 约定输出与请求分块无关，采集时要求固定与碎片分块逐位相同，否则失败。6 个 PCM id 全部设为门禁，
PCM 门禁由 196 个增至 202 个。

用例版本 2 固定输入为 48 kHz、48,000 帧；`frames: 96000` 是重放上限。按上述 seek 时间线，实际应交付
`31000 + (48000 - 4800) = 74200` 帧。探针独立跟踪媒体位置，逐次校验 `process` 返回长度；采集器与比较器
再从固定输入及事件时间线推导输出形状，不将实际产物写回 manifest 后的自洽当作帧数正确。
审查回归覆盖忽略 seek、提前 EOS、多出帧数以及声道数／采样率错误；这些错误即使可重复且文件 hash 自洽也必须失败。

## 内核门禁

内核探针新增 `stream-dsp` 模块：

| 文件 | 内容 |
| --- | --- |
| `10-spread.f32` | 10 × 10 × 4 组 width / height / depth（含负值、归一化取值与角度取值）的锥角 |
| `20-fold.f32` | −180° 至 180° 每 7.5° 及 BS.2051 扬声器方位的 {L, R} 折叠增益 |
| `30-live-gain.f32` | −60 至 +12 dB 每 0.05 dB 及若干端点的实时增益 |
| `40-mix.f32` | 三个立体声源的加权求和，以及从一组和到另一组和的拓扑淡化 |

4 个文件全部设为门禁（模式 `stream-dsp.*`）。内核文件由 213 个增至 217 个，门禁由 208 个增至 212 个。

## 验证


**本机（Linux x64，config A）。** 与 2.4 相比 196 个原有 PCM id 逐位相同；6 个文件流式重放在迁移前后逐位相同，
同进程两遍、新进程与固定/碎片分块的比较全部相同；原有内核文件除 2.4 合并审查修正的四个 `meter-acceptance-*.10-gates.f64`
外不变。ctest 全部通过；Rust fmt / clippy、FFI 头校验（270 个函数）、clang-format 与 clang-tidy 无告警。
重放中的事件按预期生效：−3 dB 与 −14.5 dB 两段相对无事件渲染的 RMS 比分别为 0.708 与 0.190。

**三平台验收。** `b20a365` 的 Consistency 38844929927、CI 38844932172 与 Quality 38844933977 通过：

- A/B 两种配置下 202 个 PCM id 三平台相同，包括 6 个 `stream-binaural-*` 文件流式重放；216 项门禁无失败；
- 217 个内核文件全部三平台相同，包括 4 个 `stream-dsp` 文件；
- 诊断运行的 Scene 检查点全部相同，其余诊断检查点的相同 / 不同分布与 2.4 一致。

合并审查随后补上用例版本 2 的独立帧数校验与回归测试。上述三平台记录对应 `b20a365` 的版本 1；
版本 2 保持输入音频、事件与数值运算不变，由合并后的 Consistency 工作流重新验收。
