# 最终 PCM 与三期结项（三期 3.6）

> 对应 [PCM 一致性审计](PCM_CONSISTENCY_AUDIT.md)三期 3.6，审计项 S06，以及“已收敛的实现与 PCM 写出”中的增益重写、整数量化、
> float 写出与声道排列。

## 秒到帧换算（S06）

用户以秒指定的时间有两类入口：
- 离线输出裁剪（`--start` / `--end`、`adm_render_options_set_render_*_sec`）与预览窗口（`adm_preview_render_window*`），都经
  `RenderService` 用 `llround(seconds * rate)` 换算，即取最近帧、半帧远离零；
- Monitor 定位与循环（`adm_monitor_seek`、`adm_monitor_set_loop`）经 `clamp_frame` 用 `static_cast<uint64_t>(seconds * rate)`，
  即向零截断。

两者在三个平台上都是确定的：乘积是一次正确舍入的 binary64 乘法，取整也是精确运算。问题在于规则不同：同一秒数在预览和监听中可能
相差一帧，例如 0.25001041666666666 s 在 48 kHz 下为 12000.5 帧，裁剪取第 12001 帧，Monitor 定位取第 12000 帧。

3.6 新增 `mradm_dsp::timeline::seconds_to_frame`（C 边界 `mradm_dsp_seconds_to_frame`，C++ 包装 `dsp::seconds_to_frame`）：
取 binary64 乘积 `seconds * rate` 的最近帧，半帧远离零（`libm::round`）；非有限、负的时间或零采样率返回失败，乘积达到 2^64 时饱和到
`u64::MAX`。`RenderService` 的裁剪与预览窗口改用它，结果与原先的 `llround` 相同；Monitor 改用同一规则后再钳位到素材长度
（负值与 NaN 为 0，+∞ 为末尾），定位由截断改为最近帧，最多移动一帧。窗口渲染的 preroll、Scene epoch 与帧计数仍是整数规则，
1.5 已锁定，不受影响。

## 增益重写、整数量化与 float 写出

| 环节 | 实现 | 3.6 |
| --- | --- | --- |
| 最终增益与自动增益 | 增益判定与 dB 换算在 Rust `post_gain::render_gain`（2.3）；`apply_gain_to_file` 以 4096 帧为块读写，逐样本乘法在 Rust `mradm_dsp_apply_gain` | 不变。C++ 中只剩 `|gain − 1| < 1e-6` 的跳过判断，是单次 f32 减法与比较，结果确定 |
| 整数 WAVE | Rust `mradm-wav` writer：f32 钳位到 [−1, 1]，转 f64 乘 32767 / 8388607 / 2147483647，向零截断；NaN 拒绝 | 不变。新增 `wav-quantize.*` 内核：边界值、各整数步长的 1/4、1/2、3/4 点及相邻 binary32 值、±∞、次正规数经生产 writer 量化 |
| float32 WAVE | Rust writer 直接写出小端位模式 | 不变 |
| float32 CAF | C++ `FloatCafWriter` 原先 `fwrite(float)`，`FloatCafReader` 原先 `fread(float)`，依赖主机字节序 | 改为显式按小端序列化与解析；三个平台均为小端，输出字节不变 |

16 位与 24 位整数转回 float32 是精确的，因此一致性采集仍比较读回的 float32 位模式；这等价于比较整数样本。

## 声道排列

WAVE 输出的声道排列与元数据由 Rust `finalize_wav_layout` 完成：7.1.4 由渲染器顺序（侧环绕在后环绕之前）置换为 WAVE 掩码顺序，
binaural、9.1.4、9.1.6、22.2 与 HOA3 写入 ADM AXML / CHNA，Triple Balance 9.1.6 使用 speaker re-render 配置（无掩码、无 ADM）。
EAR 的 `wav71` 只交换样本位置，不含运算。原有离线用例只覆盖 float32 WAVE；3.6 补上整数 WAVE 与 CAF 的最终文件。

## 新增用例与内核门禁

