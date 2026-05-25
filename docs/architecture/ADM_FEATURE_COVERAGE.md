# ADM 特性覆盖审计

本文档记录 MacinRender ADM Core 对 BS.2076 / BS.2127 特性的当前覆盖状态，
以及与 libadm / libear 能力边界之间的差距。

最后更新：2026-05-23

---

## 图例

| 符号 | 含义 |
|---|---|
| ✅ | 已实现并经过测试 |
| ⚠️ | 部分实现或有已知行为偏差 |
| ❌ | 未实现 |
| — | 所在层不适用 |
| 🚫 | 第三方库在运行时抛出 `not_implemented` |

---

## 1. Objects 块参数（`AudioBlockFormatObjects`）

下表中"libear"列表示 libear `GainCalculatorObjects::calculate()` 对该字段的实际处理，
而非仅字段是否出现在 `ObjectsTypeMetadata` 结构体定义中。

| 参数 | libadm 建模 | libear 渲染 | `SceneObjectBlock` | importer | EAR | VBAP |
|---|---|---|---|---|---|---|
| position（polar） | ✅ | ✅ | ✅ | ✅ | ✅ | ✅ |
| position（cartesian） | ✅ | 🚫 见注① | ✅ | ✅ | ✅ 见注① | ✅ |
| gain | ✅ DefaultParam | ✅ | ✅ | ✅ | ✅ | ✅ |
| diffuse | ✅ DefaultParam | ✅ 见注② | ✅ | ✅ | ⚠️ 见注② | — |
| width / height / depth | ✅ DefaultParam | ✅ | ✅ | ✅ | ✅ | ✅（MDAP） |
| rtime / duration | ✅ Default/Opt | — | start/end\_sample | ✅ | ✅ | ✅ |
| jumpPosition + interpolationLength | ✅ DefaultParam | — | ✅ | ✅ | ✅ | ✅ |
| **channelLock** | ✅ DefaultParam | 🚫 flag=true 时抛出 | ✅ | ✅ | ✅ 项目内预处理 | ✅ 项目内预处理 |
| **objectDivergence** | ✅ DefaultParam | 🚫 divergence≠0 时抛出 | ✅ | ✅ | ✅ 项目内预处理 | ✅ 项目内预处理 |
| **zoneExclusion** | ❌ libadm 不解析此字段 | 🚫 zones 非空时抛出 | ❌ | ❌ | — 见注③ | ❌ |
| **screenRef** | ✅ DefaultParam | 🚫 screenRef=true 时抛出 | ✅ | ✅ | ⚠️ warn+degrade | ⚠️ warn+degrade |
| importance | ✅ DefaultParam | — 结构体无此字段 | ❌ | ❌ | — | — |
| headLocked | ✅ DefaultParam | — | ❌ | ❌ | — | — |
| headphoneVirtualise | ✅ DefaultParam | — | ❌ | ❌ | — | — |
| screenEdgeLock | ✅ Opt（在 position 内） | — | ❌ | ❌ | — | — |

### 注①：libear Cartesian Objects（M5 已修复）

libear `GainCalculatorObjects` 的第一行检查：

```cpp
if (metadata.cartesian) throw not_implemented("cartesian");
```

**M5 实现（✅）**：EAR 渲染器现在在 `object_metadata_from_block()` 中将 Cartesian 坐标
按 BS.2076 §10.1 公式转换为极坐标，再传入 libear，不再触发 `not_implemented`：

```
azimuth  = atan2(-X, Y) × (180/π)
elevation = atan2(Z, √(X²+Y²)) × (180/π)
distance  = √(X²+Y²+Z²)
```

VBAP 渲染器自行处理坐标变换（`source_direction()` 已有此逻辑）。

### 注③：zoneExclusion（M6 说明）

libadm 在解析 BW64/ADM XML 时不读取 `zoneExclusion` 元素（标注为 unsupported），
因此该字段永远不会出现在 importer 产生的 `SceneObjectBlock` 中，
`meta.zoneExclusion.zones()` 对 libear 始终为空，libear 的 `not_implemented` 路径不会触发。
当前状态：ADM 内容中的 zoneExclusion 数据在 libadm 层静默丢弃，EAR 渲染不会因此失败，
但语义也未被渲染（等同于无 zoneExclusion）。**无需额外代码防御层**。

