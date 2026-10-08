# Triple Balance 渲染器

Triple Balance 是独立于 SAF VBAP 的房间坐标渲染后端。
本次拆分保留既有点源、尺寸、bed 和 WAV 输出行为；旧 `room-compat` 名称及
`--speaker-panner` 入口不保留别名。原有冻结 fixture 与历史研究证据目录保持原名。

## 使用

```sh
./build/release/mradm render -i input.wav -o output.wav \
  --renderer triple-balance --output-layout 9.1.6
./build/release/mradm layouts --format wav --renderer triple-balance
```

C++ 使用 `RendererSelection::triple_balance` 或 `create_triple_balance_renderer()`；
C ABI v1.43 使用 `ADM_RENDERER_TRIPLE_BALANCE = 7`。
GUI 批量渲染通过相同的能力查询和枚举显示 Triple Balance。`auto` 的原有选择策略不变。

## 支持范围

- 输出 7.1.4、9.1.6，以及项目自有的实验性 22.2 扩展。
- ADM Cartesian Objects；标准单个 7.1.2 DirectSpeakers bed 采用固定参考路由。
- 文件元数据允许同一 512 帧控制块内多次更新；时间非递减，同采样点按文件顺序处理。
  每块消费其中全部事件，以最后一条的位置／尺寸作为本块控制目标；首块也采用此规则。
  控制网格、平滑和块内增益插值保持原样，语义报告以 `control_target` 标明每块选中的事件。
- 非零尺寸限已验证的 48 kHz、等尺寸 `width=height=depth`、size ∈ [0,1]。
  22.2 全路径要求 48 kHz。对象来源语义与时间限制仍按原有校验执行。
- `--speaker-spread-mode auto` 使用尺寸内核，`none` 明确忽略尺寸。
- 固定房间几何；拒绝 Apple 几何、MDAP、额外平滑、自定义 bed 路由、HOA、
  positionOffset、channelLock、divergence、screenRef、headLocked 和其他未验证组合。
- 支持离线窗口渲染及重复使用 prepared metadata。有状态尺寸分支从第 0 帧预热，
  窗口之外的音频不写出、不进入该窗口计量。实时流通过同一块处理内核输出 PCM。
- 保留 speaker re-render WAV profile：7.1.4 按 WAVE mask 重排；9.1.6 保留原生声道序、
  WAVEFORMATEXTENSIBLE mask 0，不写 ADM；需要显式 CoreAudio 9.1.6 布局时使用 CAF。

详细参考行为与数值验证见 [等尺寸内核](DAR_NATIVE_SIZE_ALIGNMENT.md)、
[对象语义](DAR_OBJECT_SEMANTICS.md)、[bed 语义](DAR_BED_SEMANTICS.md)、
[22.2 扩展](ROOM_222_EXTENSION.md)。更名不扩大这些证据的适用范围。

## 实时监听与编辑

`open_stream()` / 既有 `adm_monitor_*` 支持播放、暂停、循环、后端切换与精确跳转。
GUI 在“系统空间音频”的渲染床后端中提供 Triple Balance，布局为后端与平台设备能力的交集。
传输条的循环按钮重复整段素材；C API 还可设置任意帧区间。
`7.1.4`、`9.1.6`、实验性 `22.2` 沿用上面的输入限制。扬声器输出的头部空间化由系统完成。

- 流按 1024 帧渲染、512 帧更新控制目标，以 FIFO 适配任意请求长度；末块仅输出有效帧。
  同块多事件一次消费完毕，不把剩余事件积压到后续控制块。
  无实时过渡的输出及固定覆盖后的跳转与对应离线浮点 PCM 一致，监听输出级的淡化／系统空间化另计。
- 对象及 bed 声道的实时增益使用 20 ms 斜坡，作用在各对象干声与尺寸混合之后；静音不重置滤波器。
- 等尺寸编辑复用 `extent_scale × extent_{width,height,depth}_scale`，三轴有效倍率必须相同。
  源尺寸乘倍率并限幅到 [0,1] 后进入现有平滑与量化；源 size=0 保持点源。
  过零的点源／尺寸路径转换以一个控制块淡化，已有非等轴编辑不会被自动改写。
