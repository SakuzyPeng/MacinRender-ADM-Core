# PCM 一致性审计与 Rust 迁移路线

统计日期：2026-10-09。源码与门禁基准：`456a3baaa338c8b7cc3da74951f28a8d5dfe5cc8`。

从 ADM / 参数输入到最终 PCM，项目自有渲染链路仍有 **18 组数值实现依赖、8 组状态或环境条件**需要纳入一致性管理。
这些是按生产入口和影响机制归并的 26 个审计条目，不代表发现了 26 个已复现的跨平台 PCM 缺陷。
平台数学函数尚未统一的路径很多；已有用例逐位相同，只约束这些用例及其构建条件。

本文件统一维护剩余数值运算清单与本轮分期路线，吸收 `d42e294` 中的 C++ 审计和性能检查。
原分支的审计内容保留在 Git 提交历史中；各 ADR、迁移验收记录和原始证据继续作为独立历史记录引用。

## 比较范围

这里的“一致”指样本位模式、声道顺序和有效帧数一致，覆盖两个终点：

- 离线渲染：自有后端渲染、重采样、裁剪、响度与峰值反馈、最终增益，以及 WAV float32 / 整数 PCM 或 CAF float32 的样本写出。
- 文件流式渲染和 Scene 流式渲染：自有后端生成并交给输出设备的应用侧 PCM，包括实时监听时适用路径上的 HpTF、音量、淡化和峰值保护。

FLAC、Opus、APAC、IAMF 等后续编码不在范围内。WAV / CAF 头部、时间戳、临时文件名和元数据字节也不作为 PCM 比较对象。
设备驱动和操作系统在接收应用侧 PCM 之后的重采样、混音与空间化，位于实时终点之后。
按维护者确定的范围，Apple AUSpatialMixer 后端及其专用 C++ 数值适配不纳入本次一致性统计、迁移清单或验收要求。
该后端依赖系统实现，版本也无法由项目固定。macOS 上的项目自有后端仍在比较范围内。
Windows SpatialAudioClient 等设备端空间化发生在应用提交床或对象 PCM 之后；本审计终点不延伸到系统生成的耳机波形。

平台比较需要固定输入字节、有效参数、资源、输入和输出采样率、通道映射、控制事件样本位置、初始状态及分块。
跨平台、跨分块、跨 API 路径、跨编译器版本是不同的比较维度，应分别记录。

目标边界是：自有数值求值、系数生成和样本运算集中在 Rust；C++ 保留公开 ABI、对象与资源管理、请求和规则匹配、
队列、线程、设备、文件生命周期、进度及取消。常量数据和纯整数计数不自动构成一致性缺陷；
会决定帧边界、顺序或语义选择的规则仍须明确并测试。

## 当前门禁与证据

以下数字取自基准提交的 `phase2_common.py` 与 `phase2-gates.json`：

| 项目 | 基准提交中的数量 |
| --- | ---: |
| 离线 PCM 配置 | 40 |
| Scene 配置 | 74 |
| Scene epoch 输出 | 148 |
| 精确 PCM 门禁 id | 188 |
| 内核测量文件 | 160 |
| 9 个内核门禁模式匹配的文件 | 150 |
| 内核观察文件 | 10 |

10 个观察文件为 EAR 布局 3 个、Scene 输入和输出 2 个、HpTF 输入和系数 4 个、平台 libm f64 twiddle 1 个。
基准清单没有独立的 Triple Balance D 模式 PCM 用例；普通 Triple Balance 离线用例和 Scene 流式用例不能替代它。

门禁的实际采集边界与缺口为：

- 40 个离线用例输出 float32 WAV，再提取最终样本位模式；包含适用的 trim、后置增益和布局整理。
  其中只有 `ear-5_1-point-postproc`、`ear-loudness-minus23`、`ear-peak-minus6` 配置了自动后置增益，
  均使用 EAR 5.1 和 `objects-point`；其他后端、素材及反馈阈值还需补覆盖。
- Scene 主 epoch 在样本 6144 实际执行 `global.gain.scale=0.5` 的语义策略更新，短尾 epoch 重新清空策略。
  当前并非“Scene 不带策略”；缺少的是 `gain_db`、身份匹配、位置、channelLock、divergence 等更完整的策略矩阵。
- 148 个 Scene id 比较 pull 或虚拟设备输出的 float PCM；其固定分块与碎片分块分别作跨平台比较。
  本清单没有用它们声明所有分块互相逐位相同，也没有声明所有文件流式渲染路径已覆盖。
- i16/i24 WAV、CAF 以及独立 Monitor 的完整路径尚未作为这份精确 PCM 矩阵的替代格式全面覆盖；后续编码仍排除。

[覆盖扩展验收](RUST_COVERAGE_EXTENSION.md)记录的是此前 **118/118 PCM 相同、132/133 内核文件相同**，
唯一不同项为 f64 twiddle 观察列，最大 1 ULP。188 是基准提交的门禁清单数量，不借用旧报告宣称已完成本轮 188 项三平台实测。
本清单中的“潜在”表示源码中存在依赖或差异机制，未据此断言某段正常节目已出现 PCM 分歧。

[SOFA 扩展记录](RUST_SOFA_CONSISTENCY.md)已增加 30 个 PCM id、27 个内核文件，并记录 macOS arm64 Release A
的重复性、完整性及诊断无扰动验证；该记录尚未给出新增范围的三平台 A/B 验收结果。
`bc506fb` 随后增加 VBAP Scene 22.2 的 48/96 kHz 和两种分块，共 8 个 epoch 输出，清单由 180 扩到 188。
本记录核对清单与源码，不替代这些新增范围各自的三平台验收报告。

