# C++ 数值面审计与纯调度路线

> 2026-10-09：盘点离线渲染链路（从输入 WAV 到写出 PCM）和 Scene 拉流链路（从状态输入到设备缓冲区），仍由 C++ 执行、可能影响跨平台一致性的计算，
> 并据此排出 C++ 退化为纯调度的迁移顺序。本文只做盘点和规划，不改变任何实现。

## 口径

- **一致性**指同一输入、同一选项在 macOS arm64（Apple clang / libc++）、Linux x64（GCC / libstdc++）和
  Windows x64（MSVC）上得到逐位相同的解码后 PCM，与二期门禁口径一致（[`RUST_PHASE2_CLOSEOUT.md`](RUST_PHASE2_CLOSEOUT.md)）。
- **纯调度**指 C++ 只负责：请求构造与参数校验、模块与后端选择、线程/设备/进度/取消、文件打开与临时文件替换、
  日志，以及公开 C ABI。凡是改变样本值、增益、位置、滤波参数、块边界或块顺序的计算和语义判定，都应在 Rust 中完成。
- 只统计真实的 C++ 运算。已经经 `mradm-ffi` 委托给 Rust 的部分，C++ 只做搬运，单列在后文，不计入清单。

## 现有门禁覆盖到哪里

- 34 个离线用例都输出 `--output-bit-depth f32` WAV，由 `mr_adm_pcm_bits` 读取最终文件的位模式。
  - 覆盖范围：渲染 →（裁剪）→ 后置增益 → Rust f32 写出与 `finalize_wav_layout`。
  - 后置增益只有 3 个用例真正触发，都是 EAR 5.1、`objects-point` 素材：
    - `ear-5_1-point-postproc`（默认 −1 dBTP 限制）；
    - `ear-loudness-minus23`；
    - `ear-peak-minus6`。
  - 其余用例都带 `--no-peak-limit`（`scripts/consistency/phase2_common.py:75`），后置增益在这些用例里不起作用。
- 84 个 Scene id 采集进程内 Scene pull 的 float 输出，不经过文件写出。
- **门禁外**：
  - i16 / i24 WAV、FLAC、Opus MKA、APAC、IAMF、CAF；
  - 默认开启峰值限制的其它渲染器和素材；
  - 语义策略（`--semantic-policy`）。

## 编译条件

- 生产 C++ 目标没有任何浮点相关编译选项：没有 `-ffp-contract`、没有 `/fp:*`，也没有 `-march`。
  - 只有 4 个冻结参考测试目标挂了 `mr_adm_core_scene_reference_fp`（`cmake/MRStrictFp.cmake:99`）。
  - `scene-separate-v1` 不是编译选项，而是传给 Rust 的算术标志（`src/adm_dsp/scene_math.h:16`）。
- 因此剩下的 C++ 运算按各工具链的默认行为执行：

  | 工具链 | 默认 contraction | 结果 |
  |---|---|---|
  | Apple clang arm64 | `-ffp-contract=on` | 同一表达式里的 `a*b+c` 会融合成 FMA |
  | GCC x64 | `fast` | 基线 x86-64 没有 FMA 指令，实际不融合 |
  | GCC arm64（CI 的 Linux ARM64 Release） | `fast` | 内联后跨语句也可能融合 |
  | MSVC x64 | `/fp:precise`，无 AVX2 | 不融合 |

- Rust 从不隐式融合乘加，也不设置 target-cpu。代码中没有任何地方设置 FTZ/DAZ 或修改舍入模式。
  唯一的例外是下文 O4 提到的 ebur128。
- 平台 libm（glibc、Apple libm、UCRT）的 `pow`/`log10`/`powf`/`sinf` 不保证正确舍入，
  三个平台之间可能相差 1 ULP。`sqrt`、单次乘法、单次加法都是 IEEE 精确舍入，不受平台影响。

## 清单

风险分级：**高**表示作用于每个样本或每个增益，且没有门禁覆盖；**中**表示作用于每块或每个对象的参数，
或者只在特定条件下触发；**低**表示一次性的元数据计算，或今天已经确定性一致、只是仍由 C++ 计算。

### 输入与场景