### 注②：diffuse bus（M4 已修复）

libear 的增益公式为：

```
directGains  = pv_gains × √(1 − diffuse)
diffuseGains = pv_gains × √(diffuse)
```

**M4 实现（✅）**：EAR 渲染器现在完整处理 diffuse bus：
① `diffuse_gains` 由 `GainCalculatorObjects::calculate()` 同步输出；
② 渲染循环中通过 `designDecorrelators<double>(layout)` 得到的逐声道 512-tap FIR 进行去相关；
③ direct bus 延迟补偿 `decorrelatorCompensationDelay()` = 255 样本后与 diffuse bus 混合。

修复后各场景：
- `diffuse = 0`：direct gains = pv（√1），无 diffuse 贡献，行为与之前一致。
- `0 < diffuse < 1`：direct 和 diffuse 均正确缩放并混合，能量守恒。
- `diffuse = 1`：direct = 0，信号全部经去相关后输出，**不再静音**。

HOA encode 路径同样按 `√(1-d)` / `√d` 拆分 direct 与 diffuse；direct 保持点声源
SH 编码，diffuse 进入固定 32 方向虚拟声场 cloud，并通过短延迟去相关后编码为 HOA3。
VBAP 渲染器仍忽略 `diffuse` 参数；`width/height/depth` 驱动的 MDAP 扩散基于 extent
几何分布，与 ADM diffuse 语义无关。

### 注④：DS CartesianSpeakerPosition（M8b）

libear `GainCalculatorDirectSpeakers` 接受 `CartesianSpeakerPosition` 时在运行时抛出 `not_implemented`。
M8b 的处理策略与 M5 (Objects Cartesian) 完全对称：importer 检测到 `CartesianSpeakerPosition`，
即按 BS.2076 §10.1 公式（`az = atan2(-X,Y)×180/π`，`el = atan2(Z,√(X²+Y²))×180/π`，`dist = √(X²+Y²+Z²)`）
转换为极坐标后写入 `azimuth/elevation/distance`，EAR 和 VBAP 渲染器无需感知原始格式。
libear `CartesianSpeakerPosition` 路径不再触发。

---

## 2. DirectSpeakers 块参数（`AudioBlockFormatDirectSpeakers`）

| 参数 | libadm 建模 | libear `DirectSpeakersTypeMetadata` | `SceneDirectSpeakersBlock` | importer |
|---|---|---|---|---|
| speakerLabels | ✅ | ✅ | ✅ | ✅ |
| SphericalSpeakerPosition（nominal） | ✅ | ✅ | ✅（polar only） | ✅ |
| **position ranges（azMin/Max, elMin/Max, distMin/Max）** | ✅ | ✅ | ✅ | ✅ |
| **CartesianSpeakerPosition** | ✅ | 🚫 运行时抛出 | ✅ 转换为 polar | ✅ 见注④ |
| gain | ✅ DefaultParam | 手动应用 | ✅ | ✅ |
| **rtime / duration**（时域块） | ✅ Default/Opt | — | ✅ | ✅ |
| **channelFrequency**（在 AudioChannelFormat） | ✅ | ✅ | ✅ `low_pass_hz` | ✅ LFE 识别，EAR 透传，VBAP fallback 抑制 |
| importance / headLocked | ✅ DefaultParam | — | ❌ | ❌ |

### DS 时域块（M3.2 已修复）

`AudioBlockFormatDirectSpeakers` 的 `rtime`（DefaultParam）和 `duration`（OptParam）
现已读取：`SceneDirectSpeakersBlock` 新增 `start_sample` / `end_sample`，
importer 从 rtime/duration 推算样本偏移后写入，EAR 和 VBAP 渲染器均已接入时域门控。

---

## 3. AudioObject 级参数