- 未支持的覆盖整批拒绝，C API 返回错误，GUI 保留上次有效声音并显示原因；失败 revision 不被确认。
  diffuse、divergence、headLocked=true 及 bed 尺寸覆盖不在实时编辑范围内。
  带 diffuse 的尺寸对象被缩成纯 diffuse 点源时仍按原有输入校验拒绝。
- 尺寸跳转使用最多 32 MiB 的会话内 DSP 快照，约每秒保存一次，LRU 淘汰并优先保留最近跳转／循环起点。
  不保存整轨 PCM、不写磁盘缓存；快照只含固定大小滤波／控制状态，不复制事件表。尺寸对象的点源备用路径逐块推进，不额外展开全曲增益时间线。
  倍率变化清除旧快照；增益和静音不使其失效。连续编辑的历史状态不作为静态倍率的离线参考快照。
- 没有有效快照时从第 0 帧预热。预热在控制锁外执行，可被更新的跳转或停止取消。
  后端热切换先应用编辑再后台预热，旧流继续输出，新流追齐后交叉淡化。

`mr_adm_triple_balance_stream_tests` 覆盖逐样本参考、倍率 policy、过零、静音恢复、缓存淘汰及非对齐跳转。
Release 的 `mr_adm_triple_balance_monitor_benchmark [9.1.6|9+10+3]` 使用 13 路合成 PCM
（7.1.2 bed、两个尺寸对象和一个点源）经 null 音频设备按实际时钟循环监听 60 秒，输出 RTF、
首次／缓存跳转耗时、underrun 和 macOS 内存峰值。它不代替物理设备或高对象数素材的验收。

## Scene 流（C ABI v1.44）

`adm_create_scene_stream` 和 `adm_scene_stream_switch_backend` 现在接受
`ADM_RENDERER_TRIPLE_BALANCE`。输出布局为 `7.1.4`（`4+7+0`）、`9.1.6`、
`22.2`（`9+10+3`），PCM 顺序与对应 renderer 的内部布局一致。既有 `adm_scene_output_*`
可绑定 null 或平台支持的系统空间音频设备；设备支持范围仍由平台决定。

Scene 接收 canonical 有效状态，不经过文件输入的原生 ADM gain/mute/time 忽略规则。
`linear_gain` 和 `active` 在对象干声／尺寸混合后生效，静音继续推进滤波历史。
位置、等尺寸和音量各自维护 ramp；事件在 `offset_samples` 开始，显式 ramp 优先于
`object_smoothing_frames`，jump 按 Scene 契约立即更新。更新一个字段不延长其它字段的
ramp。多次更新可以位于同一控制块甚至同一采样点，按提交顺序处理。

此路径不使用文件渲染的 512 帧事件取整及位置／尺寸平滑。两者复用空间算法和滤波系数，
动态输出不承诺与文件渲染逐样本相同。四路滤波逐样本推进，保留内部延迟但无额外分块延迟；
连续 512 个低于既有阈值的输入采样触发静音退出，最后 32 个采样淡出。尺寸归零后重置
尺寸滤波历史；纯点源不引入尺寸滤波。generation/epoch 重建清空历史，EOS 截在媒体终点。

- 非零尺寸、bed、22.2 要求 48 kHz 输入；7.1.4/9.1.6 纯点源可使用 Scene 的 8–192 kHz
  输入范围。输出采样率由既有 Scene 重采样路径处理。
- 7.1.4/9.1.6 坐标 X/Y 为 [-1,1]、Z 为 [0,1]；22.2 的 Z 为 [-1,1]。
  尺寸要求三轴相等且在 [0,1]。沿用当前尺寸语义：diffuse 只允许 0，或非零尺寸上的 1；
  它不作为独立的干湿混合旋钮。`spread=none` 显式忽略尺寸与 diffuse 并记录诊断。
- bed 必须包含一组完整的十个标准 7.1.2 通道。RC、BS.2051 和现有 DAW 别名经公共
  标签规范化映射到固定路由；PCM 按 element ID 绑定，描述符可乱序。单个 LFE role 的空标签
  按 LFE1 解释。重复／缺失标签、多 bed 身份及显式 bed 位置覆盖被拒绝。
- MDAP、Apple 几何、SOFA、非等尺寸、divergence、channelLock、screenRef、headLocked
  等未支持组合严格报错；不会回退到其他声像算法。配置错误同步返回；worker 中发现的
  不支持状态通过 FAILED 状态与结构化 backend diagnostic 报告。