| ID | 位置 | 计算 | 一致性风险 | 级别 |
|---|---|---|---|---|
| S1 | `ear_renderer.cpp:445`、`vbap_renderer.cpp:366`、`hoa_renderer.cpp:201`、`binaural_renderer.cpp:653/739` | 用非稳定的 `std::ranges::sort` 按 `start_sample` 给块排序 | 三个平台的 STL 对相同起点的块排出不同顺序（元素数超过各 STL 的插入排序阈值、约 16–32 个之后进入分区排序），导致生效的增益块和插值前驱不同。同一 CHNA 声道上有多个块，或对象时长裁剪留下空块时都会出现 | 高 |
| S2 | `semantic_policy.cpp:793/863/954/964` | 策略 `gain_db` 用 `std::pow(10.0F, x/20.0F)` 转成线性 | 平台 `powf` 可能差 1 ULP，并作用于整个对象的每个样本 | 高（仅在使用策略时） |
| S3 | `semantic_policy.cpp:58-83`、`direct_speakers_matrix.cpp:82` | nlohmann 读入策略 JSON 和矩阵权重，经 `strtod` 再 `get<float>()` | 依赖 C 库 `strtod` 是否正确舍入。当前三个平台的 C 库都正确舍入，但下溢和次正规数边界没有测试，与 HpTF 迁移时发现的问题同类 | 中 |
| S4 | `semantic_policy.cpp:785-1036` | 策略的缩放、钳位、偏移、`max_samples` 计算 | 单次 IEEE 运算，目前确定，但仍是 C++ 在改写参数 | 低 |
| S5 | `hoa_renderer.cpp:70` | 用 `std::stof` 解析扬声器标签中的方位角 | 跟随进程 locale 的小数点，并接受十六进制和 `inf`；与 VBAP/EAR 共用的 `from_chars` 解析规则不一致 | 中 |
| S6 | `direct_speakers_matrix.cpp:142` | `sqrt(weight / Σweight)`（double） | `sqrt` 精确舍入、求和顺序固定，结果确定 | 低 |
| S7 | `channel_bed_importer.cpp:225-289`、`speaker_layouts.cpp` | 用 C++ 静态表合成声道床的位置 | 常量，结果确定，但这是 C++ 持有的数值数据，必须与 Rust EAR 布局表保持同步 | 低 |
| S8 | `scene_importer.cpp:34`、`render_common.cpp:81-96` | `toupper`/`tolower` 规范化 UID 和标签 | 跟随进程 locale；项目自身不调用 `setlocale`，但宿主进程可能调用 | 低 |

### 渲染

| ID | 位置 | 计算 | 一致性风险 | 级别 |
|---|---|---|---|---|
| R1 | `binaural_renderer.cpp:1888-1901/2003-2007/2041-2044`、`binaural_spreader.cpp:125/159-160` | 双耳离线链路的逐样本 float 累加：OLA 各源归约、延迟环混入、spreader 分组归约、尾部排空、spreader 输入增益 | 归约顺序固定，目前确定，是剩下最大的一块逐样本 C++ 运算 | 中 |
| R2 | `binaural_renderer.cpp:1100-1106` | LFE 旁路：先 `sample = in*gain`，再 `+=` | 乘和加分属两条语句；GCC 在 arm64 上内联后可能把它们融合成 FMA | 中 |
| R3 | `binaural_renderer.cpp:495-500` `extent_spread_deg` | `(max(0,w)*60)+depth_r` | 同一表达式在 Apple clang arm64 上会融合成 FMA，x64 不会。它决定 spreader 的锥角，以及走 spreader 还是 OLA（仅 `saf_spreader` 模式；depth 为 0 时融合与否结果相同） | 中 |
| R4 | `vbap_renderer.cpp:158/260/264/284-308/349` | 增益表乘对象增益、发散累加 | 目前确定；`gains += table*gain` 内联后在 GCC arm64 上可能融合 | 中 |
| R5 | `ear_renderer.cpp:179/221-228/330/391` | double 增益乘积、发散累加、HOA 解码矩阵乘声道增益 | 顺序固定，结果确定；转成 f32 发生在 Rust `PcmMixPlan` 中 | 低 |
| R6 | `binaural_renderer.cpp:640-644/764-789/1012/1050/1967` | `sqrt(1-diffuse)`、源增益乘块增益 | `sqrt` 和单次乘法都精确舍入，结果确定 | 低 |
| R7 | `triple_balance_renderer.cpp:209`、`bed.cpp:93/103` | `output_gain*bed_user_gain`、`sqrt(0.5F)` | 结果确定 | 低 |