| 参数 | libadm 建模 | `SceneObject` | importer |
|---|---|---|---|
| id / name | ✅ Required | ✅ | ✅ |
| start（含父子层级 BFS 累计） | ✅ DefaultParam | ✅ | ✅ |
| **duration**（对象有效时长上限） | ✅ Opt | ✅ | ✅ |
| **gain**（对象级增益，乘到所有块） | ✅ DefaultParam | ✅ | ✅ |
| **mute** | ✅ DefaultParam | ✅ | ✅ |
| headLocked | ✅ DefaultParam | ❌ | ❌ |
| **positionOffset**（polar / cartesian） | ✅ Opt | ✅ | ✅ |
| interact / disableDucking | ✅ Opt | ❌ | ❌ |
| audioObjectInteraction | ✅ Opt | ❌ | ❌ |
| **dialogue / dialogueId** | ✅ Opt | ✅ `dialogue_id` | ✅ |
| **importance** | ✅ Opt | ✅ `importance` | ✅ |
| **audioObjectLabel** | ✅ VectorParam | ✅ `labels` (values) | ✅ |
| audioComplementaryObjectGroupLabel | ✅ VectorParam | ❌ | ❌ |

`gain`、`mute`、`duration` 已在 M3.1 中实现：`gain` 乘入所有块最终增益，`mute=true` 跳过对象渲染，`duration` 推算 `end_sample` 进行时域门控。

---

## 4. AudioContent 级参数

| 参数 | libadm 建模 | `SceneContent` | importer |
|---|---|---|---|
| id / name | ✅ Required | ✅ | ✅ |
| **audioContentLanguage** | ✅ Opt | ✅ `language` | ✅ |
| **audioContentLabel** | ✅ VectorParam | ✅ `labels` (values) | ✅ |
| **loudnessMetadata** | ✅ VectorParam | ✅ 首条 `SceneLoudnessMetadata` | ✅ 同 Programme |
| **dialogue / contentKind** | ✅（DialogueId + ContentKind variants） | ✅ `dialogue_kind` / `content_kind` 字符串 | ✅ |

---

## 5. AudioProgramme 级参数

| 参数 | libadm 建模 | `SceneProgramme` | importer |
|---|---|---|---|
| id / name | ✅ Required | ✅ | ✅ |
| **audioProgrammeLanguage** | ✅ Opt | ✅ `language` | ✅ |
| **audioProgrammeLabel** | ✅ VectorParam | ✅ `labels` (values) | ✅ |
| **audioProgrammeReferenceScreen** | ✅ Opt（libadm 仅记录元素存在，不解析内部几何） | ✅ `has_reference_screen` bool | ✅ |
| **loudnessMetadata**（内嵌响度） | ✅ VectorParam | ✅ 首条 `SceneLoudnessMetadata` | ✅ method / integrated / true peak / LRA / momentary / short-term / dialogue |
| **start / end** | ✅ Default/Opt | ✅ `start_sample` / `end_sample` | ✅ |
| maxDuckingDepth | ✅ Opt | ❌ | ❌ |

`audioProgrammeReferenceScreen` 是 `screenRef` 完整建模的所需输入。当前 libadm 只记录元素存在，
不解析 reference screen 几何；因此 EAR / VBAP 均只输出 warning，并按普通对象位置渲染。

---

## 6. AudioPackFormat / AudioChannelFormat 层

importer 仅从这两层提取路由信息（TypeDescriptor、对 AudioChannelFormat 的引用）。
其余字段（absoluteDistance、frequency、typeLabel 文本）目前未被提取，
也未被用于 `inspect` 子命令输出的增强。

---

## 7. ADM typeDefinition 覆盖

| typeDefinition | 作为渲染输入 | 渲染/编码输出 |
|---|---|---|
| Objects | ✅ EAR / VBAP / HOA-encode | Objects→HOA3 16ch PCM 文件（encode，非 ADM 输出） |
| DirectSpeakers | ✅ EAR（polar，标签路由）/ VBAP | — |
| HOA | ✅ EAR（GainCalculatorHOA，多 block，mute/gain 门控） | — |
| Matrix | ⚠️ 丢弃，emit `import_warnings` 警告 | — |
| Binaural | ⚠️ 丢弃，emit `import_warnings` 警告 | — |

