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

<!-- 本机与三平台结果在采集完成后填写。 -->