### 后置处理与写出

| ID | 位置 | 计算 | 一致性风险 | 级别 |
|---|---|---|---|---|
| O1 | `speaker_pcm.cpp:248`、`ear_renderer.cpp:981`、`hoa_renderer.cpp:730`、`binaural_renderer.cpp:2081` | 用 `20*log10(peak)` 换算真峰值 | 平台 `log10`；结果流入 O2 的增益与阈值判断 | 高 |
| O2 | `render_service.cpp:912-977` | 合成 `gain_db`（响度目标、峰值补偿、峰值钳位、最终增益，带 0.1/0.01 阈值），再 `float(pow(10, gain_db/20))` | 平台 `pow`；1 ULP 差异可能改变转换后的 float，阈值附近还会改变是否施加增益。默认 `peak_limit=true`，因此任何真峰值高于 −1 dBTP 的渲染都会走这条路径，是最常用的路径。门禁只用一份素材覆盖它 | 高 |
| O3 | `audio_handles.cpp:228-230` `apply_gain_to_file` | 逐样本 `buf[i] *= gain` | 单次乘法，结果确定，但每个样本都要经过 C++ | 中 |
| O4 | Rust `ebur128 =0.1.10`（`filter.rs:69-71`、`utils.rs:35`、`history.rs:105`、`filter.rs:380` 起） | K 加权系数用 `tan`/`powf`，响度用 `log10`；**只在 x86 上**通过 MXCSR 打开 FTZ | 代码在 Rust 里，但不是可移植实现：平台 libm 加上只在 x86 生效的 FTZ，会让 `measured_lufs` 和真峰值在平台之间出现差异，进而传入 O2。这是 O2 的上游依赖 | 高 |
| O5 | `flac_io.cpp:306-310` | `lroundf(clamp(x,-1,1) * 8388607.0F)` | 有限值输入时结果确定。**NaN** 输入时：Linux 的 `lroundf` 返回 LONG_MIN，截断为 int32 后是 0；macOS 返回 0；Windows 的 `long` 是 32 位，可能返回 −2^31，超出 24 位范围。此外，FLAC 是四舍五入，而 Rust 写 i24 WAV 是向零截断（`mradm-wav` `writer.rs:146`），同一次渲染的 `.flac` 和 `.wav --output-bit-depth i24` 最多相差 1 LSB | 高（NaN 只在 Windows 出错；格式之间不一致） |
| O6 | `cmake/MRDependencies.cmake:284-293` | libFLAC 使用 `-fassociative-math -freciprocal-math`，在运行时按 CPU 选择 SSE/AVX2/NEON 代码 | 无损编码，解码后的 PCM 不受影响；但 `.flac` 文件字节可能因平台而不同 | 低（只影响文件字节） |
| O7 | `render_service.cpp:485/492` | 裁剪秒数换算帧数：`llround(sec*sr)` | 运算本身确定。CLI11 用 `strtold` 解析秒数：x64 Linux 的 long double 是 80 位，aarch64 Linux 是 128 位，MSVC 和 macOS arm64 等于 double，因此十进制输入恰好落在中点时可能差 1 ULP。C ABI 和 GUI 直接传 double，不受影响 | 低 |
| O8 | `loudness_normalizer.cpp:151`、`peak_limiter.cpp:117-131` | 旧的两遍归一化，公式与 O2 重复，峰值限制用的是比值而不是 dB | 不在 CLI 生产路径上，但能通过公开的 `loudness.h`/`peak.h` 调用，可能与 CLI 结果分叉 | 中 |
| O9 | `opus_mka_io.cpp:396-489`；IAMF bridge；APAC `ExtAudioFileWrite` | 有损编码器 | libopus 浮点构建没有开 `FLOAT_APPROX`，调用平台 `log`/`exp`，在 Apple clang arm64 上还会被融合成 FMA；IAMF 自带的 libopus 编译选项不受本仓控制；APAC 是闭源实现，只在 macOS 上可用。这些格式按设计就不承诺逐位一致 | 不纳入承诺 |

### Scene 拉流