当前项目没有 ADM/BW64 写出能力；输出为普通 PCM 文件（WAV/CAF float）、FLAC 24-bit integer、Opus MKA 有损交付文件，或 macOS-only APAC `.m4a/.mp4`。Opus MKA 对标准 5.1/7.1 会重排到 Opus/Vorbis 声道顺序；HOA3 写入 Opus ambisonics mapping family 2；其它 9 声道及以上离散布局使用 mapping family 255，属于透明多流编码。布局语义不能只依赖播放器自动识别。

HOA 直接回放目前只确认 macOS 上的 CAF 与 APAC 可行。WAV HOA3 依赖 AmbiX `ambi` chunk 支持；Opus HOA3 虽可写入 ambisonics mapping，但常见播放器兼容性不足，不作为通用直接监听格式。实测 VLC 4.0 可回放 Opus HOA3；播放时需要在音频选项中将 mix node 从 `original: ambisonics` 改为 `binaural`，避免依赖声卡或系统直接承载 HOA 多声道输出。

### HOA typeDefinition 说明

`hoa_renderer.cpp` 的作用是将 **Objects 元数据编码**为 HOA3 球谐系数输出（Objects→HOA encode），
与 ADM HOA 输入渲染方向相反。

- **Objects→HOA encode**：读 Objects 块位置，输出 16ch HOA3 ACN/SN3D。WAV 通过 AmbiX `ambi` chunk 标记，CAF / APAC 使用 CoreAudio `HOA_ACN_SN3D` layout tag。
- **ADM HOA 输入渲染（已实现）**：`scene_importer.cpp` 的 `extract_hoa_packs()` 读取
  `typeDefinition=HOA` 的 AudioChannelFormat，解析每个 AudioBlockFormatHoa 的
  order/degree/gain/rtime/duration，以及 AudioPackFormatHoa 层的 normalization/nfcRefDist/screenRef；
  `ear_renderer.cpp` 使用 `GainCalculatorHOA::calculate()` 将 HOA 系数解码为扬声器增益，
  支持多 block 时间门控、AudioObject 级 mute 和 gain 缩放，
  无 blocks 的 UID（UID→CF 链断裂）自动跳过，不产生 phantom W 通道。

`typeDefinition=Matrix/Binaural` 块仍不参与渲染，但 importer 会写入
`AdmScene.import_warnings`，`RenderService` 会以 `importer` warning 输出，避免用户误以为已渲染。

---

## 8. HOA 渲染器特有缺口

HOA 渲染器当前功能：Objects / DirectSpeakers 块 → SH3（SN3D，16ch）编码输出。

| 缺口 | 当前状态 |
|---|---|
| 块级时间门控 | ✅ 已实现（start\_sample / end\_sample） |
| jumpPosition / 插值斜坡 | ✅ 已实现（M8c：binary-search 块定位 + 5ms 线性插值，与 VBAP/EAR 一致） |
| diffuse 参数 | ✅ 已实现：direct / diffuse 能量拆分，diffuse 通过 32 方向虚拟 cloud 去相关后编码 |
| 固定 SN3D 输出 | ⚠️ order/normalization 暂不可配置；不读取 ADM HOA 参数，因为该路径是 Objects→HOA encode，不是 ADM HOA 输入渲染 |

`normalization`、`nfcRefDist`、`screenRef` 是 ADM HOA 输入渲染的字段（已在 `SceneHOATracks` / `extract_hoa_packs()` 实现），
不属于当前 Objects→HOA encode 路径的范围。

HOA3 输出的响度 / True Peak 不再直接测量系数域。渲染器会把非 LFE HOA 内容解码到 7.1.4
AllRAD 参考播放域后送入 ebur128；DirectSpeakers LFE 仍以 W-only 形式写入 HOA 输出，但会从
LUFS / 空间 True Peak 测量缓冲中剥离，并由独立 mono True Peak tracker 计入最终峰值。
因此 `--loudness-target` 与 `--peak-limit-dbtp` 可用于 HOA 输出；结果代表项目定义的 7.1.4
参考解码，而不是任意下游 HOA 解码器的发行响度。

