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

## 模块边界

`src/adm_render_triple_balance/` 和 `MacinRender::ADMRenderTripleBalance` 独立拥有：

- 点源房间几何、位置量化与运动平滑；
- 对象和 bed 语义适配及 effective report；
- 尺寸空间增益、四路有状态去相关处理及生命周期；
- 22.2 自有几何。

生产目标不依赖 SAF 或 libear。与 VBAP 共享的增益时间线、文件读写、窗口裁剪、
取消与异步计量位于 `adm_render_common/speaker_pcm.*`，接口仅使用项目类型。
历史测量 FIR 对照工具移至 `tests/support/triple_balance_size_filter.*`，仅其测试使用 SAF FFT。

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