Scene 拉流从 `adm_scene_stream_*` 接收对象状态，经 `scene_stream_engine.cpp` 转换成渲染状态，交给
Live VBAP、Live 双耳或 Live Triple Balance 渲染，最后由 `scene_output_session.cpp` 送往设备。
重采样、声像、卷积、Triple Balance、HpTF 和立体声峰值保护都已经在 Rust 里。
Live 双耳和 Live Triple Balance 的 C++ 部分只做搬运。

| ID | 位置 | 计算 | 一致性风险 | 级别 |
|---|---|---|---|---|
| SC1 | `scene_stream_engine.cpp:611-640/683/748`，调用 `semantic_policy.cpp:1473-1536` | 每代按语义策略解析对象身份，每次状态更新时按策略改写对象和 DirectSpeakers 的参数 | 与离线链路共用 S2–S4，其中包括平台 `powf`。84 个 Scene 门禁 id 都不带策略 | 高（仅在使用策略时） |
| SC2 | `scene_stream_engine.cpp:684/750` | `block.gain * object_gain` | 单次乘法，结果确定 | 低 |
| SC3 | `scene_stream_engine.cpp:697-700/744-747` | Rust 算出方向向量后，C++ 乘以距离得到笛卡尔坐标 | 单次乘法，结果确定 | 低 |
| SC4 | `live_vbap_renderer.cpp:431/442/523-524` | 距离下限 0.4、发散子源增益按声道累加、LFE 分功率增益 | 顺序固定，结果确定 | 低 |
| SC5 | `scene_output_session.cpp:229-234` | 多声道输出在没有设备音量时逐样本 `*= volume`；立体声带峰值保护时，音量在 Rust 里施加 | 单次乘法，结果确定 | 低 |
| SC6 | `scene_stream_engine.cpp:1400-1425` | 输出帧边界的有理数累加 | 纯整数运算，结果确定 | 无 |

### 实时监听（不写文件，只作记录）

- `monitor_session.cpp:49-53`：立体声折叠系数用 float 版 `sin`/`cos`，即平台 `sinf`/`cosf`。
- `render_common.cpp:310`、`hoa_renderer.cpp:309`：实时覆盖增益用平台 `powf`。
- `binaural_renderer.cpp:1447-1448/1481`：`BinauralStream` 归约和拓扑交叉淡化的乘加表达式可能被融合成 FMA。
- `monitor_session.cpp:43`：`clamp_frame` 截断取整，而离线链路是 `llround`，两条路径不一致。
- miniaudio 和系统混音器做的格式/采样率转换都不在本仓控制范围内。

### 已在 Rust（C++ 只搬运，不计入清单）

- AXML 解析，时间到采样点的换算（`mradm-adm`）。
- WAV 读写、整数与 float 的换算、`finalize_wav_layout`（`mradm-wav`）。
- Scene 空间数学：channelLock、发散、extent cloud、位置偏移（`scene_math`）。
- 块时间线插值与平滑（`PcmMixPlan`/`PcmMixer`）。
- EAR 增益与 FIR 设计、EAR 后处理。
- VBAP/MDAP 声像、HOA 编解码。
- HRTF 插值、OLA 卷积、spreader、头部旋转。
- Triple Balance 的数值状态、HpTF、Meter、重采样、输出保护和交叉淡化。

C++ 侧的包装头（`src/adm_dsp/*.h`）不含任何运算。

## 纯调度路线（按优先级）

涉及一致性问题的 C++ 代码，迁入 Rust 时顺带解决，不先在 C++ 里修一遍。

迁到 Rust 后，下列隐患自动消失：
- Rust 不隐式融合乘加，R2–R4 消失；
- `sort_by_key` 是稳定排序，S1 消失；
- `as` 把 NaN 转为整数时固定得到 0，O5 的 Windows 越界消失；
- `parse::<f64>` 与 locale 无关，并且正确舍入，S3、S5 消失。

libm 是例外。Rust 的 `f32::powf`、`f64::log10`、`tan` 等仍然调用平台数学库，ebur128 就是例子。
因此迁移时要显式改用可移植实现（`libm` crate 或 `mradm-math`）。

每个切片分两次提交：
1. **等价迁移**
   - 冻结旧实现，Rust 先用 std 数学函数。它与 C++ 在同一平台上调用同一个库函数，所以差分测试可以要求每个平台上都与冻结的 C++ 逐位相同。
   - 同时做灵敏度检查。