---

## 9. 平台无关 DSP / 渲染扩展

下列能力不完全等同于 ADM 字段覆盖，而是面向后续里程碑的渲染器能力扩展。
它们应尽量落在 `adm_render_*` 或独立 DSP 模块内，不反向污染 `AdmScene` 领域模型。

| 能力 | 当前状态 | 建议定位 |
|---|---|---|
| **HRTF / SOFA binauraliser** | ✅ 已有独立 `binaural` 后端：默认 SAF 内置 KEMAR HRTF，支持 `--sofa` 加载用户 FIR SOFA HRIR（首版 2 receiver / 48 kHz / 不重采样），输出 2ch binaural | 后续可扩展更多 HRTF 数据集选择、距离策略和房间响应 |
| **decorrelator / diffuse bus** | ✅ EAR 已实现 BS.2127 去相关 FIR + 延迟补偿（M4）；HOA encode 使用 32 方向去相关 diffuse cloud；VBAP 忽略 ADM `diffuse` | — |
| **reverb / room simulation** | ❌ 未实现 | 可作为可选后处理，不应默认改变 ADM 合规渲染结果 |
| **扬声器布局统一** | ✅ EAR / SAF VBAP 共用项目布局 registry；对外均显示 8 个 speaker layouts（内部另保留 `0+2+0`） | 后续可增加配置文件或插件式布局源 |
| **VBAP 布局扩展** | ✅ 通道顺序和 LFE 位置已校对；SAF 补齐 `5.1.2` / `9.1.4` / `9.1.6`，并保留 `register_vbap_layout()` 运行时注册入口 | — |
| **VBAP 2D / 3D 配置** | ✅ 自动按布局高度判断；能力报告和 render 日志显示 2D/3D，2D 布局遇到高度源会 warning | 后续可增加显式 override 开关 |
| **VBAP 插值策略配置** | ✅ `RenderOptions.default_interp_ms`（默认 5ms，0=瞬时切换，CLI `--interp-ms`）；EAR / HOA 渲染器同步生效 | — |
| **Objects 动态元数据去拉链** | ✅ `RenderOptions.object_smoothing_frames`（默认 8875，CLI `--object-smoothing-frames`）；VBAP / EAR / HOA encode / binaural 均接入，DirectSpeakers 不平滑 | 用于高密度 jumpPosition 压力素材的工程平滑；设为 0 可逐样本跟随 ADM 块 |

### HRTF / SOFA binauraliser

`binaural` 已从普通立体声别名升级为独立双耳后端。当前实现路径为：
Objects / DirectSpeakers → 位置解析 → HRTF 数据集（默认 SAF 内置 KEMAR，或 CLI `--sofa` 用户 SOFA）→
OLA 分块卷积 → 固定 2ch binaural 输出。
渲染器会忽略用户请求的扬声器布局；PCM CAF 写入时使用 CoreAudio `Binaural` layout tag，
普通 speaker stereo ADM 渲染不再作为用户入口暴露：默认 2ch 输出会自动路由到
`binaural`，显式请求 `--renderer ear|saf --output-layout stereo/0+2+0` 会返回
`unsupported`。底层音频 I/O 仍保留 `0+2+0`，仅用于普通两声道文件格式写入、测试和
非 ADM 渲染语义，避免把不可听的 speaker-stereo 投影误认为双耳或正式下混。

SOFA 支持通过 `MR_ADM_ENABLE_SOFA=ON` 默认启用，使用 SAF 的 SOFA reader 与其内置
libmysofa/zlib 路径，`SAF_ENABLE_NETCDF` 保持关闭。首版边界为 free-field FIR HRIR：
`SOFAConventions` 仅接受 `SimpleFreeFieldHRIR` / `GeneralFIR`，`DataType` 必须为 `FIR`，
`nReceivers` 必须为 2，`DataSamplingRate` 必须等于输入 BWF 采样率（当前 binaural 后端仍要求
48 kHz），`DataLengthIR > 0` 且 `nSources >= 4`。spherical `SourcePosition` 使用
degree/degree/metre，azimuth 按 ADM 的 `+az = left` 直接消费；cartesian `SourcePosition`
按 SOFA 的 `+X front, +Y left, +Z up` 转为极坐标。其他单位、TF SOFA、BRIR、重采样和
receiver 数不为 2 的文件都会返回 `unsupported` 或清晰的加载错误。