| 用例 | 输入 | 渲染器 / 输出 | 最终文件 |
| --- | --- | --- | --- |
| `final-ear-714-i24` | `objects-extent` | EAR 7.1.4 | 24 位 WAVE，7.1.4 置换 |
| `final-ear-51-i16-clip` | `objects-point` | EAR 5.1，最终增益 +6.5 dB | 16 位 WAVE，满幅钳位 |
| `final-ear-51-f32-gain` | `objects-point` | EAR 5.1，最终增益 +6.5 dB | float32 WAVE，保留超过满幅的样本 |
| `final-hoa3-i24-trim` | `objects-point` | HOA3，裁剪 0.25001041666666666—0.7000104166666667 s | 24 位 BW64 + ADM |
| `final-binaural-i16-trim-gain` | `objects-extent` | 双耳 cloud，裁剪 0.1—0.65 s，最终增益 −3.25 dB | 16 位 WAVE |
| `final-saf-222-i24` | `directspeakers` | VBAP 22.2 | 24 位 BW64 + ADM |
| `final-triple-916-i24` | `objects-room` | Triple Balance 9.1.6 | 24 位 WAVE，speaker re-render 配置 |
| `final-caf-ear-714-trim-gain` | `objects-extent` | EAR 7.1.4，自 0.25001041666666666 s 裁剪，最终增益 +2.5 dB | float32 CAF |
| `final-caf-binaural-postproc` | `objects-point` | 双耳，默认峰值限制 | float32 CAF |

两处裁剪起点在 48 kHz 下分别为 12000.5 帧（恰为半帧，取 12001）与 33600.50000000001 帧（取 33601）。离线用例新增 `container`
字段与 `--output-bit-depth` / `--final-gain-db` 参数，命令行与 `mr_adm_phase2_render` 两条路径都按用例写出最终文件；
读回时校验整数位深与 float 标志。9 个用例标注 `slice: 3.6`，记在二期冻结参数集之外，PCM 门禁由 233 个增至 242 个。

| 内核文件 | 内容 |
| --- | --- |
| `timeline.10-seconds-to-frame.f64` | 14 段十进制秒数（含恰为半帧与相差一个 ulp 的乘积）在 8 个采样率下的帧号 |
| `wav-quantize.10-pcm16.i32` / `20-pcm24.i32` / `30-pcm32.i32` | 306 个边界样本经生产 writer 量化后的整数 |

4 个文件全部设为门禁（模式 `timeline.*`、`wav-quantize.*`）。内核文件由 231 个增至 235 个，门禁由 226 个增至 230 个，
门禁总数由 253 项增至 264 项。

## 验证

**本机（Linux x64，config A）。** 与 3.5 修正后的采集相比，原有 233 个 PCM id 与 231 个内核文件全部不变（裁剪换算与原先的
`llround` 相同，原有用例都写 WAVE），9 个 `final-*` 用例与 4 个内核文件为新增。新用例符合预期：
- 两个裁剪用例分别为 21,600 帧（第 12001—33601 帧）与 35,999 帧（自第 12001 帧起），半帧边界取远离零的一侧；
- 16 位与 24 位用例的样本都落在对应整数网格上，`final-ear-51-i16-clip` 在满幅处钳位；
- 同样 +6.5 dB 的 float32 WAVE 与 +2.5 dB 的 CAF 保留超过满幅的样本（峰值 2.11 与 2.70）。

比较器自检 242 个 PCM id、264 项门禁无失败。ctest 80 项全部通过；Rust 单元测试（含半帧、饱和与非法输入）、fmt / clippy、
FFI 头校验（272 个函数）、一致性工具测试、clang-format 与 clang-tidy 无告警。

### 审查与诊断采集修正

公开 C++ `RenderService` / `PreviewSession` 的裁剪入口必须保留时间换算失败，不能用第 0 帧或素材末尾代替。
现在非有限的起止时间及负起点返回 `invalid_argument`，不创建输出；未指定终点时的 NaN / 负起点也进入校验。
有限的极大终点仍按饱和换算后钳位到素材长度。`render_trim_fixture_test` 对两个入口覆盖 9 组非法窗口，并覆盖
有限终点乘以采样率溢出的情况；新增测试在修正前失败，修正后 12 项 fixture 检查全部通过。