- 后端切换保持现有输出声道数和采样率限制，并使用 2048 输入帧交叉淡化。
  候选拓扑或有效状态不兼容时保留当前后端。每个 renderer 调用先校验完整输入，拒绝时
  不修改该次调用的输出和 DSP 历史。

Scene 的三角函数与衰减幂函数使用已有 libm crate 的可移植实现，文件路径保持原算术。
固定积分格点约 64 KiB，在创建时准备并由会话共享，避免尺寸 ramp 逐样本重复积分。
Rust session 仅持有各元素的控制／滤波状态，不保留历史事件或整轨 PCM；私有 FFI 每个
worker slice 调用一次，处理和 reset 不分配。C++ 的场景复制和控制编排不属于此零分配承诺。
`mr_adm_live_triple_balance_tests`、Scene C API/设备测试及 Rust `live_triple_balance`
测试覆盖固定路由、拒绝原子性、独立 ramp、静音恢复、任意分块和现有静态尺寸内核对照。
跨平台矩阵新增 `scene-triple-balance-*` 用例，涵盖三布局、尺寸过零、重采样、generation、
epoch 与 VBAP 热切换；一致性结论以实际执行平台的结果为准。

### 本次验证（2026-10-07）

macOS arm64 Debug 全部 72 项 CTest、macOS Release 定向 3 项、Linux arm64 Release 定向与
Rust 4 项、Windows x64 canonical Release 定向与 Rust 6 项均通过。新增 32 个 Scene PCM
结果在三平台逐位一致；Windows replay 工具 28 项测试通过，原有矩阵参数保持冻结。

60 秒 Release null 输出使用 7.1.2 bed、两个持续移动／变尺寸对象和一个点源，共 13 路输入。
RTF 单独计量 10 秒等价 live renderer 输入的处理时间。macOS（M4 Pro）9.1.6／22.2 的渲染 RTF 分别约 0.030／0.057，Windows 9.1.6 约 0.066；均无 underrun，
RSS 在预热后未持续增长。此合成基准不替代物理设备或更高对象数素材的实测。
可复现命令为 `mr_adm_triple_balance_scene_benchmark [9.1.6|22.2] 60`，必须使用 Release。
来源哈希、PCM 哈希和详细结果见 [验证记录](evidence/live-triple-balance/validation.json)。

## 模块边界

`MacinRender::ADMRenderTripleBalance` 通过私有适配层使用 Rust 数值内核：

- Rust 持有点源/尺寸/22.2 几何、编译事件表、运动/过渡状态、四路去相关和数值快照；
- C++ 保留对象与 bed 语义、effective report、覆盖发布、文件读写、窗口调度及快照 LRU；
- 与 VBAP 共享的 PCM 增益时间线已由 Rust 持有，C++ 继续复用读取、取消和异步计量编排。

生产目标不依赖 SAF 或 libear；详细所有权、错误边界和验收见
[Rust Triple Balance 迁移](RUST_TRIPLE_BALANCE_MIGRATION.md)。
历史测量 FIR 对照工具保留在 `tests/support/triple_balance_size_filter.*`，不用于生产渲染。

## 名称与公开资料

`balance` 使用单个 `l`。专利 US9467791B2 的表 12（PDF 第 25 页，第 28 栏）
写作 `dualBallance: Dolby method`，同篇正文第 22 栏及权利要求 10、20 则写 `dual balance`。
这构成拼写不一致，没有证据说明双 `l` 是另一种算法。

ITU-R BS.2127-1 §7.3.10（印刷页 53）将 Cartesian point source panner 明确称为
5.1/7.1 dual-balance 概念的三维扩展：相邻 Z 层、Y 行、X 列逐级分配，声道增益为
三轴权重的乘积。Hanschke 等人的 2023 年论文引言直接使用
“dual/triple balance amplitude panning”。因此 Triple Balance 有公开算法家族的依据。
本项目的固定几何、量化、参考语义、尺寸滤波和 22.2 扩展仍以各自的实现与验证为准。

- [US9467791B2](https://patents.google.com/patent/US9467791B2/en)
- [ITU-R BS.2127-1](https://www.itu.int/dms_pubrec/itu-r/rec/bs/R-REC-BS.2127-1-202311-I!!PDF-E.pdf)
- [Improved Panning on Non-Equidistant Loudspeakers with Direct Sound Level Compensation](https://arxiv.org/html/2310.17004v2)