手动验证（2026-05-23，`afinfo`）显示：PCM CAF 可读为 `Channel layout: Binaural`；
APAC `.m4a/mp4f` 以及 APAC-in-CAF 即使请求 `Binaural`，最终仍报告为 `Stereo (L R)`。
因此 APAC 目前不能作为可靠的 binaural layout carrier。项目会保留 2ch 音频本体，
并在 `.m4a/.mp4` 的 iTunes-style comment metadata（`©cmt`）中写入
`layout=binaural`，供自家工具和诊断流程恢复语义；播放器仍可能只按普通 stereo
显示或播放。

ALAC 与 APAC 一样通常交付为 `.m4a/mp4` 容器。ALAC 编码算法本身已有开源实现，
但跨平台支持的真正成本在 MP4 muxer、iTunes metadata、channel layout 和依赖分发，
而不是 lossless 压缩核心。因此 ALAC 不进入首个全平台核心格式集合；若后续实现，
优先作为 Apple 生态便利格式处理：

- macOS 快路径：AudioToolbox ALAC encoder，工程量最低，但属于 Apple-only 后端；
- 全平台可选路径：FFmpeg/libavformat provider，能力完整但依赖体积和许可证审计成本高；
- 自维护路径：vendored Apple ALAC encoder + 项目内 MP4 muxer，最可控但工作量最大。

Binaural ALAC 的语义策略与 APAC 一致：音频本体为 2ch lossless ALAC，容器里写
`©cmt` comment metadata（`layout=binaural`），不把它伪装成普通 speaker stereo。
播放器仍可能只显示 Stereo；项目工具应以后备 metadata 恢复 binaural 语义。

仍待实现：

- BRIR / room-response SOFA、TF SOFA、HRIR 重采样和多 receiver 数据集；
- 距离策略、头外化相关补偿；
- HOA 输入到 binaural 的串接方式；
- ALAC 输出（先 macOS-only AudioToolbox 快路径，跨平台 provider 另立里程碑评估）。

### decorrelator / diffuse

EAR diffuse bus 已在 M4 中实现（见注②）：`designDecorrelators()` FIR 去相关 + `decorrelatorCompensationDelay()` direct 延迟补偿。HOA encode 不绑定扬声器布局；它将 diffuse 能量送入 32 个固定球面方向的虚拟 cloud，以短延迟去相关后编码为 HOA3。VBAP 的 MDAP spread 对应 extent 几何扩散，与 ADM `diffuse` 语义无关，暂不计划实现 VBAP 侧去相关。

### reverb / room simulation

房间模拟不属于 BS.2076 / BS.2127 基础渲染要求，默认启用会改变合规输出。
若实现，应作为显式 opt-in 的后处理或单独 renderer option，并在测试中区分
"ADM 合规渲染"与"创作型空间效果"。

### VBAP 完整度

当前 SAF VBAP 后端已经覆盖 Objects / DirectSpeakers、2D / 3D gain table、
ADM 块插值和 MDAP extent spread。EAR 与 SAF VBAP 的扬声器布局能力由
`adm_render_common` 中的共享 registry 驱动。

**已内置扬声器布局（`speaker_layouts.cpp`）：**

| 布局 ID | 名称 | 声道数 | LFE |
|---|---|---|---|
| `0+2+0` | Stereo（内部保留，speaker 渲染入口禁用） | 2 | — |
| `0+5+0` | 5.1 | 6 | LFE1@ch3 |
| `2+5+0` | 5.1.2 | 8 | LFE1@ch3 |
| `wav71` | WAV 7.1 | 8 | LFE1@ch3 |
| `4+5+0` | 5.1.4 | 10 | LFE1@ch3 |
| `4+7+0` | 7.1.4 | 12 | LFE1@ch3 |
| `4+5+4` | 9.1.4 | 14 | LFE1@ch3 |
| `9.1.6` | 9.1.6 (Dolby Atmos) | 16 | LFE1@ch3 |
| `9+10+3` | 22.2 | 24 | LFE1@ch3，LFE2@ch9 |