`d07f4cd` 的 [Consistency 38932508634](https://github.com/SakuzyPeng/MacinRender-ADM-Core/actions/runs/38932508634)
在六个平台 / 配置组合中均因 `final-caf-ear-714-trim-gain` 缺少 writer 检查点而失败：普通采集完成，但 CAF writer
尚未像 WAVE writer 一样记录诊断 PCM。CAF 现在在诊断构建中记录每次成功写出的完整帧，按 writer pass 与帧位置命名；
WAVE / CAF 共用 pass ID，避免同一进程混用容器时覆盖检查点。普通构建不包含这些诊断操作。

macOS 的 Release 定向验证复用已有依赖、重编译音频读写模块：两个 `final-caf-*` 用例分别产生 45 / 59 个 writer
检查点，最后一轮检查点拼接后与最终文件 PCM 逐位相同，诊断开关前后 PCM 不变。另验证混用 WAVE / CAF 的 pass ID、
分块帧位置、零长度写入及带符号零 / 次正规数位模式；此项本机验证不替代完整三平台矩阵。

**三平台验收。** 合并后的 `4ea3665`（含上述裁剪校验与 CAF 诊断检查点修正）的
[Consistency 38937703621](https://github.com/SakuzyPeng/MacinRender-ADM-Core/actions/runs/38937703621)、
CI 38937703580 与 Quality 38937703595 通过：

- A/B 两种配置下 242 个 PCM id 三平台相同，包括 9 个 `final-*` 用例（16 / 24 位整数 WAVE 与两个 float32 CAF）；264 项门禁无失败；
- 235 个内核文件全部三平台相同，包括 `timeline.10-seconds-to-frame.f64` 与 3 个 `wav-quantize` 文件；
- 诊断检查点 80,631 个（每对平台 26,877 个）全部三平台相同，两个 CAF 用例的 writer 检查点也在其中。

## 三期结项

三期按审计的分期处理文件流式渲染与离线渲染，六个切片的门禁规模如下：

| 切片 | 内容 | PCM 门禁 | 内核文件（门禁） | 记录 |
| --- | --- | ---: | ---: | --- |
| 3.1 | 文件流式双耳与监听适配 | 202 | 217（212） | [RUST_FILE_STREAM_BINAURAL.md](RUST_FILE_STREAM_BINAURAL.md) |
| 3.2 | EAR | 208 | 220（215） | [RUST_EAR_PORTABLE_MATH.md](RUST_EAR_PORTABLE_MATH.md) |
| 3.3 | HOA | 216 | 224（219） | [RUST_HOA_PORTABLE_MATH.md](RUST_HOA_PORTABLE_MATH.md) |
| 3.4 | Triple Balance | 228 | 228（223） | [RUST_TRIPLE_BALANCE_PORTABLE_MATH.md](RUST_TRIPLE_BALANCE_PORTABLE_MATH.md) |
| 3.5 | 参数入口与同起点块 | 233 | 231（226） | [RUST_PARAMETER_ENTRY.md](RUST_PARAMETER_ENTRY.md) |
| 3.6 | 最终 PCM | 242 | 235（230） | 本文 |

结项时各审计条目的状态：
- **数值实现依赖 N01—N18 全部处理。** 各 Rust crate 的生产代码不再调用平台初等函数（源码扫描测试约束），C++ 中剩下的浮点运算
  经逐切片核对只有单独的加法、乘法、比较与精确取整。
- **S01、S02、S06、S08 已处理。** S06 由本切片统一秒到帧换算；Scene 的 epoch、preroll 与帧计数在 1.5 锁定。
- **S03 保留为分块条件。** Scene 的分块条件在 1.5 记录，11 个用例要求分块无关；双耳卷积与分数比率重采样依赖分块，按分块配置
  分别门禁。文件流式 `Mixer` 的对象平滑在每次调用的首尾增益之间插值，同样依赖请求分块；三期的文件流式重放都不开平滑，
  这一路径没有门禁，需要时应按分块配置分别设例。
- **S04、S05 保留为运行时条件。** Monitor 与实时监听按墙钟取样控制快照，欠载时补零；同一 UI 操作落在哪个样本、是否欠载取决于调度，
  不能要求逐位相同。可重放的部分（事件固定在样本位置的文件流式与 Scene 重放、HpTF 排队规则、欠载不推进 HpTF 与峰值保护）
  已有门禁。
- **S07 保留为构建条件。** 门禁结果依赖固定的编译器与 Rust 工具链、`Cargo.lock`、vendored 依赖、有效编译选项（config A 为发布构建
  选项）与默认的宿主浮点环境；公开 C ABI 不为调用线程建立舍入模式或 FTZ/DAZ 环境。三期核对的 C++ 路径中，乘法与加法都是单独的
  语句（例如双耳离线 LFE 旁路先算 `sample = in * gain` 再累加）。macOS arm64 的 clang 默认只在单个表达式内融合，x86-64 基线没有 FMA
  指令，因此锁定的三个平台不会融合；GCC 在 C++ 中默认 `-ffp-contract=fast`，可能跨语句融合，Linux arm64 等不在矩阵内的组合不受
  这一结论保护。

FLAC、Opus、APAC、IAMF 等编码与 Apple AUSpatialMixer 后端仍不在范围内；FLAC 与 WAVE 的量化方式不统一（FLAC 用 `lroundf`），
按审计约定不在本轮处理。门禁只约束锁定的矩阵、构建条件与三个平台，不构成对任意输入或工具链的全局承诺。
