# PCM 一致性审计与 Rust 迁移路线

统计日期：2026-10-09。源码与门禁基准：`456a3baaa338c8b7cc3da74951f28a8d5dfe5cc8`。
四期自 2026-10-16 起，基准为 `0a3b362`（三期结项），方向见 [ADR 0021](../adr/0021-rust-primary-project.md)。

从 ADM / 参数输入到最终 PCM，项目自有渲染链路仍有 **18 组数值实现依赖、8 组状态或环境条件**需要纳入一致性管理。
这些是按生产入口和影响机制归并的 26 个审计条目，不代表发现了 26 个已复现的跨平台 PCM 缺陷。
平台数学函数尚未统一的路径很多；已有用例逐位相同，只约束这些用例及其构建条件。

本文件统一维护剩余数值运算清单与本轮分期路线，吸收 `d42e294` 中的 C++ 审计和性能检查。
原分支的审计内容保留在 Git 提交历史中；各 ADR、迁移验收记录和原始证据继续作为独立历史记录引用。
N/S 条目表描述基准提交时的机制，保留原文以便追溯；一期结项后的处理情况见[条目状态](#条目状态)。

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
2026-10-16 起，这一边界由 [ADR 0021](../adr/0021-rust-primary-project.md) 推进：四期把 PCM 写出为止的数据链路整体迁入 Rust，C++ 只保留适配壳与调度；五期起编排、实时调度、C ABI 实现与 CLI 也按 [Rust 主导逆转路线](RUST_INVERSION_ROADMAP.md) 迁入 Rust。

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
- Scene 主 epoch 在样本 6144 执行语义策略更新，短尾 epoch 重新清空策略。自 1.2 起该更新包含
  `gain.scale=0.5`、`gain.gain_db=-4.6`（libm 与 glibc powf 在此相差 1 ulp）、`extent`、`diffuse` 和 `divergence.range_scale`；
  此前只有 `gain.scale=0.5`。1.3 起，VBAP 和双耳回放再加入 `position.offset`（方位 12.5°、仰角 −4°），经可移植的 `scene_math` 转为极坐标；Triple Balance 只接受房间立方体内的 Cartesian 位置，不加该项。
  仍缺少身份匹配、channelLock 等策略矩阵。
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
| N15 | [`RenderService::render`](../../src/adm_engine/render_service.cpp#L924)、`apply_loudness_norm`、`apply_peak_limit`（无生产调用者，4.8 A 删除）、各自有后端 `measured_peak_dbtp` | C++ `std::log10/std::pow`，以及 0.1 LU/dB、0.01 dB 等反馈判定边界。 | 可能改变是否执行增益或最后的 float 增益，从而影响整段 PCM。`measured_peak_dbtp` 并非总是纯日志字段，需按消费路径判断。 |
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

## 条目状态

一期结项（`d288d3c`）后各条目的处理情况。“已处理”指 Scene 链路上的该机制已统一并有门禁；
共享实现随之改变的离线与文件流式路径一并受益，但这些路径的完整覆盖仍按二期、三期验收。

| 条目 | 状态 | 处理与剩余 |
| --- | --- | --- |
| N01 | 已处理 | 3.5：CLI 浮点选项与 HOA 标签改用 Rust 正确舍入、与 locale 无关的十进制解析（binary32 参数一次舍入），UID / 扬声器标签 / 布局名等的大小写规范化与字符分类改为只看 ASCII；JSON 数字原经 nlohmann（`strtod`），1.6 登记为依赖三个 C 运行时的正确舍入；4.4 起语义策略、DirectSpeakers 矩阵与屏幕配置的 JSON 由 Rust `mradm-json` 解析，不再经 C 运行时 |
| N02 | 已处理 | 3.5：ADM 导入的 dB 增益与 Cartesian DirectSpeakers 投影改用 `libm`（`mradm-adm` `numeric`），`adm-import.*` 内核设为门禁 |
| N03 | 已处理 | 语义策略的 dB 换算与乘积已迁入 Rust `semantic_policy`（1.2，`libm::powf`），离线与文件流式共用；3.1：`resolve_live_channel_gain`、HOA `set_overrides` 与覆盖溢出检查改用同一 `libm::powf`；3.3：HOA 文件流式改用共用的按声道解析（扬声器标签优先于整体覆盖）；3.5：核对语义策略、实时覆盖与 Monitor 的全部增益入口都经 Rust `libm::powf`，C++ 不再有 `std::pow` |
| N04 | 已处理 | Scene 空间数学改用 `libm`（1.3） |
| N05 | 已处理 | `geometry::direction` 与 VBAP spread 环改用 `libm::sincos`（1.3） |
| N06 | 已处理 | 3.2：EAR 几何、extent 权重与立体声 panner 改用 `libm`，`ear-gains.*` 内核设为门禁；EAR HOA 解码的球谐与采样方向属 N07（3.3） |
| N07 | 已处理 | 3.3：HOA 编码与 EAR HOA 解码改用 `libm`，解码器的矩阵乘积改为固定顺序的标量计算（不再走 matrixmultiply 的运行时 SIMD 内核），`hoa-encode.*` 与 `ear-hoa.*` 内核设为门禁 |
| N08 | 已处理 | twiddle 部分随 N09 处理；3.2：去相关随机相位改用 `libm::sin/cos`，`ear-*.20-fir.f32` 继续门禁 |
| N09 | 已处理 | RustFFT 与 RealFFT twiddle 改用 `libm`（1.3），f64 twiddle 列设为门禁 |
| N10 | 已处理 | HRTF 幅度改用 `libm::hypotf`（1.3）；`1e-9` 相位回退是固定规则 |
| N11 | 已处理 | SOFA 坐标改用 `libm::atan2`（1.3）；门禁只覆盖仓库内三份 SOFA |
| N12 | 已处理 | 3.4：去掉 `PORTABLE` 泛型，标准模式的离线、文件流式与 Scene 共用同一组 `libm` 实现与 size 积分格点，位置与 size 平滑常量改用 `libm::expf`；`triple-balance.*` 内核与 D 模式离线用例设为门禁 |
| N13 | 已处理 | 2.2：K-weighting、门控系数与 LUFS 换算改用 `libm`，计量探针全部设为门禁 |
| N14 | 已处理 | True Peak 插值系数改用 `libm`（2.1），生产计量的 True Peak 设为门禁 |
| N15 | 已处理 | 2.3：峰值转 dB、合成增益判定、两个公开后处理 API 的增益与逐样本乘法移入 Rust（`libm`）；对象增益覆盖的 dB 换算留在三期 |
| N16 | 已处理 | HpTF 设计改用 `libm`（1.4），设计内核设为门禁 |
| N17 | 已处理 | 峰值保护释放系数改用 `libm::expf`（1.4） |
| N18 | 已处理 | 3.1：Monitor 折叠矩阵改用 `libm::sinf/cosf`，`stream-dsp.20-fold.f32` 设为门禁 |
| S01 | 已处理 | 2.2：去掉 x86 FTZ，所有架构统一清零低于 `f64::EPSILON` 的滤波器状态；2.4：清零改在 100 ms 块网格上，读数与分块无关 |
| S02 | 已处理 | HpTF 尾音清理改为自 reset 起的 1024 帧网格（1.4） |
| S03 | 保留（分块条件） | Scene 的分块条件已在 1.5 逐环节记录，并对 11 个用例设分块无关门禁；双耳卷积与分数比率重采样依赖分块，按分块配置分别门禁；三期结项：文件流式 `Mixer` 的对象平滑按每次调用的首尾增益插值，依赖请求分块，三期的文件流式重放都不开平滑，这一路径没有门禁 |
| S04 | 保留（运行时条件） | HpTF 排队目标在淡化结束帧开始（1.5，Scene 与文件流式监听共用）；回放固定事件位置与虚拟时钟；三期结项：Monitor 按墙钟取样控制快照，同一 UI 操作落在哪个样本取决于调度，不要求逐位相同 |
| S05 | 保留（运行时条件） | Scene 欠载补零不推进 HpTF 与峰值保护（1.5）；三期结项：Monitor 是否欠载取决于调度，数值比较使用无欠载的文件流式与 Scene 重放 |
| S06 | 已处理 | Scene epoch、preroll、短尾与 EOS 规则已锁定（1.5）；3.6：输出裁剪、预览窗口与 Monitor 定位 / 循环共用 Rust `timeline::seconds_to_frame`（最近帧，半帧远离零），`timeline.*` 内核与半帧边界裁剪用例设为门禁 |
| S07 | 保留（构建条件） | Scene 由 `scene-separate-v1` 与 Rust 实现覆盖，一致性 config A 改用发布构建选项（1.6）；3.1：双耳 `extent_spread_deg`、`BinauralStream` 归约与拓扑淡化移入 Rust 并有文件流式重放门禁；3.2：核对 EAR 的 C++ 部分只剩单独的加法与乘法，不受乘加融合影响，EAR 文件流式重放设为门禁；3.3：HOA 的 C++ 部分同样不含可融合的乘加，HOA 文件流式重放设为门禁；3.4：Triple Balance 的 C++ 部分只做校验与编排，数值运算全部在 Rust，文件流式重放设为门禁；三期结项：门禁依赖固定的工具链、`Cargo.lock`、有效编译选项与默认宿主浮点环境，C++ 剩余乘法与加法都是单独语句，锁定的三个平台不会融合（GCC 在 C++ 中默认 `-ffp-contract=fast`，不在矩阵内的组合不受保护）；4.7：双耳的声源计划、OLA、spreader 与文件流式编排全部移入 Rust，C++ 适配壳不再含 PCM 或滤波控制参数的算术 |
| S08 | 已处理 | 3.5：在 libstdc++ 上复现（40 个同起点块时旧实现生效的不是文档中的最后一块）。规则：同起点块保持场景（ADM 文档）顺序，最后一块在其整个区间生效，被覆盖块不留下分量、插值前驱或路由判断。双耳、EAR、VBAP、HOA、Apple、22.2 LFE 路由与 VBAP 高度判断都在建立增益、总线或路由前经 `render_common::last_blocks_at_each_start` 只保留每个起点的最后一块；`binaural_render_fixture_test` 与 `same_start_blocks_test` 与仅含获胜块的场景逐位比较，`params-*` 用例设为门禁 |

## C++ 剩余执行点

下表补充语言与调用路径，复用上面的审计条目和分期，不另设一套风险编号。
“确定性运算”表示在相同输入、顺序和浮点环境下没有新增平台函数依赖，并不表示它已经迁入 Rust。

| 路径 | 仍在 C++ 执行的工作 | 性质与归属 | 阶段 |
| --- | --- | --- | --- |
| Scene 策略与状态 | dB 换算、乘积、偏移/钳位和方向乘距离在 Rust `semantic_policy`（1.2），极坐标经 `libm`（1.3）；解析、匹配、过滤、布尔字段与逐更新应用 4.4 起在 Rust `mradm-scene` `live_policy`。 | 4.4 起无 C++ 运算；JSON 数字由 `mradm-json` 解析（N01）。 | 一期（已结项）；四期 4.4 迁入其余部分 |
| Scene VBAP | 4.5 起整个渲染器在 Rust `mradm-render` `live_vbap`（元素生成、逐帧暂存、声道映射与 LFE 路由、warn_once），C++ 只转换帧记录。`split_power` 模式的 LFE 增益为常量 1 乘以固定系数，结果精确；channelLock 与 divergence 阈值 1.3 起在 Rust。 | 无 C++ 运算。 | 一期（已结项）；四期 4.5 迁入其余部分 |
| Scene Triple Balance | 床系数由 Rust `triple_balance::bed` 生成；4.6 起整个渲染器在 Rust `mradm-render` `live_triple_balance`（床标签与 lane、逐帧暂存、spread=none 警告），C++ 只转换帧记录。 | 文件、文件流式和 Scene 共用；系数为精确常数。4.6 起无 C++ 运算。 | 一期（已结项）；四期 4.6 迁入其余部分 |
| Scene 输出 | 1.4 起 `SceneOutputSession` 不再有决定样本值的 C++ 浮点运算；无保护路径的音量在 Rust `gain::scale` 中相乘。 | 只剩缓冲清零和拷贝；回调路径无锁、无 C++ 分配（1.6）。 | 一期（已结项） |
| Scene 帧计数 | `SceneStreamEngine` 的输入/输出帧边界有理数累加。 | S06；纯整数运算，规则与测试在 1.5 锁定。 | 一期（已结项） |
| 公共路由与布局 | DirectSpeakers 矩阵的权重归一化（4.4 起在 Rust `mradm-scene` `direct_matrix`）；声道床/布局常量（4.3 起在 `mradm-scene`）；标签规范化、DS 矩阵按块路由与 22.2 LFE 路由（4.5 起在 `mradm-render` `routing`，C++ 只剩给 Apple 岛、双耳以及 VBAP 布局注册与渲染器工厂的 LFE 标签判定用的薄包装）。 | `sqrt(weight/sum)` 与固定求和本身确定。1.6 核对确认 Scene 不调用 DirectSpeakers 矩阵。 | 三期核对为确定性运算；4.3 迁入布局表，4.4 迁入 DS 矩阵 JSON，4.5 迁入路由与标签 |
| 文件流式及离线后端准备 | EAR double 增益乘积与发散累加、VBAP 增益表组装、HOA 元数据分组与扬声器离线驱动 4.5 起在 Rust `mradm-render`；Triple Balance 的规范化、床与计划，room 的事件表与运行时 4.6 起也在其中，双耳的声源计划与块排序 4.7 起也在其中。 | N03/N06/N07/N12/S08；包括确定性运算与顺序风险。 | 三期已收敛数学函数与同起点块；四期 4.5 迁入 EAR、VBAP、HOA，4.6 迁入 Triple Balance 与 room，4.7 迁入双耳 |
| 双耳文件流式渲染 | 逐源加权归约、拓扑两路淡化、spreader 锥角计算。 | S07；有潜在乘加融合，直接改变 PCM 或滤波控制参数。 | 三期（3.1 已迁移）；四期 4.7 C 起其余编排（FIFO、seek、gain ramp、拓扑重建）也在 Rust [`binaural_stream`](../../rust/crates/mradm-render/src/binaural_stream.rs) |
| 双耳离线渲染 | OLA 源归约、LFE 旁路、spreader 分组/延迟环归约与尾部排空；原 C++ `BinauralSpreaderAdapter::push_batch/drain_ring` 逐样本施加输入增益并累加输出。 | S07；归约顺序固定，乘加与单纯加法分别审查。 | 三期核对为确定性运算；四期 4.7 B 迁入 Rust [`binaural_render`](../../rust/crates/mradm-render/src/binaural_render.rs)（spreader 适配器、OLA 与离线驱动），归约顺序不变 |
| 文件流式渲染的监听适配 | Monitor 折叠矩阵、实时覆盖增益、`clamp_frame` 秒到帧换算。 | N03/N18/S06；实时监听用途不改变其所属渲染流程。 | 三期（3.1、3.6 已迁移） |
| 离线后处理 | 2.3 后只剩文件读写、取消、进度、日志与临时文件替换；峰值转 dB、增益判定与逐样本乘法在 Rust。 | N13—N15/S01 已处理；保留各 API 已定义的行为。 | 二期（2.3 已迁移）；四期 4.8 起文件遍历、临时文件与安装在 Rust [`postprocess`](../../rust/crates/mradm-render/src/postprocess.rs)，C++ 只剩取消与进度的循环壳 |
| PCM 输出控制 | 写出块循环与临时文件替换。3.6 起秒到帧换算在 Rust `timeline`；WAVE 样本量化和序列化在 Rust；CAF float32 由 C++ 显式按小端序列化。 | S06 已处理；4.8 起写出端与 CAF 容器在 Rust（[`output`](../../rust/crates/mradm-render/src/output.rs)、[`mradm-wav::caf`](../../rust/crates/mradm-wav/src/caf.rs)），C++ 只剩薄包装与编码器。整数 PCM 继续拒绝 NaN。 | 三期（3.6 已完成）；四期 4.8 已迁移 |

## 已收敛的实现与 PCM 写出

| 链路 | 当前实现 | 本次判断 |
| --- | --- | --- |
| ADM 数字与时间解析 | [`document::number/samples`](../../rust/crates/mradm-adm/src/document.rs#L381)：Rust 十进制解析；时间使用 u128 纳秒与整数舍入。 | 不再依赖 C++ stream 或平台 long double；应与 N02 的后续数学转换分开。 |
| HpTF 文本解析 | [`parametric_eq::number`](../../rust/crates/mradm-dsp/src/hptf/parametric_eq.rs#L60)：固定 C locale 字节规则，拒绝十六进制和非零下溢。 | 前次发现的解析分歧已统一；N16 和 S02 已在一期 1.4 处理（设计改用 `libm`，尾音清理改为固定帧网格）。 |
| PCM 输入 | [`Reader::read_frames`](../../rust/crates/mradm-wav/src/reader.rs#L263)：显式小端整数转换与二的幂缩放，float32 按位读。Scene 直接接受调用方 f32。 | 相同合法字节和格式没有新增随机路径。PCM32 输入转 f32 的精度损失是确定的格式转换。通用 `FloatFlacReader` 仍用 dr_flac；它不是 ADM WAVE 主输入的替换实现，也不由 WAVE 门禁证明。 |
| 重采样 | [`Resampler`](../../rust/crates/mradm-dsp/src/resampler.rs)、vendored rubato 固定标量归约、[`mradm_math` 窗和 sinc](../../rust/vendor/rubato/src/windows.rs#L3)。 | 已处理原有运行时 SIMD 和平台三角函数差异；已有 `trig.*` / `resampler-*` 门禁。仍固定比率、质量、初始延迟、reset 和 EOS。 |
| Spreader 与 SVD | 固定 8 组预算、按索引归约、稳定源 seed；nalgebra `libm-force`；spreader 几何与滤波系数显式走 libm。 | 已处理二期的根因，不再把硬件线程数本身列作剩余随机混音源。见 [ADR 0017](../adr/0017-deterministic-spreader.md)。 |
| FFT 执行 | 锁定 RustFFT 标量规划与保持算术树的独立列优化。 | SIMD 分派问题已收敛；表生成另见 N09。 |
| 混音与随机性 | Rust PCM 混音按已准备的 row 顺序累加；几何凸包按插入顺序处理边；EAR MT19937 和 diffuse/spreader PCG seed 固定。 | 未发现把 HashMap 随机遍历或 `random_device` 用于这些 PCM 归约的现行路径。改变对象身份、布局或顺序属于输入改变。文件名随机数不进入 PCM。 |
| HRTF 缓存 | [`Filters` 查询缓存](../../rust/crates/mradm-dsp/src/hrtf_filters/cache.rs)、[`PreparedHrtfs`](../../rust/crates/mradm-dsp/src/live_binaural/prepared.rs)保留原有邻居和乘加顺序，缓存保存已计算结果。 | 缓存容量或命中本身未发现新的数值算法切换；需继续保持命中与未命中逐位回归。 |
| WAV float32 | [`Writer::write_frames`](../../rust/crates/mradm-wav/src/writer.rs#L136)直接 `sample.to_le_bytes()`。 | 有限样本位模式相同就写出相同样本字节。NaN payload 若在上游不同，writer 不会替上游统一它。 |
| WAV 整数 PCM | 同一 writer：f32 裁剪到 [-1,1]，转 f64 乘 32767 / 8388607 / 2147483647，再向零截断；NaN 拒绝。 | 无随机 dither，无平台 `lrint`。相同 f32 输入在默认浮点环境下有明确结果；不同上游样本在整数阈值附近可能相差 1 LSB，也可能量化成相同整数。 |
| CAF float32 | [`caf::Writer::write_frames`](../../rust/crates/mradm-wav/src/caf.rs#L106)：3.6 起把位模式显式按小端序列化（读取同样显式解析），文件声明 little-endian float32；4.8 B 起由 Rust 写出，字节不变。 | 不再依赖主机字节序；三平台输出字节不变，没有额外 DSP。`final-caf-*` 用例设为门禁。 |
| 后处理重写 | 增益改写、trim、降位（4.8 C 起在 Rust [`postprocess`](../../rust/crates/mradm-render/src/postprocess.rs)）使用同一 WAVE 读写层；增益是单独的 f32 乘法。 | 增益来源见 N15，帧边界见 S06。成功完成后的样本比较不应包含临时路径、metadata 或文件头差异。 |

## 本轮实施优先级

按维护者 2026-10-09 的要求，**Scene 相关工作明确列为本轮一期**。
这里的一期、二期、三期是本轮剩余数值实现的实施顺序，与已完成的 Rust 数值一致性历史分期分别记录。
本节为待实施计划；前面的源码审计与历史验收结果不因此变为已完成的迁移。

| 阶段 | 优先范围 | 完成目标 |
| --- | --- | --- |
| 一期 | Scene 流式渲染及其全部实际数值依赖，从 Scene 参数输入到应用侧输出 PCM。 | Scene 所用的数值计算集中在 Rust；共享数学函数、系数、输出 DSP 和状态规则纳入统一实现与三平台精确门禁。 |
| 二期 | 响度、True Peak 及其归一化反馈。 | 计量器、反馈增益与样本增益处理形成可重复的数值链路，兼顾计量准确性和最终 PCM 一致性。 |
| 三期 | 文件流式渲染、离线渲染及其余专用入口。 | 收走其余自有 C++ 数值运算，完成 EAR、HOA、Triple Balance 相应路径及最终 PCM 转换的覆盖。 |
| 四期 | PCM 写出为止的数据链路（2026-10-16 增补）。 | 导入投影、语义策略、全部自有后端、公共渲染层与 PCM 写出编排迁入 Rust，C++ 只留适配壳与调度；`room-binaural` exact 档进矩阵；S03、观察内核、N01 JSON 残留与覆盖缺口收尾。 |

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

**1.1 状态**：检查点与规则草案已提交（`7cc09ab`、`a7b6dd7`；规则见 [ADR 0018](../adr/0018-scene-numeric-replay-rules.md)，2026-10-14 接受）。
Consistency 运行 38034179850 在 A/B 两种配置下完成三平台诊断采集：

- 188 个 PCM id 全部逐位相同，197 项数值门禁无失败，诊断构建与普通构建输出相同（无扰动）。
- 按 Scene 用例目录及 epoch id 统计，每个平台对有 **18034 次 Scene 检查点比较**，三平台两两全部相同。
  原先按 `scene-` 名称前缀统计的 18461 混入了三个离线用例：`scene-binaural-cloud` 240 项、
  `scene-binaural-rotated` 136 项、`scene-hoa-extent` 51 项，共 427 项。
  当时记录包含输入、策略前后状态、渲染器命令与系数、切换前两路、渲染、重采样、过渡、HpTF 系数与设备侧输出。
  因此当前矩阵内 Scene 链路**没有观察到首个分歧**。N04、N05、N09—N11、N16、N17 的平台数学函数在这些输入上没有表现出差异，
  1.2 和 1.3 替换数学函数属于按 ADR 0018 主动收紧规则，不是修复已观察到的差异；替换后相对旧输出的位变化逐项记录。
- 仍有差异的检查点都不在 Scene 链路，且与二期基线已记录的观察项一致，最终 PCM 仍相同：
  - `ear-hoa`、`ear-5_1-hoa-input` 的 `ear.hoa-decode-row-*.f64`（三平台两两不同）；
  - 离线 `vbap-extent` 的 `vbap.03-gains.f32`（仅 Windows 与其他两平台不同）；
  - EAR 去相关 FIR 的 84 份 f64 频谱/逆变换内核检查点，以及不设门禁的 `fft-twiddles.10-libm.f64` 观察列（最大 1 ulp）。

  这些属于三期或观察列，不进入一期范围。

上述结果对应 `a7b6dd7` 的实际观测范围。同类型后端切换可能复用命令检查点名称，原收集器也未逐组强制所有配套字段流；
结果不能证明这些未完整记录的边界相同。修订将渲染器 current/outgoing/incoming 分开、为 lane 系数保留 epoch，
并校验每个已记录边界的配套文件和形状；新报告的 `scene_checkpoints` 汇总按 Scene 用例目录计算。
这些诊断修正的验证单独记录，不追溯改写运行 38034179850 的文件或数量。

2026-10-10 修订后的本机验证（macOS arm64）：

- Release 诊断构建通过全部 40 项工具测试，包含 74 个 Scene 配置的回放、逐组检查点校验，
  以及 SOFA→KEMAR 切换与元数据更新同时发生时保留两路命令的回归。
- 10 个定向配置、20 份 epoch PCM 在普通与诊断 Release 构建之间逐位一致；覆盖 VBAP 立体声/多声道/22.2、
  双耳、三份 SOFA、Triple Balance、变采样及设备 DSP，均无欠载。
- 独立 C++ 检查点测试在 Debug/Release 通过；clang-format、clang-tidy、cppcheck 和文档链接检查通过。

以上是本机修订验证；修订后的三平台检查点结果需由新的 Consistency 运行确认，不能沿用 38034179850 的记录。

**1.2 状态**：语义策略数值求值、Live VBAP 源增益合成和 Triple Balance 床系数已迁入 Rust（`8a73dc9`、`4d01d0e`、`1ad7641`）；
策略 dB 换算改用 `libm::powf`（`d476f1a`）；Scene 回放的策略更新加入 `gain_db`、extent、diffuse 和 divergence（`4bede24`）。
结构迁移在 Linux x64 上对普通与诊断运行逐文件位相同，`1ad7641` 的三平台 Consistency/CI/Quality 通过。
验证、性能与剩余项见 [Scene C++ 数值残留迁移](RUST_SCENE_RESIDUAL_MIGRATION.md)；回放变更后的三平台结果以新的 Consistency 运行为准。

**1.3 状态**：channelLock 与 divergence 阈值移入 Rust（`c1a9c51`）；Scene 空间数学、VBAP spread、HRTF 幅度、
live 双耳 divergence 与 SOFA 坐标转换改用 `libm`（`8d260b8`）；RustFFT 与纳入仓库的 RealFFT 的 twiddle 改用 `libm`（`e432729`）。
三平台 Consistency 38545398947 与 38548940341 通过，全部 160 个内核文件三平台相同，f64 twiddle 列设为门禁。
Scene 空间与卷积准备链路上不再调用平台数学库；HpTF 设计与峰值保护属于 1.4。见 [1.3 记录](RUST_SCENE_PORTABLE_MATH.md)。

**1.4 状态**：无峰值保护输出的音量乘法移入 Rust（`caf6859`）；HpTF 极小尾音清理改为自 reset 起每 1024 帧的固定网格，
淡化在第 2048 帧精确结束（`8ee8f82`，S02）；HpTF 设计与 `StereoPeakGuard` 释放系数改用 `libm`（`877a383`，N16、N17）。
Linux x64 上锁定矩阵的普通与诊断运行全部不变；Consistency 38570610669 中 188 个 PCM id、160 个内核文件和 44 个设备/HpTF 检查点三平台相同，
HpTF 设计内核设为门禁（共 155 份）。`SceneOutputSession` 中不再有决定样本值的 C++ 浮点运算。
见 [1.4 记录](RUST_SCENE_OUTPUT_DSP.md)。

**1.5 状态**：ADR 0018 §4、§5 的每条规则都已对应到实现与测试。HpTF 淡化期间发布的目标改为在当前淡化结束的那一帧开始，
设备输出不再随回调分块变化（`f94d28e`，S04）。逐阶段比较固定与碎片分块后：
- VBAP、Triple Balance、2 的整数次幂比率重采样、后端过渡和设备侧 DSP 与分块无关，
  对应的 11 个同采样率 VBAP/Triple Balance 回放用例在每个平台上要求两种分块逐位相同（`07030f5`）；
- 双耳卷积与分数比率重采样依赖分块（S03），继续作为分块配置分别门禁；
- 设备欠载补零不推进 HpTF 与峰值保护历史（S05）。

锁定矩阵输出不变；`07030f5` 的 Consistency 38598585257 中 188 个 PCM id 与 160 个内核文件三平台相同，12 个采集作业的分块无关检查全部通过。见 [1.5 记录](RUST_SCENE_TIMING.md)。

**1.6 状态**：一致性 config A 改用发布构建的选项（C ABI bundle，全部目标位置无关编译），`ReleaseParityTests` 防止两者漂移；
Windows 发布构建不再使用已安装依赖（`eedf2fe`）。设备侧 HpTF、峰值保护与音量新增变采样后接外部 SOFA 和 VBAP 立体声馈送的回放，
PCM 门禁增至 196 个（`d85bc79`）；设备回调由 C++ 分配计数测试约束，回调路径只用原子量（`ad4ef3c`、`0512770`）。
`0512770` 的 Consistency 38677291567 中 196 个 PCM id、160 个内核文件三平台相同，207 项门禁无失败；CI、Quality 与 Release Build 通过。
C++ 中剩余的 Scene 运算已登记，**一期结项**。见 [1.6 记录](RUST_SCENE_RELEASE_ACCEPTANCE.md)。

相关审计项：N04、N05、N09—N11、N16、N17，以及 N01/N03/S03—S07 中服务于 Scene 的部分和 S02。
一期对精确一致性的要求适用于锁定输入与配置的矩阵，并不把真实设备的墙钟操作或欠载输出当作相同事件序列。
数值内核的迁移与统一完成后，C++ 中若仍存在直接决定 Scene 样本值的运算，应登记原因和边界后才能结项。

## 二期 响度与 True Peak

当前 Scene 核心与 `SceneOutputSession` 没有通过 EBU R128 / True Peak 计量反馈调整 PCM。
因此 N13、N14、N15 和 S01 排在二期；一期中的 `StereoPeakGuard` 是样本峰值保护，仍按一期处理。
若实施时确认某项共享计量或反馈已进入 Scene 的数值链路，该部分随 Scene 前移到一期。

| 执行顺序 | 工作 | 具体范围与交付 |
| --- | --- | --- |
| 2.1 | True Peak 插值系数 | 统一插值系数生成和累加规则，保持现有滤波长度与过采样策略（N14）。 |
| 2.2 | 响度 | 统一 K-weighting、能量门控与 LUFS 换算，处理 ebur128 在 x86/ARM 上不同的极小状态清理（N13、S01）。 |
| 2.3 | 反馈增益 | 将离线后处理的反馈增益计算、dB/linear 转换和逐样本增益应用集中到 Rust；复用一期建立的公共数值规则（N15）。 |
| 2.4 | 验收 | 同时验收独立计量准确性与三平台逐位一致性，覆盖门限边界、跨采样峰值、长静音、分块、reset，检查归一化后的最终 PCM。 |

**2.1 状态**：ebur128 0.1.10 纳入 `rust/vendor/ebur128`，True Peak 插值滤波器的窗与 sinc 改用 `libm`；
内核探针新增 4 种采样率 × 2 种声道数的生产计量结果，True Peak 设为门禁，响度作为观察项留给 2.2。
Linux x64 上 glibc 与 `libm` 的系数逐位相同，锁定矩阵输出不变。`bcb6c3f` 的 Consistency 38718937064 中 196 个 PCM id 与全部 True Peak 门禁三平台相同；
响度观察项在 192 kHz 三平台两两不同（macOS 与 x86 最大 8.3e-11 LU，Linux 与 Windows 1 ulp），由 2.2 定位和统一。见 [2.1 记录](RUST_METER_TRUE_PEAK.md)。

**2.2 状态**：vendored ebur128 的 K-weighting `tan`/`pow`、门控系数 `pow` 与 LUFS `log10` 改用 `libm`；
x86 不再打开 FTZ，所有架构统一在每次调用后清零低于 `f64::EPSILON` 的滤波器状态（S01）。内核探针新增 K-weighting 系数，
末段改为让状态停在阈值以下的 2^-80 直流，计量文件全部设为门禁（188 个内核文件中 183 个）。Linux x64 上锁定矩阵的 PCM 不变；
`56f1ded` 的 Consistency 38742360010 中 196 个 PCM id 与全部 188 个内核文件三平台相同。修复前的检查点运行把 2.1 的分歧定位到
macOS 上 192 kHz K-weighting 分母的 1 ulp 差异（经接近 1 的极点放大），Linux 与 Windows 之间的 1 ulp 来自 `log10`，
S01 使末段读数在 macOS 与 x86 之间相差 120–168 LU。见 [2.2 记录](RUST_METER_LOUDNESS.md)。

**2.3 状态**：`mradm-dsp` 新增 `post_gain`：各后端的峰值转 dB、`RenderService` 的响度目标 / 峰值补偿 / 峰值上限 / 最终增益判定、
公开 `apply_loudness_norm` 与 `apply_peak_limit` 的增益，以及三处整段重写的逐样本乘法都移入 Rust，dB 换算使用 `libm`；
判定顺序、容差与 NaN / −0 的取值保持不变。内核探针新增 5 个 `post-gain` 文件并设为门禁（193 个内核文件中 188 个）。
Linux x64 上 196 个 PCM id 与 2.2 逐位相同；`d836744` 的 Consistency 38757362457、CI 38757366795 与 Quality 38757371551 通过，
196 个 PCM id 与全部 193 个内核文件三平台相同，209 项门禁无失败。见 [2.3 记录](RUST_POST_GAIN.md)。

**2.4 状态**：以测试内独立实现的 BS.1770-4 参考（标准表格的 48 kHz K-weighting、400 ms 块、绝对与相对门限）和 EBU Tech 3341
式合成信号验收计量：48 kHz 下 integrated、momentary、short-term 与参考相差不超过 3.2e-12 LU，门限两侧的取舍一致；
4 种采样率下与标称值最大偏差 0.041 LU；44.1/48/96 kHz 的 True Peak 偏差 0 至 +0.104 dB，192 kHz 按既有行为等于样本峰值。
验收发现 2.2 的极小状态清零发生在每次调用结束时，使静音段的 momentary 与 short-term 依赖分块；vendored ebur128 改为每完成一个
100 ms 块时清零，6 种分块与 reset 后的读数逐位相同，长静音后 integrated 与 True Peak 不变。内核探针新增 20 个 `meter-acceptance`
文件（213 个内核文件中 208 个设为门禁）；`mr_adm_normalization_acceptance_tests` 重新测量归一化后的最终 PCM，与目标相差不超过 1.6e-6。
`a347a32` 的 Consistency 38769696216 中 196 个 PCM id 与全部 213 个内核文件三平台相同，209 项门禁无失败；
`418986d` 的 CI 38773742902 与 Quality 38773737452 通过。N13—N15 与 S01 全部处理，**二期结项**。见 [2.4 记录](RUST_METER_ACCEPTANCE.md)。
合并审查随后修正了相对门限样例，并为 reset 增加全新实例基准与采集期断言；修订后的验证与上述三平台基线分开记录。
修订后 `0c3504b` 的 Consistency 38837774849、CI 38837774791 与 Quality 38837774975 通过，196 个 PCM id 与全部 213 个内核文件三平台相同。

保持既有声道权重、LFE 处理、完整积分历史、worker 归属和错误语义；声道映射修正或计量算法变更另行评估。
对 ebur128 使用可追踪的小范围补丁，不以更换语言或更新依赖版本代替数值规则和标准测试。

## 三期 文件流式渲染与离线渲染

按以下顺序处理一期和二期没有覆盖的路径，复用已收敛的共享实现：

1. **文件流式双耳与监听适配**：`BinauralStream` 的逐源加权混音、拓扑淡化、spreader 锥角，以及 Monitor 折叠矩阵。它们属于文件流式渲染或相应监听适配，不能因为用于实时监听就记成 Scene 一期任务。
2. **后端专用数值**：EAR 点源/extent/DirectSpeakers 与去相关 FIR、HOA 编解码、Triple Balance 非 D 模式的离线与文件流式运算。为 D 模式建立独立精确门禁；其控制与系数不能借用其他模式的验收结果。
3. **文件与命令参数入口**：ADM 增益与坐标投影、CLI 数字解析、专用 JSON/标签解析，以及尚未复用公共 Rust 数值入口的策略或覆盖项；为 S08 明确同起点块顺序，再随块时间线迁移落实。
4. **最终 PCM 与全链路验收**：验证 trim、增益重写、整数 PCM 量化、float 写出及声道排列。已明确的量化规则优先补边界覆盖，迁移剩余数值运算；文件生命周期与调度继续留在 C++。

按上述顺序分为以下切片。每个切片沿用一期、二期的做法：先保证锁定矩阵输出不变或逐项说明变化，
再以三平台 Release 结果设门禁；新增的文件流式与离线用例同样必须按精确 id 加入门禁。

| 执行顺序 | 工作 | 具体范围与交付 |
| --- | --- | --- |
| 3.1 | 文件流式双耳与监听适配 | `BinauralStream` 逐源加权归约与拓扑淡化、双耳 extent 锥角、Monitor 立体声折叠矩阵与实时覆盖增益的 dB 换算移入 Rust；一致性采集新增文件流式重放（实时增益、静音、拓扑、头部朝向与 seek，固定与碎片请求分块）并设为门禁（S07 双耳部分、N18、N03 实时增益）。 |
| 3.2 | EAR | 点源、extent、DirectSpeakers 几何与立体声 panner（N06），去相关随机相位（N08 剩余），EAR 增益乘积与发散累加；补 EAR 文件流式重放。 |
| 3.3 | HOA | HOA 编码系数与 EAR HOA 解码（N07），其中 `ear.hoa-decode-row-*` 诊断检查点已观察到三平台差异；HOA 实时覆盖与文件流式重放。 |
| 3.4 | Triple Balance | 非 D 模式离线与文件流式入口改用可移植数学函数（N12），D 模式建立独立精确门禁。 |
| 3.5 | 参数入口 | ADM 增益与坐标投影（N02），CLI 数字与标签解析（N01），尚未复用公共入口的策略与覆盖项（N03 剩余），明确同起点块顺序（S08）。 |
| 3.6 | 最终 PCM 与三期验收 | trim 与秒到帧换算（S06）、增益重写、整数量化、float 写出与声道排列的边界覆盖；三平台 Release 验收与结项。 |

**3.1 状态**：`BinauralStream` 的逐源增益求和与拓扑淡化、双耳 extent 锥角改由 Rust `stream_dsp` 分别舍入计算，
Monitor 立体声折叠矩阵改用 `libm::sinf/cosf`，实时覆盖增益的 dB 换算（EAR、VBAP、Triple Balance、HOA、双耳与两处溢出检查）
统一为 `libm::powf`。一致性采集新增 `mr_adm_phase2_stream` 与 6 个双耳文件流式重放（实时增益、静音、拓扑、头部朝向与 seek，
固定与碎片请求分块），PCM 门禁增至 202 个；内核探针新增 4 个 `stream-dsp` 文件（217 个内核文件中 212 个设为门禁）。
Linux x64 上原有 196 个 PCM id 不变；`b20a365` 的 Consistency 38844929927、CI 38844932172 与 Quality 38844933977 通过，
202 个 PCM id 与全部 217 个内核文件三平台相同，216 项门禁无失败。合并审查补上的用例版本 2（独立帧数校验）
输出与版本 1 逐位相同，`e7accaa` 的 Consistency 38857339402 重新验收通过。见 [3.1 记录](RUST_FILE_STREAM_BINAURAL.md)。

**3.2 状态**：`mradm-ear` 的几何、extent 权重、立体声 panner 与去相关随机相位改用 `libm`（N06、N08），EAR 在 C++ 中剩下的增益运算
经核对只有单独的加法与乘法。内核探针新增 3 个 `ear-gains` 文件，文件流式重放经共享渲染器工厂新增 6 个 EAR 用例（含按扬声器标签的覆盖），
PCM 门禁增至 208 个，内核文件 220 个中 215 个设为门禁。Linux x64 上只有 3 个使用 extent 的 EAR 离线用例在最后几位改变；
`c931750` 的 Consistency 38865168292、CI 38865171453 与 Quality 38865174302 通过，208 个 PCM id 与 220 个内核文件三平台相同，
223 项门禁无失败。见 [3.2 记录](RUST_EAR_PORTABLE_MATH.md)。

**3.3 状态**：HOA 编码与 EAR HOA 解码改用 `libm`（N07）；AllRAD 解码器原先经 nalgebra 动态矩阵调用 matrixmultiply，二阶及以上输入在多于
5 个扬声器的布局上随主机选择 AVX/FMA 或 NEON 内核，现改为与 nalgebra 标量循环同序的固定乘积，一阶解码器逐位不变。HOA 文件流式的
实时覆盖改用共用的按声道解析。新增三阶 HOA 夹具、`ear-hoa3` / `hoa-loudness-minus23` 离线用例、6 个 HOA 文件流式重放与 4 个 HOA 内核文件，
PCM 门禁增至 216 个，内核文件 224 个中 219 个设为门禁。Linux x64 上只有 2 个 HOA extent 用例在最后几位改变；
`5c00c47` 的 Consistency 38868898890、CI 38868901777 与 Quality 38868905213 通过，216 个 PCM id 与 224 个内核文件三平台相同，
233 项门禁无失败，诊断检查点全部相同。见 [3.3 记录](RUST_HOA_PORTABLE_MATH.md)。

**3.4 状态**：去掉 Triple Balance 的 `PORTABLE` 泛型，标准模式的离线、文件流式与 Scene 入口共用同一份 `libm` 实现与 size 积分格点，
平滑常量改用 `libm::expf`（N12）；D 模式自引入起即用 `libm`，本切片为它建立独立门禁。新增 `objects-room` 夹具、6 个 size / D 模式离线用例、
6 个 Triple Balance 文件流式重放与 4 个 `triple-balance` 内核文件，PCM 门禁增至 228 个，内核文件 228 个中 223 个设为门禁。
Linux x64 上原有 216 个 PCM id 不变；`e0ef382` 的 Consistency 38874020588、CI 38874023487 与 Quality 38874026573 通过，
228 个 PCM id 与 228 个内核文件三平台相同，246 项门禁无失败，诊断检查点全部相同。见 [3.4 记录](RUST_TRIPLE_BALANCE_PORTABLE_MATH.md)。

**3.5 状态**：ADM 导入的 dB 增益与 Cartesian DirectSpeakers 投影改用 `libm`（N02），各 Rust crate 的生产代码不再调用平台初等函数；
CLI 浮点选项与 HOA 标签改用 Rust 正确舍入、与 locale 无关的十进制解析，标签 / UID 规范化只看 ASCII（N01）；核对后 N03 不再有需要迁移的
增益入口；同起点块在各后端建立增益、总线或路由前只保留 ADM 文档顺序中的最后一块（S08，在 libstdc++ 上复现了旧实现的错误生效块；
合并后又修正了双耳取到同起点第一块、EAR / VBAP / HOA 从被覆盖块插值、Apple LFE 总线与 22.2 LFE 路由残留被覆盖块的问题）。新增 `params` 夹具、
5 个 `params-*` 离线用例与 3 个 `adm-import` / `decimal` 内核文件，PCM 门禁增至 233 个，内核文件 231 个中 226 个设为门禁。
Linux x64 上原有 228 个 PCM id 不变；`75e38ae` 的 Consistency 38879296864、CI 38879300366 与 Quality 38879303103 通过，
233 个 PCM id 与 231 个内核文件三平台相同，253 项门禁无失败，诊断检查点全部相同；同起点块修正后，`c89e81a` 的 Consistency 38907550214
与 CI 38907555184 再次确认 233 个 PCM id（含新的 `params-*` 结果）与 231 个内核文件三平台相同。见 [3.5 记录](RUST_PARAMETER_ENTRY.md)。

**3.6 状态**：输出裁剪、预览窗口与 Monitor 定位 / 循环共用 Rust `timeline::seconds_to_frame`（最近帧，半帧远离零），Monitor 由截断改为
最近帧（S06）；CAF float32 显式按小端序列化。增益重写与整数量化原已在 Rust 且确定，本切片补边界覆盖：新增 9 个 `final-*` 离线用例
（16 / 24 位整数 WAVE、满幅钳位、7.1.4 置换与 ADM BW64、float32 CAF、半帧边界裁剪）与 4 个 `timeline` / `wav-quantize` 内核文件，
PCM 门禁增至 242 个，内核文件 235 个中 230 个设为门禁。Linux x64 上原有 233 个 PCM id 与 231 个内核文件不变；合并审查补上非法裁剪时间的校验与 CAF 诊断
检查点后，`4ea3665` 的 Consistency 38937703621、CI 38937703580 与 Quality 38937703595 通过，242 个 PCM id 与 235 个内核文件三平台相同，
264 项门禁无失败，诊断检查点全部相同。见 [3.6 记录](RUST_FINAL_PCM.md)。

**三期结项**：六个切片完成，N01—N18 全部处理，S01、S02、S06、S08 已处理；S03 保留为分块条件（文件流式对象平滑依赖请求分块，无门禁），
S04、S05 保留为 Monitor 的运行时调度条件，S07 保留为构建与宿主浮点环境条件。各项依据与门禁规模见 [3.6 记录](RUST_FINAL_PCM.md#三期结项)。

相关审计项：N02、N06—N08、N12、N18、S08，以及 N01/N03/S03—S07 的其余部分。
一期和二期完成的共享实现继续受门禁保护，三期新增路径不能重新引入平台数学函数或不同乘加规则。

若 S08 在当前生产输入上复现出实际错误，可独立提前修复并保留回归；尚未复现时按三期计划推进，
不把猜测的跨平台块顺序差异写成已测事实。

各阶段都保存候选的有效参数位模式、系数、适用的反馈增益、最终 float PCM 和整数 PCM，并将数值分歧与事件/帧数分歧分开定位。
跨平台精确比较的对象是同一候选版本；历史生产实现与独立参考用于准确性和行为对照。预期数值变化需要记录依据和误差，不能直接刷新期望值或放宽门禁来代替验收。
每一批新增精确门禁均附对应的三平台 Release 结果。

## 四期 PCM 写出为止的数据链路迁入 Rust

四期按 [ADR 0021](../adr/0021-rust-primary-project.md) 推进项目逆转为 Rust 主导，是这条路线的第一期。范围截止到 PCM 写出，与本文“比较范围”相同：
- 包含 WAV float32 / 整数 PCM 与 CAF float32 的样本写出，以及文件流式渲染、Scene 流式渲染交给设备的应用侧 PCM；
- FLAC、Opus、APAC、IAMF 编码与 Apple AUSpatialMixer 后端继续排除；
- Linux arm64 不进矩阵。

主线有三条：
1. 把 PCM 写出为止的数据处理整体迁入 Rust，C++ 只保留适配壳与调度。
2. `room-binaural` 的 exact 档进统一矩阵；realtime 档使用原生 SIMD FFT，没有跨平台契约，不纳入。
3. 收尾保留条件与覆盖缺口：S03、5 份观察内核、N01 的 JSON 残留、策略矩阵与后置增益。

`SceneStreamEngine`、`RenderService`、Monitor、C ABI 与 CLI 在四期保留在 C++。它们是一致性工具的驱动面，见[逆转路线](RUST_INVERSION_ROADMAP.md#oracle-的驱动面)。

四期的切片遵守 ADR 0021 的迁移纪律：
- 结构迁移逐位不变：锁定矩阵的 PCM id、内核文件与既有诊断检查点逐位相同，迁移后的代码继续发出同名检查点；
- 先补覆盖，再迁移它保护的路径；
- 有意的数值变化单独成片；
- 迁出即删除，生产 C/C++ 由 `scripts/quality/language-surface.py --check` 设棘轮，每个切片报告变化。
- 三平台验收集中进行（维护者 2026-10-17 决定）：每个切片以 Linux x64 本机的基线 / 候选逐字节对照为完成条件，不逐片等待三平台 Consistency（每次约半小时）；在新增门禁的切片累积后或 4.10 统一触发一次，结果回填到所覆盖的各切片记录，出现分歧时按 `first_observed` 定位到切片。

| 执行顺序 | 工作 | 具体范围与交付 |
| --- | --- | --- |
| 4.1 | 方向与基线 | ADR 0021、[逆转路线](RUST_INVERSION_ROADMAP.md)、本节与上文“C++ 剩余执行点”的四期归属；C/C++ 规模指标与棘轮；Rust 侧生产路径检查点写出器；离线渲染在导入后与语义策略后的场景检查点。PCM 与内核输出不变。 |
| 4.2 | 覆盖先行 | ① `room-binaural` exact 档：提交并钉住非个人的合成房间包（外部 HRIR 用已钉住的 `simple.sofa`），增加离线、文件流式（头姿、seek）、Scene 用例与 `room.*` 内核探针；换房是异步的，没有固定的开始帧，未纳入。② 策略矩阵：`objects[]` 身份规则、`channel_lock`、`direct_speakers` 过滤、`lock_*` 与 DirectSpeakers 矩阵 JSON 的离线与 Scene 用例。③ VBAP / HOA / 双耳的离线对象平滑，VBAP 与 Triple Balance 的自动响度与峰值。④ 5 份观察内核设为门禁。 |
| 4.3 | Rust 渲染场景：导入投影与声道床 | 新 crate `mradm-scene` 持有渲染场景：WAVE 格式读取、CHNA 与 UID 规范化、`src/adm_metadata` 的导入复制、声道床预设 / 目录 / 标签解析 / 场景合成迁入，扬声器布局表（含 Apple 几何，原列 4.5）一并迁入。C++ `AdmScene` 改为由 Rust 场景视图生成，`05-import` 场景检查点改由 Rust 场景发出。场景检查点与 PCM 逐位相同。`RenderPlan` 的 Rust 场景句柄推迟到 4.4（维护者 2026-10-17 决定）。 |
| 4.4 | 语义策略与参数 JSON | 语义策略的解析、匹配、合并、应用、报告与模板，以及 DirectSpeakers 矩阵与屏幕 JSON 迁入 Rust，N01 的 JSON 残留随之消除。`SceneStreamEngine` 的逐更新应用改调 Rust。`RenderPlan` 携带策略后的 Rust 场景句柄，`06-policy` 检查点改由 Rust 发出。报告与模板 JSON 保持逐字节相同。 |
| 4.5 | 扬声器后端与公共渲染层 | EAR、VBAP（含 Scene 胶水）、HOA，DirectSpeakers 路由与矩阵、标签别名、LFE / 22.2 路由与 `speaker_pcm` 离线驱动迁入 Rust。渲染循环由 Rust 驱动，进度、日志、取消与 PCM sink 经 C 回调接口，计量顺序不变。 |
| 4.6 | Triple Balance 与 room | Triple Balance 的场景改写、床、文件流式与 Scene 胶水，room 的准备、准备线程状态机与 Scene 胶水迁入 Rust。只被冻结参考使用的 C++（`panner` / `size_*`）连同参考冻结进 `tests/reference/`。 |
| 4.7 | 双耳 | 计划构建（channelLock 参考对、cloud、divergence×extent 槽、种子、ADR 0017 分组）、OLA 源归约、LFE 旁路、spreader 适配器 / 延迟环 / 排空、工作池、HRTF 网格缓存与 Scene 胶水迁入 Rust，保持源索引归约顺序。 |
| 4.8 | PCM 写出与后处理 IO | WAV / CAF 写出编排、增益重写、trim、降位、容器元数据与 peak / loudness 公共 API 迁入 Rust；编码器沿用同一 sink 接口，留在 C 岛（4.8 解释为编码器继续经 Rust reader 读取已写完的 float WAVE，不另建 C++ sink）；删除失去调用者的私有 FFI 头。 |
| 4.9 | S03 对象平滑规则 | 有意的数值变化：文件流式对象平滑改为按绝对样本网格的内部块插值，与离线渲染一致、与请求分块无关；增加开平滑的文件流式重放（固定与碎片分块逐位相同）并设门禁。离线平滑输出不变。 |
| 4.10 | 三平台验收与结项 | 全矩阵三平台 Release 验收，记录 C/C++ 规模变化，准备五期。 |

排序依据：
- 4.2 先补 4.3–4.8 要保护的路径。
- 渲染场景先于后端：迁入 Rust 的后端直接读取 Rust 场景，不需要临时的 C++→Rust 场景编组。
- S03 放在后端迁移之后：届时它只改 Rust 代码，也不影响既有门禁。
- 双耳卷积与分数比率重采样的分块依赖仍按分块配置分别门禁；S04、S05 的运行时条件不变。
- 4.5–4.7 删除被冻结参考引用的生产 C++ 时，把旧实现连同参考冻结进 `tests/reference/` 并登记哈希（维护者 2026-10-16 决定），见[逆转路线](RUST_INVERSION_ROADMAP.md#c-删除与冻结参考)。

**4.1 状态**：ADR 0021、逆转路线、C/C++ 规模棘轮（生产 45,090 行、岛 3,213 行起算）、Rust 生产路径检查点写出器与离线场景检查点已提交。Linux x64 本机上，候选的 242 个 PCM id、全部内核文件与既有 26,358 个诊断检查点都与 `0a3b362` 逐字节相同，新增 124 个场景检查点；三平台 Consistency 38990075874（`7950cfb`）在 config A、B 上 242 个 PCM id 相同、门禁失败 0，诊断报告中场景检查点与全部既有检查点三平台相同。见 [4.1 记录](RUST_INVERSION_BASELINE.md)。

**4.2 状态**：覆盖用例、门禁与一致性工具扩展已合入，不改既有 PCM。Linux x64 本机上既有 242 个 PCM id、235 份内核测量与全部既有诊断检查点都与 4.1 候选逐字节相同，5 个策略用例的 `06-policy` 场景检查点与 `05-import` 不同。三平台 Consistency 39012071641（`45ab448`）在 config A、B 上 283 个 PCM id 相同、308 条门禁失败 0，诊断报告中 237 份内核、201 个内核检查点、31,100 条用例检查点与 21,380 项 Scene 检查点比较三平台相同。见 [4.2 记录](RUST_INVERSION_COVERAGE.md)。

**4.3 状态**：扬声器布局表、输入导入（WAVE、CHNA 映射、ADM 投影调用）与声道床迁入 `mradm-scene`，`05-import` 场景检查点改由 Rust 写出，`RenderPlan` 的 Rust 场景句柄推迟到 4.4。Linux x64 本机上 283 个 PCM id、237 份内核测量、201 个内核检查点与 30,772 个诊断检查点都与 `main`（`f277b64`）逐字节相同；生产 C/C++ 由 45,026 行降到 44,632 行。三平台 Consistency 39022845303（`3e7a806`）在 config A、B 上 283 个 PCM id 相同、门禁失败 0，诊断报告中 237 份内核、201 个内核检查点、31,100 条用例检查点与 21,380 项 Scene 检查点比较三平台相同。见 [4.3 记录](RUST_INVERSION_SCENE.md)。

**4.4 状态**：语义策略（解析、匹配、合并、应用、报告与模板）、Scene 流式的逐更新应用、导出回写，以及 DirectSpeakers 矩阵与屏幕配置的 JSON 迁入 `mradm-scene`，JSON 读写改用新 crate `mradm-json`，N01 的 JSON 残留随之消除。`06-policy` 由 Rust 写出，`RenderPlan` 携带策略后的 Rust 场景句柄。报告、模板、导出与错误消息与 C++ 逐字节相同，只有维护者同意的几项有意变化：语法错误正文、两处报告修正、LF、身份冲突的报错顺序，以及 extent 缺省上限改为 ADM 取值范围（唯一改变 PCM 的一项）。黄金数据分别来自 nlohmann 与删除前的 C++：JSON 格式化与解析语料、714 例策略、742 条导出、958 条配置。生产 C/C++ 由 44,632 行降到 42,406 行。三平台 Consistency 39031059419（`294415d`）、39824671399（`8fff2a8`）与 39891455786（`cdc23c8`）在 config A、B 的普通与诊断报告中都是 283 个 PCM id 相同、门禁失败 0。见 [4.4 记录](RUST_INVERSION_POLICY.md)。

**4.5 状态**：扬声器后端（VBAP、HOA、EAR）、Scene 流式的 VBAP 渲染器，以及它们共用的 DirectSpeakers / 标签 / LFE 路由、对象块预处理、离线驱动与文件流式渲染迁入新 crate `mradm-render`，Rust 后端只读策略后的 Rust 场景句柄；C++ 只剩适配壳、能力表、离线驱动的写出 / 取消 / 进度，以及给 4.6、4.7 后端用的薄包装。EAR 的 channelLock / divergence 预处理与 22.2 LFE 策略随之迁入。`speaker/s*`、`vbap.0*`、`hoa.0*`、`ear.*` 与 `.30-vbap*` 检查点改由 Rust 写出。路由规则以删除前 C++ 生成的 4,550 条黄金数据验收；各批的新旧 CLI / Scene 对照（saf 1,148 例、hoa 886 例、ear 2,096 例、88 个 Scene 重放）全部逐字节相同。生产 C/C++ 由 42,406 行降到 39,747 行。三平台 Consistency 39933972829（`16e14dc`）、39959813760（`08350d9`）、39987342040（`dd3f82b`）与 40004502957（`763d637`）在 config A、B 的普通与诊断报告中都是 283 个 PCM id 相同、门禁失败 0。见 [4.5 记录](RUST_INVERSION_SPEAKERS.md)。

**4.6 状态**：Triple Balance 后端（场景规范化、床、报告、离线 TB 内核、文件流式与 Scene 流式渲染器）和 room 后端（资源与 HRIR 加载、事件表、准备线程状态机、文件流式与 Scene 流式渲染器）迁入 `mradm-render` 与 `mradm-dsp` `room::loading`。C++ 只剩适配壳、能力表、`render_window` 的写出 / 计量 / 取消，以及 `IRoomControl` 薄壳；只剩测试使用的 Triple Balance C++ 移到 `tests/support/triple_balance`。`.30-triple-*` 检查点改由 Rust 写出，room 原有的浮点环境边界由 FFI 作用域保留。Triple Balance 报告以删除前 C++ 生成的 1,300 例黄金数据验收；各批新旧对照（Triple Balance 离线 992 例、文件流式 650 例，room CLI 1,014 例，88 个 Scene 重放的普通与诊断构建）全部逐字节相同。行为差别只有 Scene Triple Balance 警告的顺序；room 离线默认 ramp 的 `×48` 照原样迁移，记为已知问题。生产 C/C++ 由 39,747 行降到 36,931 行。三平台 Consistency 40099269643（`6940df9`）、40122405263（`a072146`）、40166924313（`305159f`）、40176869018（`72c188b`）与 40186174718（`ea07aa9`）在 config A、B 的普通与诊断报告中都是 283 个 PCM id 相同、门禁失败 0。见 [4.6 记录](RUST_INVERSION_TRIPLE_ROOM.md)。

**4.7 B 本机对照（2026-10-27）**：`c967d840` 与 `1c52d999` 在无诊断插桩 Release 下，macOS arm64 的 83 例、Windows x64 的 77 例全部与基线逐位相同，77 个共用候选输出跨平台也相同。Windows 复测仍有 cloud 离线端到端耗时 +10.02%、带拓扑编辑的文件流式渲染峰值内存 +21.04% 两项开放回退。维护者要求记录问题后合并；性能验收尚未完成，本轮未继续 macOS 性能矩阵或长测。范围、隔离观察和原始轮次见 [4.7 记录的 B 合并审查](RUST_INVERSION_BINAURAL.md#b-合并审查2026-10-27)。

**4.7 B 性能续查（同日）**：原端到端计时受约 15.55 ms 的退出轮询粒度影响；直接等待进程退出的两轮无插桩 cloud 对照为 +3.21% / +1.39%，不删除原始历史记录。拓扑重建的新旧卷积 bank 重叠解释了内存回退，提前释放的归因实验将峰值从 52.11 MiB 降到 43.28 MiB（参考 43.15 MiB）。内部工作池提交与输出归约有已测优化方向；未合入生产修复，问题仍开放。见[性能调查](RUST_BINAURAL_PERFORMANCE_REVIEW.md)。

**4.7 B 内存修复（2026-10-28）**：文件流式拓扑重建复用 bank、工作池及卷积工作区，增长时只补足差额。完整控制重放的峰值在 Windows 由 52.31 降至 43.02 MiB，macOS 直接 Rust 对照由 58.30 降至 48.45 MiB；静态对照未见本次改动新增开销。Windows 77 项、macOS 83 项 PCM 对照和 77 个共用候选输出全部逐位一致。BINAURAL-002 已修复，其他耗时问题仍开放。见[内存修复](RUST_BINAURAL_MEMORY_FIX.md)。

**4.7 状态**：双耳后端的 HRTF 状态与缓存（A）、声源计划、逐源 OLA、spreader 适配器、工作池与离线驱动（B）、文件流式渲染（C）和 Scene 流式渲染器（D）迁入 `mradm-render`（`binaural_hrtf`、`binaural`、`binaural_render`、`binaural_stream`、`live_binaural`），源索引与组索引的归约顺序不变，线程数不影响输出。C++ 只剩适配壳、能力表、`HrtfState` 视图、写出 / 取消 / 进度与创建函数；只剩测试与冻结参考使用的 C++ 移到 `tests/support/binaural` 与 `tests/support/head_rotation.h`。`binaural.01–05`、`hrtf-grid-*`、`cloud.*`、`spreader/*` 与 `.30-binaural-*` 检查点改由 Rust 写出。声源计划以删除前 C++ 生成的 2,511 例黄金数据与 54 个 CLI 渲染指纹验收；各批新旧对照（离线 CLI 948 例、文件流式 574 例、88 个 Scene 重放的普通与诊断构建、矩阵子集的普通与诊断构建）中，成功渲染用例的 PCM 逐位相同。离线文件比较时将 bext 的墙钟日期时间置零；逐字节对照仍有三类例外：A 中 1 例两版均中止但 stderr 不同，B 中 10 例由中止改为报错，D 中 1 个 VBAP 设备回放的 24 个设备回调 trace 切分不同（按样本顺序拼接后 36 条流全部相同）。行为差别：线程数取法、worker 中止改为返回错误、文件流式检查输出缓冲、Scene 双耳 session 创建失败改为返回错误。生产 C/C++ 由 36,931 行降到 33,732 行。三平台 Consistency 40300248787（`c967d84`）、40484741776（`c34f937`）、40653088266（`0f49c1b`）、40808829856（`f6c697e`）与 40931990370（`31bb6dc`）在 config A、B 的普通与诊断报告中都是 283 个 PCM id 相同、门禁失败 0。BINAURAL-001（Windows cloud 离线端到端耗时）仍开放。见 [4.7 记录](RUST_INVERSION_BINAURAL.md)。

**4.8 状态**：没有生产调用者的 peak / loudness 公共 API 删除（A）；CAF 容器、最终 float 写出端、进程级 writer 编号与 `writer/pass*` 检查点迁入 `mradm-wav::caf` 与 `mradm-render::output`（B）；增益改写、trim、整数降位、WAV 布局收尾与 AXML 替换，连同 sidecar 命名、安装与备份恢复，迁入 `mradm-render::postprocess`（C）。C++ 只剩 `audio_io.h` 的薄包装、检查取消与发进度的分步循环壳 `run_output_pass`，以及留在 C 岛的编码器（FLAC、Opus MKA、IAMF、APAC 与 MP4 udta）。错误码、文字、context、进度与取消点、sidecar 命名与 writer 编号顺序不变。维护者决定的四处修正：写出端报告 `finish()` 失败、改写保留源 CAF 的布局标签、`.caf` 拒绝 i16 / i24、`trim_file_smoke_test` 的断言；另有增益与 trim 不再接受 FLAC 输入。CAF 以删除前 C++ 生成的 376 例 `caf-v1` 黄金数据验收；新旧对照中 B 的 460 例 CLI 0 差异，C 的 CLI 460 例与直接 API 373 个操作只在上述有意变化处不同，矩阵子集普通与诊断 0 个问题。逐字节对照的例外：bext 与 CAF `info` 的日期置零、上述有意变化，以及 Windows 上的安装语义、OS 错误文字与路径编码。生产 C/C++ 由 33,718 行降到 32,551 行。三平台 Consistency 41037527666（`4207627`）与 41074793418（`e9a9541`）在 config A、B 的普通与诊断报告中都是 283 个 PCM id 相同、门禁失败 0；C 的 41152970680（`8278a4b`）待回填。见 [4.8 记录](RUST_INVERSION_OUTPUT.md)。

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