2. **一致性修正**
   - 换成可移植数学函数，或去掉平台相关的行为。
   - 由三平台门禁确认结果一致；相对旧输出的变化，在 `RUST_*_MIGRATION.md` 中逐项记录。

两次提交属于同一个切片，一起合入。这样可以分开验证"搬运是否出错"和"有意改变了什么"。
合入前要把三平台矩阵和门禁更新到位。

### 一期：Scene 链路

完成标准：从 `adm_scene_stream_*` 输入到设备缓冲区，C++ 只剩队列、线程、时钟和设备调度。
现有 84 个 Scene id 保持逐位相同，新增的带策略的 Scene id 纳入门禁。

1. **语义策略迁入 Rust（SC1、S2–S4）**
   - 迁移内容：
     - 策略 JSON 改用 serde_json 解析，依赖已经引入；
     - 身份解析、规则合并和参数改写也迁入 Rust。
   - Scene 和离线共用这一套 Rust 实现：
     - Scene 每代解析一次，每次状态更新时调用改写；
     - 离线在渲染前改写整个场景。
   - dB 转线性改用可移植的 `powf`，不再依赖平台 libm。
   - 门禁新增：
     - Scene 用例：带策略的 `gain_db`、`scale`、位置偏移、channelLock、divergence；
     - 离线用例：一个带策略的。
   - 这一步对应之前列出的“EAR channelLock/divergence 与语义策略”迁移。
2. **Scene 状态转换迁入 Rust（SC2、SC3）**：对象状态到渲染状态的换算（增益相乘、极坐标转笛卡尔）
   并入 Rust 的 scene_math 入口，C++ 只搬运字段。
3. **Live VBAP 胶水层迁入 Rust（SC4）**：距离下限、发散子源累加、LFE 增益并入 Live VBAP 的 Rust 状态。
4. **Scene 输出音量迁入 Rust（SC5）**：多声道音量并入 Rust 输出 DSP，与立体声峰值保护走同一入口。

### 二期：离线默认路径上会改变 PCM 的项

5. **非稳定排序（S1，先在 C++ 修复，见下文例外）**：把 5 处排序改成稳定排序，或在起点相同时按块序号排序；补一个同起点块超过 32 个的回归用例。
6. **后置增益链（O1–O4、O8）**
   - 增益合成、dB 与线性互换、逐样本乘法都迁入 Rust，并改用可移植数学函数。
   - ebur128 中的 libm 调用和只在 x86 生效的 FTZ，在 vendored 补丁里改成可移植实现。
   - 旧的两遍归一化接口改为调用同一个 Rust 函数。
   - 门禁新增默认开启峰值限制的用例，覆盖 VBAP、HOA、双耳和 Triple Balance。
7. **整数量化统一（O5）**
   - FLAC 改用 `mradm-wav` 的量化函数，NaN 一律当作 0。
   - 门禁新增 i24 WAV 和 FLAC 解码后的 PCM。
   - 待用户决定统一用四舍五入还是向零截断。两种做法都会改变其中一种格式的现有输出。

### 三期：离线结构性迁移（目前结果确定，但仍由 C++ 计算）

8. **双耳逐样本归约（R1–R3、R6）**：OLA/spreader 归约、LFE 旁路、`extent_spread_deg` 迁入 Rust 混音内核，C++ 只保留线程池。
9. **块时间线与增益组装（R4、R5、S5–S7、O7）**：按声道分组块、组装增益、解析标签、处理声道床位置表和裁剪换算，
   都交给 Rust。完成后渲染后端的 C++ 只剩调度。

### 四期：实时监听剩余项

10. 折叠系数、实时覆盖增益、`BinauralStream` 归约与交叉淡化、`clamp_frame` 取整。

### 例外与不纳入范围

S1 要到三期才会随块时间线迁走，而在 C++ 里修复只需要几行，所以作为唯一的例外，先在 C++ 里单独修复。

不再给 C++ 目标加 `-ffp-contract=off` 过渡：融合乘加的风险点（R2–R4）会在二期、三期直接迁走。

以下项目不纳入路线：
- 有损编码器（O9）和 libFLAC 的文件字节（O6），不承诺逐位一致；
- 线程、设备、文件替换、进度和公开 C ABI，本来就是调度层，继续留在 C++。