`0+2+0` 用于内部测试和普通两声道文件写入，不在 CLI `backends` 的 speaker layouts 中对外显示；用户 2ch 渲染入口仍走 `binaural`。`wav71` 使用 CoreAudio `kAudioChannelLayoutTag_WAVE_7_1` / Microsoft WAVE 7.1 槽位；`9.1.4` 与 `9.1.6` 使用项目侧 Atmos-style 声道顺序，其中 libear 后端通过自定义 `ear::Layout` 构造。LFE 声道参与输出但不参与 VBAP panning；DS LFE 轨按标签（"LFE1"/"LFE2"）直接路由。

**仍待改善：**

- 内置布局仍是代码内静态 registry，缺少配置文件或外部布局包加载机制；
- 2D / 3D 由布局是否存在非零 elevation 自动决定，尚无用户显式 override；
- 3D spread 使用 MDAP，2D SAF API 无 spread 参数；当前通过能力报告、日志和 warning 暴露行为差异；
- 过长 interpolationLength 的 clamp / 诊断策略仍可继续加固。

---

## 10. 优先级分类

### P1 — 影响渲染正确性（当前输出有可测量偏差）

> 所有 P1 项已修复：diffuse bus 丢弃（M4）、AudioObject gain/mute/duration 未读（M3.1）、
> DS 块无时间窗（M3.2）、EAR 拒绝 Cartesian Objects（M5）。

### P2 — libear 字段暴露（已防御 / 已预处理）

| 问题 | libear 行为 | 状态 |
|---|---|---|
| ~~channelLock~~ | ~~flag=true 时 `not_implemented`~~ | ✅ 项目内按输出布局锁定最近非 LFE 扬声器，EAR/VBAP 均不再传给 libear |
| ~~objectDivergence~~ | ~~divergence≠0 时 `not_implemented`~~ | ✅ 项目内展开为 left/center/right 虚拟源，EAR/VBAP 均不再传给 libear |
| screenRef | screenRef=true 时 `not_implemented` | ⚠️ 需要 referenceScreen geometry；当前 warn+degrade |

### 上游解析限制（libadm 不支持，运行时风险不可达）

| 问题 | libadm 行为 | 状态 |
|---|---|---|
| zoneExclusion | 不解析此字段，zones 始终为空 | — 语义未支持，但不造成渲染失败（见注③） |

warn+degrade 是防御性处理：libear 不抛出，但 P2 语义未完整渲染。

### P2b — importer 缺口（与 libear 支持无关）

| 问题 | 涉及文件 |
|---|---|
| ~~DS 位置范围（azMin/Max 等）~~ | ~~已在 M8a 中实现~~ |
| ~~DS CartesianSpeakerPosition~~ | ~~已在 M8b 中实现（importer 转极坐标）~~ |
| ~~AudioObject.positionOffset~~ | ~~已在 M7 中实现~~ |
| ~~HOA jumpPosition / 插值~~ | ~~已在 M8c 中实现~~ |
| ~~VBAP 布局正确性（LFE 路由、通道顺序、9.1.6）~~ | ~~已在 2026-05-20 修复~~ |
| ~~VBAP 布局外部注册 / CLI 能力报告增强~~ | ~~已实现：`register_vbap_layout()`、registry 驱动 `vbap_capabilities()`、EAR/HOA 能力字段补全、CLI 显示 ch/lfe/3d/spread~~ |

### P3 — 功能完整性（原版也未实现）

