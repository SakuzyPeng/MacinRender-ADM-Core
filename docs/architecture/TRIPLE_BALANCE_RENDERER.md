# Triple Balance 渲染器

Triple Balance 是独立于 SAF VBAP 的离线房间坐标渲染后端。
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
  窗口之外的音频不写出、不进入该窗口计量。实时流仍返回 unsupported。
- 保留 speaker re-render WAV profile：7.1.4 按 WAVE mask 重排；9.1.6 保留原生声道序、
  WAVEFORMATEXTENSIBLE mask 0，不写 ADM；需要显式 CoreAudio 9.1.6 布局时使用 CAF。

详细参考行为与数值验证见 [等尺寸内核](DAR_NATIVE_SIZE_ALIGNMENT.md)、
[对象语义](DAR_OBJECT_SEMANTICS.md)、[bed 语义](DAR_BED_SEMANTICS.md)、
[22.2 扩展](ROOM_222_EXTENSION.md)。更名不扩大这些证据的适用范围。

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