## 输入与空间控制的数值依赖

| 编号 | 生产方法与位置 | 剩余机制 | 对 PCM 的影响 |
| --- | --- | --- | --- |
| N01 | CLI11 `detail::lexical_cast<T>`；nlohmann JSON `lexer::strtof`；HOA [`parse_speaker_label`](../../src/adm_render_hoa/hoa_renderer.cpp#L43)；[`normalize_uid`](../../src/adm_io/scene_importer.cpp#L34) 与 [`normalise_speaker_label_key/canonicalise_speaker_label`](../../src/adm_render_common/render_common.cpp#L76) | 本地使用的 CLI11 2.5.0 先 `strtold` 再转目标类型；Linux x64 与 macOS arm64 / Windows x64 的 `long double` 精度不同，舍入边界可能发生不同的二次舍入。JSON 仍通过 `strtod`，标签仍通过 `std::stof`；UID/标签的 `toupper` 跟随 C locale。 | 同一文本可能得到不同参数、接受结果或标签键，继而影响增益和路由。JSON 已适配 locale 小数点，不能仅凭存在 locale 调用认定它有缺陷。统一 ASCII 标签规则时需保留协议语义。HpTF 和 ADM 的 Rust 十进制解析不属于这一项。 |
| N02 | ADM [`gain`、`Snapshot::direct_block`](../../rust/crates/mradm-adm/src/project.rs#L283) | `10.0f64.powf(db/20)`；Cartesian DirectSpeakers 的 `atan2`。 | ADM 导入时的线性增益或极坐标可能不同；下游所有后端都能继承差异。 |
| N03 | [`apply_object_override`、`apply_hoa_gain`、`apply_ds_override`](../../src/adm_core/semantic_policy.cpp#L786)；[`resolve_live_channel_gain`](../../src/adm_render_common/render_common.cpp#L288)；HOA `set_overrides` | C++ `std::pow` 的 dB 到线性转换仍有多处独立实现，且 float / double 路径并不相同。 | 语义策略、对象和声道的实时增益直接乘到 PCM。相同 dB 数字经不同入口，也应单独比较其最终 float 增益。 |
| N04 | Scene [`cartesian_to_polar`、`direction`、`polar`、`cloud`、`divergence`、`axis`、`Rotation::apply`、`pose`](../../rust/crates/mradm-dsp/src/scene_math.rs#L70)；[Live 双耳 divergence](../../rust/crates/mradm-dsp/src/live_binaural.rs#L644) | 标准库 `sin/cos/tan/asin/atan2/hypot`。`scene-separate-v1` 统一了乘加舍入，没有替换这些函数。 | 坐标、头追、extent 云和 divergence 方向的最低位变化，可能进一步改变 channel-lock、HRTF 网格或几何阈值分支。 |
| N05 | [`geometry::direction/directions`](../../rust/crates/mradm-dsp/src/geometry.rs#L33)、[`vbap::Panner::new/gains`](../../rust/crates/mradm-dsp/src/vbap.rs#L14)、HRTF `Tree::new/grid_into` | 共用方向转换及 VBAP spread 采样仍使用平台 `sin_cos`。spreader 专用的 `portable_direction` 没有替换这一公共入口。 | 扬声器向量、三角形和 VBAP 增益，以及 HRTF 插值网格权重可能改变。拓扑边界需要精确比较。 |
| N06 | EAR [`geom::cart/azimuth/elevation/vertex_order`](../../rust/crates/mradm-ear/src/geom.rs#L3)、[`Panner::gains`](../../rust/crates/mradm-ear/src/panner.rs#L321)、[`Extent::new/gains`、`Weight::new/cosine/weight`](../../rust/crates/mradm-ear/src/extent.rs#L10) | 平台三角函数、反三角函数、`hypot`、立体声 panner 的 `powf`。nalgebra 的 `libm-force` 不会改写源码中的 `f64::sin` 等固有方法。 | EAR 的点源、extent 权重、DirectSpeakers 几何匹配和边界选择可能不同；部分增益以 f64 继续传入混音。 |
| N07 | HOA 编码 [`coefficients::polar/extent/compile`](../../rust/crates/mradm-dsp/src/hoa/coefficients.rs#L49)；EAR HOA [`harmonic`、解码器 `new/calculate`](../../rust/crates/mradm-ear/src/hoa.rs#L46) | 编码方向和 extent 的 `sin/cos/tan`；解码采样方向与球谐函数的 `sin/cos/atan2/hypot`。 | HOA 系数、解码矩阵和最终通道 PCM 可能不同。其代数/SVD 部分已使用锁定的 nalgebra 路径。 |

## 滤波器与后端的数值依赖

| 编号 | 生产方法与位置 | 剩余机制 | 对 PCM 的影响 |
| --- | --- | --- | --- |
| N08 | EAR [`decorrelate::filters`](../../rust/crates/mradm-ear/src/decorrelate.rs#L40) | 固定种子的随机相位经过平台 `sin/cos`，再做 f64 IFFT，最后转 f32 FIR。 | 中间差异可能在某些相位或布局下穿过最终 f32 舍入。现有 `ear-*.20-fir.f32` 有门禁；随机数发生器本身是确定的。 |
| N09 | RustFFT [`compute_twiddle/fill_bluesteins_twiddles`](../../rust/vendor/rustfft/src/twiddles.rs#L6)；realfft 3.5.0 `compute_twiddle` | FFT 已固定为标量规划，但 twiddle 仍用平台 f64 `sin/cos` 后转目标精度。 | 这是已有证据确认的中间值分歧：f64 twiddle 最大 1 ULP。已测 f32 表和最终 PCM 相同；其他长度和 f64 使用路径不能仅靠这一事实获得保证。与 N08 共用该依赖，不将两者的误差重复相加。 |
| N10 | HRTF [`Filters::new/grid_bin/grid_spectrum/query`](../../rust/crates/mradm-dsp/src/hrtf_filters.rs#L80) | 复数幅度使用 f32 `hypot`，用于加权幅度和归一化；复数和幅度在 `1e-9` 处切换相位回退。 | 可影响双耳滤波频谱与 PCM，近抵消点还可能改变分支。HRTF 边界探针已有门禁，但它不是所有数据集、频点和方向的证明。 |
| N11 | SOFA [`Dataset::sofa`](../../rust/crates/mradm-dsp/src/dataset.rs#L24) | Cartesian 测量坐标转方向仍使用平台 `atan2`；导入后的网格和插值继续经过 N05、N09、N10。 | 外部 SOFA 数据可能改变方向、网格、频谱和双耳 PCM。历史 118 项验收以 SOFA OFF / 内置 KEMAR 为条件；新清单已启用 SOFA 并门禁三份固定文件，但新增范围的三平台结果需另行确认，有限文件也不覆盖任意 SOFA。 |
| N12 | Triple Balance 非 D 模式的离线渲染与文件流式渲染：[`point/room/room_mix/raw/size_mix`](../../rust/crates/mradm-dsp/src/triple_balance/panner.rs#L702)、[`Position::advance`](../../rust/crates/mradm-dsp/src/triple_balance/mod.rs#L30)、[`Processor::control`](../../rust/crates/mradm-dsp/src/triple_balance/processor.rs#L407) | 这些入口选 `PORTABLE=false`，仍使用平台 `sin/cos/powf`；位置与 size 平滑常量使用 `exp`，也可能由编译器折叠。 | 点源、size 分配和动态控制包络可能不同。Scene 流式渲染的 `live_*` 入口和 D 模式已使用可移植数学函数；不能把这一项笼统套到全部 Triple Balance 路径。 |

## 计量和输出处理的数值依赖

| 编号 | 生产方法与位置 | 剩余机制 | 对 PCM 的影响 |
| --- | --- | --- | --- |
| N13 | [`Meter::new/add_frames/integrated`](../../rust/crates/mradm-dsp/src/meter.rs#L110) → ebur128 0.1.10 `filter_coefficients`、history、`energy_to_loudness` | K-weighting 系数使用 `tan/powf`，能量转 LUFS 使用 `log10`；门控与阈值也是浮点计算。 | 单纯显示 LUFS 时不改变 PCM；启用响度归一化时，测量值进入 N15 的增益计算。门控边界可能放大微小输入或系数变化。 |
| N14 | `Meter::true_peak/max_true_peak` → ebur128 0.1.10 `interp::InterpF::new` | True Peak 插值窗和 sinc 系数使用平台 f64 `cos/sin`，再转 f32。 | 峰值限制或峰值归一化会把结果反馈为整体增益。仓库关闭 ebur128 默认特性，`precision-true-peak` 中的显式 FMA 不是当前默认生产路径。 |
| N15 | [`RenderService::render`](../../src/adm_engine/render_service.cpp#L924)、[`apply_loudness_norm`](../../src/adm_loudness/loudness_normalizer.cpp#L126)、[`apply_peak_limit`](../../src/adm_peak/peak_limiter.cpp#L111)、各自有后端 `measured_peak_dbtp` | C++ `std::log10/std::pow`，以及 0.1 LU/dB、0.01 dB 等反馈判定边界。 | 可能改变是否执行增益或最后的 float 增益，从而影响整段 PCM。`measured_peak_dbtp` 并非总是纯日志字段，需按消费路径判断。 |
| N16 | HpTF [`design_band/section_response/cascade_response/extrema/peak_response/design`](../../rust/crates/mradm-dsp/src/hptf/design.rs#L15) | 平台 `powf/sin/cos/asin/log10`；`Complex64::norm` 内部还调用 `hypot`；峰值搜索有优先队列和停止阈值。 | 可改变 float biquad、稳定性判定、峰值元数据及 `auto_trim` 的实际增益。仅转换三角函数不足以封闭整条路径。 |
| N17 | [`StereoPeakGuard::new/next_gain`](../../rust/crates/mradm-dsp/src/peak_guard.rs#L18) | 释放系数 `1-exp(-1/(0.1*rate))` 仍使用平台 f32 `exp`。 | 触发限幅后的释放包络可能不同；未触发时不会仅因该系数产生增益差异。现有虚拟设备 DSP 用例覆盖有限采样率和信号。 |
| N18 | Monitor [`stereo_pan_for_azimuth/build_downmix_matrix`](../../src/adm_engine/monitor_session.cpp#L46) | C++ `std::sin/std::cos` 生成等功率折叠矩阵。 | 扬声器布局折叠为耳机立体声时直接改变左右增益；后续矩阵乘法已迁到 Rust，系数生成尚未统一。独立 Monitor 路径不属于基准 Scene 矩阵的完整覆盖。 |

“用 Rust 实现”本身不表示数学函数跨平台固定。上述固有方法仍可能调用平台 libm；已经显式使用 `libm::...`
或 `mradm_math::...` 的位置需要分别辨认。`sqrt`、基本加减乘除、固定规则的整数转换不因名称出现在扫描结果中就自动算作缺口。

## 状态与环境条件

| 编号 | 方法与位置 | 已确认的实现行为 | 比较时的要求 |
| --- | --- | --- | --- |
| S01 | ebur128 0.1.10 `filter::ftz::with_ftz`、`Filter` 的处理循环；入口为 [`Meter::add_frames`](../../rust/crates/mradm-dsp/src/meter.rs#L144) | x86/SSE2 分支临时打开 FTZ；其他架构走无 FTZ 令牌分支，并在块末将绝对值低于 `f64::EPSILON` 的单个滤波状态清零。 | 两种清理规则并不等价。这是源码确认的架构相关行为；对常规节目最终增益的影响仍需专门测量，尤其是长静音与极低电平、门控边界。 |
| S02 | HpTF [`Cascade::process_validated`](../../rust/crates/mradm-dsp/src/hptf.rs#L223) | 块末任一状态非有限，或全部状态绝对值不超过 `1e-20`，则清空全部历史。Processor 内部最多 1024 帧一块，外部短调用也会提前形成块末。 | 不同分块可能改变极小尾音清理和异常恢复位置；即使系数完全相同，也不构成任意分块逐位相同。 |
| S03 | [`Mixer::speaker/ear`](../../rust/crates/mradm-dsp/src/pcm_mix.rs#L345)；[`LiveConvolver::new`](../../rust/crates/mradm-dsp/src/convolution.rs#L75) | 开启 smoothing 时先取本次调用首尾增益，再按 `f/(frames-1)` 插值；卷积 FFT 长度由 HRIR 长度和 `maximum_frames` 决定。 | 分块改变插值端点和运算路径；改变最大块长可能改变 FFT 分解。固定配置分别比较，不能把所有分块配置互相当作逐位参考。 |
| S04 | [`HptfProcessor::process`](../../src/adm_render_common/hptf_eq.cpp#L361)、Monitor 更新快照、Scene `apply_available_controls` 与 [`now/head_tracking_recent`](../../src/adm_realtime/scene_stream_engine.cpp#L511) | HpTF 在下一次处理调用接收新目标；淡化中合并更新。头追有效期使用时钟，控制快照在工作线程或回调取样。 | 相同墙钟时刻的 UI 操作不保证相同音频样本落点。重放应固定事件样本位置、虚拟时钟、更新合并和淡化顺序。 |
| S05 | [`MonitorEngine::pull`](../../src/adm_realtime/monitor_engine.cpp#L810)、[`SceneStreamEngine::pull`](../../src/adm_realtime/scene_stream_engine.cpp#L2247) | 欠载时补零；实时回调与 push sink 对有效帧、补零尾部和 HpTF 历史推进有不同规则。 | CPU 负载、生产和消费调度可改变实际送出的 PCM。数值比较应使用无欠载的可控 sink，并比较有效帧数和 EOS，而不是只比较缓冲区长度。 |
| S06 | Scene `begin_epoch`、Monitor `clamp_frame/apply_seek_locked`、各后端 `render_window`；[`RenderService` 的 trim 帧换算](../../src/adm_engine/render_service.cpp#L496) | seek/reset 改变滤波、重采样、去相关与交叉淡化历史；不同入口还存在向零取整与 `llround` 的秒到帧转换。窗口渲染使用后端规定的 preroll。 | 固定确切样本位置、epoch、热启动或冷启动、preroll、尾部和 EOS。相同“秒数”或相同输出区间并不自动表示输入状态相同。 |
| S07 | [`MRStrictFp.cmake`](../../cmake/MRStrictFp.cmake#L23)、[`MRRust.cmake`](../../cmake/MRRust.cmake#L63)、C ABI 宿主线程；双耳 [`extent_spread_deg`](../../src/adm_render_binaural/binaural_renderer.cpp#L495)、[`BinauralStream::render_current_sources/render_block`](../../src/adm_render_binaural/binaural_renderer.cpp#L1411) | 生产 C++ 全局严格浮点开关默认 OFF。双耳锥角仍有乘加，文件流式双耳逐源归约仍做 `output += sample * gain`，拓扑切换仍用 C++ 两路乘加淡化。公开调用没有统一建立整个线程的舍入模式、FTZ/DAZ 环境。 | 这些生产表达式仍可能受编译器融合影响，应检查实际指令或增加独立算术规则。固定编译器、Cargo.lock、依赖 provider、有效 flags 和宿主浮点环境。`scene-separate-v1` 只封闭其覆盖的 Scene 运算；诊断用 `mul_add` 不能当成当前生产缺陷。 |
| S08 | 按 `start_sample` 排序的 5 处：[`EAR`](../../src/adm_render_ear/ear_renderer.cpp#L445)、[`VBAP`](../../src/adm_render_vbap/vbap_renderer.cpp#L366)、[`HOA`](../../src/adm_render_hoa/hoa_renderer.cpp#L201)、[`双耳 spreader`](../../src/adm_render_binaural/binaural_renderer.cpp#L653) 与 [`双耳 OLA`](../../src/adm_render_binaural/binaural_renderer.cpp#L739) | 使用非稳定的 `std::ranges::sort`，同一起点的不同块没有显式次级键，标准不保证保留相对顺序。 | 若存在同起点块，不同 STL 的等价元素顺序可能改变生效块或插值前驱。尚无本轮三平台复现；需先定义同起点优先规则，覆盖少量和超过 32 个同起点块、空块及裁剪边界。稳定排序只是保留原顺序的一种实现选择，不能替代语义定义。 |

S01 和 N13 描述同一计量模块的不同机制；N08 和 N09 也存在上下游依赖。
条目计数用于定位工作，不用于计算误差总和或独立缺陷数量。

代码扫描还会命中 `render_common.h` 中未被生产调用的 `interpolated_scalar` 模板、测试信号的 `sin`、
诊断参考路径和纯 UI 峰值/RMS 显示。这些没有作为额外 PCM 风险条目计数。

## C++ 剩余执行点

下表补充语言与调用路径，复用上面的审计条目和分期，不另设一套风险编号。
“确定性运算”表示在相同输入、顺序和浮点环境下没有新增平台函数依赖，并不表示它已经迁入 Rust。

| 路径 | 仍在 C++ 执行的工作 | 性质与归属 | 阶段 |
| --- | --- | --- | --- |
| Scene 策略与状态 | `apply_policy_to_state`、共享 `apply_*_override` 的 dB 换算、增益相乘、偏移/钳位、方向乘距离。 | N01/N03/N04；既有平台函数，也有确定性标量运算。策略匹配与数值求值的边界需明确。 | 一期 |
| Scene VBAP | `gains_for` 中距离下限、发散子源按声道累加，DirectSpeakers/LFE 路由及增益组装。 | N05/S07；底层 panner 和 mixer 在 Rust，系数组装仍有 C++。 | 一期 |
| Scene Triple Balance | [`configure_generation` 调用 `bed_gains`](../../src/adm_render_triple_balance/live_triple_balance_renderer.cpp#L184)；[`bed.cpp`](../../src/adm_render_triple_balance/bed.cpp#L83) 根据布局路由，并计算分功率系数。 | 确定性系数生成也属于自有数值实现；该适配器不能整体标成“只搬运”。 | 一期 |
| Scene 输出 | [`SceneOutputSession::pull`](../../src/adm_engine/scene_output_session.cpp#L230) 在无设备音量且不走立体声保护时逐样本乘音量。 | 确定性单次乘法；仍需纳入 Rust 批量样本处理。 | 一期 |
| Scene 帧计数 | `SceneStreamEngine` 的输入/输出帧边界有理数累加。 | S06；纯整数运算不是已证实分歧。锁定帧映射规则和测试，不要求仅为换语言而搬走调度计数。 | 一期验收 |
| 公共路由与布局 | [`direct_speakers_matrix`](../../src/adm_core/direct_speakers_matrix.cpp#L135) 的权重归一化；声道床/布局常量；标签规范化。 | N01；`sqrt(weight/sum)` 与固定求和本身确定，常量资源需保持语义同步。Scene 实际使用部分随调用链前移。 | 一期或三期 |
| 文件流式及离线后端准备 | EAR double 增益乘积与发散累加、VBAP 增益表组装、HOA 元数据分组、Triple Balance 床增益、5 处块排序。 | N03/N06/N07/N12/S08；包括确定性运算与顺序风险。 | 三期 |
| 双耳文件流式渲染 | 逐源加权归约、拓扑两路淡化、spreader 锥角计算。 | S07；有潜在乘加融合，直接改变 PCM 或滤波控制参数。 | 三期 |
| 双耳离线渲染 | OLA 源归约、LFE 旁路、spreader 分组/延迟环归约与尾部排空；[`BinauralSpreaderAdapter::push_batch/drain_ring`](../../src/adm_render_binaural/binaural_spreader.cpp#L120) 仍逐样本施加输入增益并累加输出。 | S07；归约顺序固定，乘加与单纯加法分别审查。 | 三期 |
| 文件流式渲染的监听适配 | Monitor 折叠矩阵、实时覆盖增益、`clamp_frame` 秒到帧换算。 | N03/N18/S06；实时监听用途不改变其所属渲染流程。 | 三期 |
| 离线后处理 | 峰值转 dB、总增益合成、`apply_gain_to_file`；公开 `apply_loudness_norm` / `apply_peak_limit` 的独立计算。 | N13—N15/S01；统一为公共 Rust 数值入口，保留各 API 已定义的行为。 | 二期 |
| PCM 输出控制 | 秒到帧 trim、写出块循环与临时文件替换。WAVE 样本量化和序列化已经在 Rust。 | S06；数值边界统一，I/O 生命周期继续由 C++ 管理。整数 PCM 继续拒绝 NaN。 | 三期 |

## 已收敛的实现与 PCM 写出

| 链路 | 当前实现 | 本次判断 |
| --- | --- | --- |
| ADM 数字与时间解析 | [`document::number/samples`](../../rust/crates/mradm-adm/src/document.rs#L381)：Rust 十进制解析；时间使用 u128 纳秒与整数舍入。 | 不再依赖 C++ stream 或平台 long double；应与 N02 的后续数学转换分开。 |
| HpTF 文本解析 | [`parametric_eq::number`](../../rust/crates/mradm-dsp/src/hptf/parametric_eq.rs#L60)：固定 C locale 字节规则，拒绝十六进制和非零下溢。 | 前次发现的解析分歧已统一；N16 和 S02 仍然存在。 |
| PCM 输入 | [`Reader::read_frames`](../../rust/crates/mradm-wav/src/reader.rs#L263)：显式小端整数转换与二的幂缩放，float32 按位读。Scene 直接接受调用方 f32。 | 相同合法字节和格式没有新增随机路径。PCM32 输入转 f32 的精度损失是确定的格式转换。通用 `FloatFlacReader` 仍用 dr_flac；它不是 ADM WAVE 主输入的替换实现，也不由 WAVE 门禁证明。 |
| 重采样 | [`Resampler`](../../rust/crates/mradm-dsp/src/resampler.rs)、vendored rubato 固定标量归约、[`mradm_math` 窗和 sinc](../../rust/vendor/rubato/src/windows.rs#L3)。 | 已处理原有运行时 SIMD 和平台三角函数差异；已有 `trig.*` / `resampler-*` 门禁。仍固定比率、质量、初始延迟、reset 和 EOS。 |
| Spreader 与 SVD | 固定 8 组预算、按索引归约、稳定源 seed；nalgebra `libm-force`；spreader 几何与滤波系数显式走 libm。 | 已处理二期的根因，不再把硬件线程数本身列作剩余随机混音源。见 [ADR 0017](../adr/0017-deterministic-spreader.md)。 |
| FFT 执行 | 锁定 RustFFT 标量规划与保持算术树的独立列优化。 | SIMD 分派问题已收敛；表生成另见 N09。 |
| 混音与随机性 | Rust PCM 混音按已准备的 row 顺序累加；几何凸包按插入顺序处理边；EAR MT19937 和 diffuse/spreader PCG seed 固定。 | 未发现把 HashMap 随机遍历或 `random_device` 用于这些 PCM 归约的现行路径。改变对象身份、布局或顺序属于输入改变。文件名随机数不进入 PCM。 |
| HRTF 缓存 | [`Filters` 查询缓存](../../rust/crates/mradm-dsp/src/hrtf_filters/cache.rs)、[`PreparedHrtfs`](../../rust/crates/mradm-dsp/src/live_binaural/prepared.rs)保留原有邻居和乘加顺序，缓存保存已计算结果。 | 缓存容量或命中本身未发现新的数值算法切换；需继续保持命中与未命中逐位回归。 |
| WAV float32 | [`Writer::write_frames`](../../rust/crates/mradm-wav/src/writer.rs#L136)直接 `sample.to_le_bytes()`。 | 有限样本位模式相同就写出相同样本字节。NaN payload 若在上游不同，writer 不会替上游统一它。 |
| WAV 整数 PCM | 同一 writer：f32 裁剪到 [-1,1]，转 f64 乘 32767 / 8388607 / 2147483647，再向零截断；NaN 拒绝。 | 无随机 dither，无平台 `lrint`。相同 f32 输入在默认浮点环境下有明确结果；不同上游样本在整数阈值附近可能相差 1 LSB，也可能量化成相同整数。 |
| CAF float32 | [`FloatCafWriter::write`](../../src/adm_audio/caf_io.cpp#L400)直接 `fwrite(float)`，文件声明 little-endian float32。 | 现有 macOS arm64、Linux x64、Windows x64 均为小端；若新增大端平台需改为显式小端序列化。当前三平台上没有额外 DSP。 |
| 后处理重写 | `apply_gain_to_file`、trim、`downconvert_to_int` 使用同一 WAVE 读写层；增益是单独的 f32 乘法。 | 增益来源见 N15，帧边界见 S06。成功完成后的样本比较不应包含临时路径、metadata 或文件头差异。 |

## 本轮实施优先级

按维护者 2026-10-09 的要求，**Scene 相关工作明确列为本轮一期**。
这里的一期、二期、三期是本轮剩余数值实现的实施顺序，与已完成的 Rust 数值一致性历史分期分别记录。
本节为待实施计划；前面的源码审计与历史验收结果不因此变为已完成的迁移。

| 阶段 | 优先范围 | 完成目标 |
| --- | --- | --- |
| 一期 | Scene 流式渲染及其全部实际数值依赖，从 Scene 参数输入到应用侧输出 PCM。 | Scene 所用的数值计算集中在 Rust；共享数学函数、系数、输出 DSP 和状态规则纳入统一实现与三平台精确门禁。 |
| 二期 | 响度、True Peak 及其归一化反馈。 | 计量器、反馈增益与样本增益处理形成可重复的数值链路，兼顾计量准确性和最终 PCM 一致性。 |
| 三期 | 文件流式渲染、离线渲染及其余专用入口。 | 收走其余自有 C++ 数值运算，完成 EAR、HOA、Triple Balance 相应路径及最终 PCM 转换的覆盖。 |

分期按实际调用链确定。**Scene 调用到的共享实现属于一期**，即使它同时服务于文件流式渲染或离线渲染。
不能把 Scene 依赖的 FFT、HRTF、SOFA、增益换算或输出 DSP 推迟到“公共模块”阶段。
共享实现的修改会同步运行其他使用方的相关回归，但不因此扩大一期为所有后端的整体迁移。
Apple 后端和后续编码继续排除。

## 一期 Scene 流式渲染

一期边界为 `SceneStreamEngine` → `ILiveSceneRenderer` → 重采样与过渡 → `SceneOutputSession` → 应用侧 PCM。
包含 VBAP、双耳与 Triple Balance 的 Scene 实现、语义策略对数值状态的变换，以及设备绑定立体声输出。
输入的 PCM 和元数据由调用方提交；实际进入 Scene 的参数解析或共享数值投影也在本期处理。

| 执行顺序 | 工作 | 具体范围与交付 |
| --- | --- | --- |
| 1.1 | 固定数值和重放规则，建立检查点 | 明确 f32/f64 精度、乘加舍入、可移植数学函数、事件样本位置、时钟、reset 和 EOS。沿用现有 Scene 精确门禁，记录有效状态、系数、过渡前后和输出 PCM，形成后续迁移的对照。 |
| 1.2 | 迁移 Scene 的 C++ 数值残留 | `SceneStreamEngine::apply_policy_to_state` 使用的增益及坐标运算、语义策略数值求值、Scene VBAP 增益合成与路由系数、Triple Balance 床系数等。C++ 保留对象匹配、参数打包、队列和生命周期；Rust 接收批量数值输入，避免逐标量跨 FFI。 |
| 1.3 | 统一空间计算与卷积准备 | `scene_math`、Scene VBAP 和 Triple Balance 所用几何、HRTF 网格和幅度插值、SOFA 坐标转换，以及 Scene 实际使用的 FFT 表。明确几何阈值与近相消分支。已经可移植的 Triple Balance Scene 数学、重采样和固定归约继续保留并做回归。 |
| 1.4 | 完成 Scene 输出 DSP | HpTF 系数与自动衰减、极小尾音清理，StereoPeakGuard 释放系数，以及 `SceneOutputSession::pull` 中仍在 C++ 执行的输出音量乘法。将应用侧样本处理纳入 Rust，保留既有 HpTF → 峰值保护顺序。 |
| 1.5 | 验证状态与控制时序 | 参数更新、头追有效期、generation、epoch、切换、seek/preroll、短尾、EOS 和欠载规则。按固定样本位置重放事件；对宣称分块无关的内核增加跨分块精确测试，其余路径明确最大块长和分块条件。 |
| 1.6 | 三平台验收 | macOS arm64、Windows x64、Linux x64 的 Release 候选在相同配置和事件下逐位相同；覆盖内置 HRTF、固定外部 SOFA、变采样、HpTF 切换、峰值保护和多声道输出。回归准备后的处理分配、锁和 I/O 约束，保留公开 ABI。 |

相关审计项：N04、N05、N09—N11、N16、N17，以及 N01/N03/S03—S07 中服务于 Scene 的部分和 S02。
一期对精确一致性的要求适用于锁定输入与配置的矩阵，并不把真实设备的墙钟操作或欠载输出当作相同事件序列。
数值内核的迁移与统一完成后，C++ 中若仍存在直接决定 Scene 样本值的运算，应登记原因和边界后才能结项。

## 二期 响度与 True Peak

当前 Scene 核心与 `SceneOutputSession` 没有通过 EBU R128 / True Peak 计量反馈调整 PCM。
因此 N13、N14、N15 和 S01 排在二期；一期中的 `StereoPeakGuard` 是样本峰值保护，仍按一期处理。
若实施时确认某项共享计量或反馈已进入 Scene 的数值链路，该部分随 Scene 前移到一期。

1. 先统一 True Peak 插值系数生成和累加规则，保持现有滤波长度与过采样策略。
2. 再统一响度 K-weighting、能量门控与 LUFS 换算，处理 ebur128 在 x86/ARM 上不同的极小状态清理。
3. 将离线后处理的反馈增益计算、dB/linear 转换和逐样本增益应用集中到 Rust；复用一期建立的公共数值规则。
4. 同时验收独立计量准确性与三平台逐位一致性，覆盖门限边界、跨采样峰值、长静音、分块、reset，检查归一化后的最终 PCM。

保持既有声道权重、LFE 处理、完整积分历史、worker 归属和错误语义；声道映射修正或计量算法变更另行评估。
对 ebur128 使用可追踪的小范围补丁，不以更换语言或更新依赖版本代替数值规则和标准测试。

## 三期 文件流式渲染与离线渲染

按以下顺序处理一期和二期没有覆盖的路径，复用已收敛的共享实现：

1. **文件流式双耳与监听适配**：`BinauralStream` 的逐源加权混音、拓扑淡化、spreader 锥角，以及 Monitor 折叠矩阵。它们属于文件流式渲染或相应监听适配，不能因为用于实时监听就记成 Scene 一期任务。
2. **后端专用数值**：EAR 点源/extent/DirectSpeakers 与去相关 FIR、HOA 编解码、Triple Balance 非 D 模式的离线与文件流式运算。为 D 模式建立独立精确门禁；其控制与系数不能借用其他模式的验收结果。
3. **文件与命令参数入口**：ADM 增益与坐标投影、CLI 数字解析、专用 JSON/标签解析，以及尚未复用公共 Rust 数值入口的策略或覆盖项；为 S08 明确同起点块顺序，再随块时间线迁移落实。
4. **最终 PCM 与全链路验收**：验证 trim、增益重写、整数 PCM 量化、float 写出及声道排列。已明确的量化规则优先补边界覆盖，迁移剩余数值运算；文件生命周期与调度继续留在 C++。

相关审计项：N02、N06—N08、N12、N18、S08，以及 N01/N03/S03—S07 的其余部分。
一期和二期完成的共享实现继续受门禁保护，三期新增路径不能重新引入平台数学函数或不同乘加规则。

若 S08 在当前生产输入上复现出实际错误，可独立提前修复并保留回归；尚未复现时按三期计划推进，
不把猜测的跨平台块顺序差异写成已测事实。

各阶段都保存候选的有效参数位模式、系数、适用的反馈增益、最终 float PCM 和整数 PCM，并将数值分歧与事件/帧数分歧分开定位。
跨平台精确比较的对象是同一候选版本；历史生产实现与独立参考用于准确性和行为对照。预期数值变化需要记录依据和误差，不能直接刷新期望值或放宽门禁来代替验收。
每一批新增精确门禁均附对应的三平台 Release 结果。

## 每个迁移切片的验证

一个切片同时承担实现迁移与一致性收敛，提交中分别说明结构变化和有意的数值变化：

1. **冻结与对照**：保存切片开始前的源码、参数、资源和构建条件，确认参考实现的精度、运算树、FMA、排序及错误语义。
   对已固定算术规则且承诺行为不变的迁移要求位比较。C++ 与 Rust 使用同一平台数学库，不足以证明它们逐位等价；
   C++ 原生产输出已有融合或平台特有行为时，应记录原版对照，并按 [ADR 0014](../adr/0014-scene-arithmetic-policy.md)
   的方式建立明确的算术参考或兼容基线，避免要求一个候选同时复现几套互相不同的结果。
2. **一致性收敛**：显式使用可移植数学函数，固定排序、状态清理和边界规则。系数与 PCM 的预期变化单独说明，
   用独立精度/语义测试与三平台候选间的精确门禁同时验收。稳定排序、Rust 浮点转整数和换解析器都不自动等价于原行为。
3. **错误行为**：保留可恢复错误及无效输入不污染状态的约定。整数 WAV 当前拒绝 NaN，不改成静默归零。
   FLAC 与 WAV 之间的量化方式统一不属于本轮范围。

结构迁移与数值修正可以拆成两个相邻提交，分别做灵敏度检查；只有约定的精确对照和跨平台门禁均成立后才完成切片。
不额外为每个切片创建独立大缓存，复用现有 Debug/Release 树与共享 Cargo/FetchContent 缓存。

## 每个切片的性能回归

以下为待实施切片的检查要求，不是本次文档整合已经执行的基准结果。
比较切片开始前与完成后的 Release 构建；数值一致的结构迁移提交可另测，以区分 FFI/组织变化和可移植数学函数的成本。

| 检查面 | 方法 | 范围与判定 |
| --- | --- | --- |
| 离线耗时与 RSS | 复用 [`benchmark-dsp.py`](../../scripts/consistency/benchmark-dsp.py)，预热后交替运行基线/候选，至少 5 轮，保留单轮数据和中位数。 | 现有 5 个用例都带 `--no-peak-limit` 且输出 f32；按切片扩充策略、启用峰值限制、响度目标和 i16/i24 输出。先扩展脚本的选项，不能以现有固定参数声称已测这些路径。 |
| PCM 对照 | 使用 Release `mr_adm_pcm_bits` 及 `--pcm-tool` 保存最终 PCM 指纹。 | 仅对承诺逐位不变的比较加 `--require-identical-pcm`。有意更新数学规则时比较独立准确性、变化明细和三平台候选，不能跳过重复性检查。 |
| Scene worker 与输出 | 扩充 Release Scene 基准，按样本时钟重放增益、位置、策略和切换；覆盖 VBAP、双耳、Triple Balance 与输出 DSP。 | 记录 worker 每块耗时 p50/p99 与实时预算之比、欠载和输出帧数；准备阶段及稳态处理分别计时。已有 Triple Balance 基准或 stress 工具不能替代整个 Scene 覆盖。 |
| 文件流式渲染 | 对 `IRenderStream::process` 及适用的监听输出增加直接基准。 | 覆盖逐源归约、拓扑淡化和折叠，固定块长与控制序列；离线 CLI 性能不能自动代表该路径。 |
| 分配与实时约束 | 参照 [`realtime_allocations.rs`](../../rust/crates/mradm-dsp/tests/realtime_allocations.rs)，覆盖准备后首次处理、每次状态更新、淡化完成、reset 和输出增益。 | 新增 Rust 数值处理入口准备后零分配；控制侧准备允许分配。回调路径继续无锁、无 I/O。不要将该内核约束扩大为整个 C++ 工作线程已零分配。 |
| 平台与退化调查 | 复用受控本机环境；现有脚本的 RSS 收集支持 macOS/Linux。Windows 验收补计时，RSS 未采集时明确记为不可用。 | 耗时中位数比超过 1.05 或 RSS 比超过 1.10 时，复测定位并优化或记录接受依据；它们是调查阈值，不是共享 CI runner 的硬性性能门禁。Scene p99 及实时预算单独判断。 |

结果写入对应切片的 `RUST_*_MIGRATION.md`，原始 JSON 放入 `evidence/`，记录二进制、依赖、资源指纹与有效配置。
测量输出及时清理；用于听音、幅度分析和最终 PCM 验证的音频始终使用 Release。

## 依赖源码定位

项目依赖声明见 [`rust/Cargo.toml`](../../rust/Cargo.toml) 和 [`MRDependencies.cmake`](../../cmake/MRDependencies.cmake)。
以下为检查到的锁定依赖源码位置，便于在共享依赖缓存中复核：

- CLI11 2.5.0：`include/CLI/TypeTools.hpp:1146`，浮点 `lexical_cast`。
- nlohmann JSON 3.12.0：`include/nlohmann/detail/input/lexer.hpp:913`、`:1289`，浮点转换。
- ebur128 0.1.10：`src/filter.rs:64`、`:238`、`:376`，K-weighting 和 FTZ/清理分支；`src/interp.rs:101`，True Peak 窗；`src/utils.rs:25`，LUFS 转换；`src/history.rs`，能量门控。
- realfft 3.5.0：`src/lib.rs:85`，twiddle 生成。
- num-complex 0.4.6：`src/lib.rs:217`，`Complex::norm` 经标量 `hypot`；nalgebra 的 `libm-force` 不等于全局替换这一实现。

统计依据为基准源码的调用链核查和已存档证据；本记录不作为一轮新的跨平台音频实测报告。