| 问题 | 说明 |
|---|---|
| ~~audioProgrammeReferenceScreen~~ | ~~已实现：`has_reference_screen` bool，libadm 不解析内部几何，screenRef warn+degrade 不变~~ |
| ~~AudioContent/Programme language & label~~ | ~~已实现：AudioProgramme language/labels/start/end；AudioContent language/labels；AudioObject labels/importance/dialogueId~~ |
| ~~AudioContent language / label / loudness / dialogue~~ | ~~已实现：language、label values、首条 loudnessMetadata、dialogue_kind/content_kind 字符串~~ |
| ~~AudioProgramme loudnessMetadata~~ | ~~已实现：读取首条内嵌响度元数据并在 CLI inspect 显示~~ |
| ~~ADM HOA 块解码（typeDefinition=HOA）~~ | ~~已实现：GainCalculatorHOA，多 block，mute/gain，空 block 跳过~~ |
| ADM Matrix / Binaural 块 | 复杂度高，覆盖率低 |
| ~~HRTF binauraliser 基础后端~~ | ~~已实现：SAF KEMAR HRTF，Objects/DirectSpeakers，固定 48 kHz，PCM CAF 标记为 CoreAudio Binaural；APAC 经 `afinfo` 验证仍显示 Stereo~~ |
| ~~SOFA binauraliser 基础路径~~ | ~~已实现：`--sofa` 加载用户 FIR SOFA HRIR，首版限制 2 receiver / 48 kHz / 不重采样；NetCDF 关闭，走 SAF 内置 libmysofa/zlib~~ |
| ~~VBAP 插值策略配置~~ | ~~已实现：`RenderOptions.default_interp_ms`（默认 5ms，CLI `--interp-ms`），EAR/HOA 同步~~ |
| ~~Objects 动态元数据去拉链~~ | ~~已实现：`RenderOptions.object_smoothing_frames`（默认 8875，CLI `--object-smoothing-frames`），覆盖 VBAP/EAR/HOA encode/binaural；DirectSpeakers 保持物理声道语义~~ |
| ~~VBAP 2D / 3D 布局诊断~~ | ~~已实现：`is_2d_layout()` 基于输出布局扬声器位置固定选择 2D/3D VBAP；2D 布局 + 有高度源时发 warning；render 日志显示 "2D VBAP" / "3D VBAP"~~ |
| reverb / room simulation | 可选创作型空间效果，不属于默认合规渲染 |

### 暂不实现

| 参数 | 原因 |
|---|---|
| headLocked / headphoneVirtualise | libear 不支持；平台渲染器（SpatialMixer）职责 |
| importance（渲染优先级调度） | 资源调度策略，不影响增益数学；字段已读入 `SceneObject.importance` |
| interact / audioObjectInteraction | 交互层元数据，不适用于离线渲染 |
| disableDucking / maxDuckingDepth | 条件混音控制，超出当前范围 |

---

## 附录：libear `ObjectsTypeMetadata` 完整字段与运行时行为

```cpp
struct ObjectsTypeMetadata {
    Position position;           // PolarPosition ✅ | CartesianPosition 🚫
    double width{0.0};           // ✅
    double height{0.0};          // ✅
    double depth{0.0};           // ✅
    bool cartesian{false};       // 🚫 true 时 not_implemented
    double gain{1.0};            // ✅
    double diffuse{0.0};         // ✅（direct *= √(1-d)，diffuse *= √d）
    ChannelLock channelLock;     // 🚫 flag=true 时 not_implemented；项目内预处理后保持默认
    ObjectDivergence objectDivergence; // 🚫 divergence≠0 时 not_implemented；项目内预处理后保持默认
    ZoneExclusion zoneExclusion; // 🚫 zones 非空时 not_implemented
    bool screenRef{false};       // 🚫 true 时 not_implemented
    Screen referenceScreen;      // screenRef 生效时使用
};
```

## 附录：libear `DirectSpeakersTypeMetadata` 完整字段

```cpp
struct DirectSpeakersTypeMetadata {
    std::vector<std::string> speakerLabels;   // ✅
    SpeakerPosition position;                  // polar ✅ | cartesian 输入会在 importer 转为 polar ✅
    // PolarSpeakerPosition 含 azimuthMin/Max, elevationMin/Max, distanceMin/Max
    // nominal 与 min/max 范围字段均已透传
    ChannelFrequency channelFrequency;         // ✅ lowPass → SceneDirectSpeakersBlock.low_pass_hz
    boost::optional<std::string> audioPackFormatID; // ✅
};
```
