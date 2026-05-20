# ADM 特性覆盖审计

本文档记录 MacinRender ADM Core 对 BS.2076 / BS.2127 特性的当前覆盖状态，
以及与 libadm / libear 能力边界之间的差距。

最后更新：2026-05-19

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
| **channelLock** | ✅ DefaultParam | 🚫 flag=true 时抛出 | ✅ | ✅ | ⚠️ warn+degrade | — |
| **objectDivergence** | ✅ DefaultParam | 🚫 divergence≠0 时抛出 | ✅ | ✅ | ⚠️ warn+degrade | — |
| **zoneExclusion** | ❌ libadm 不解析此字段 | 🚫 zones 非空时抛出 | ❌ | ❌ | — 见注③ | ❌ |
| **screenRef** | ✅ DefaultParam | 🚫 screenRef=true 时抛出 | ✅ | ✅ | ⚠️ warn+degrade | — |
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

VBAP 渲染器忽略 `diffuse` 参数；`width/height/depth` 驱动的 MDAP 扩散
基于 extent 几何分布，与 ADM diffuse 语义无关。

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
| **channelFrequency**（在 AudioChannelFormat） | ✅ | ✅ | ❌ | ❌ |
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
| dialogue / dialogueId | ✅ Opt | ❌ | ❌ |
| importance | ✅ Opt | ❌ | ❌ |
| audioObjectLabel | ✅ VectorParam | ❌ | ❌ |
| audioComplementaryObjectGroupLabel | ✅ VectorParam | ❌ | ❌ |

`gain`、`mute`、`duration` 已在 M3.1 中实现：`gain` 乘入所有块最终增益，`mute=true` 跳过对象渲染，`duration` 推算 `end_sample` 进行时域门控。

---

## 4. AudioContent 级参数

| 参数 | libadm 建模 | `SceneContent` | importer |
|---|---|---|---|
| id / name | ✅ Required | ✅ | ✅ |
| **audioContentLanguage** | ✅ Opt | ❌ | ❌ |
| **audioContentLabel** | ✅ VectorParam | ❌ | ❌ |
| **loudnessMetadata** | ✅ VectorParam | ❌ | ❌ |
| **dialogue / contentKind** | ✅（DialogueId + ContentKind variants） | ❌ | ❌ |

---

## 5. AudioProgramme 级参数

| 参数 | libadm 建模 | `SceneProgramme` | importer |
|---|---|---|---|
| id / name | ✅ Required | ✅ | ✅ |
| **audioProgrammeLanguage** | ✅ Opt | ❌ | ❌ |
| **audioProgrammeLabel** | ✅ VectorParam | ❌ | ❌ |
| **audioProgrammeReferenceScreen** | ✅ Opt | ❌ | ❌ |
| **loudnessMetadata**（内嵌响度） | ✅ VectorParam | ❌ | ❌ |
| start / end | ✅ Default/Opt | ❌ | ❌ |
| maxDuckingDepth | ✅ Opt | ❌ | ❌ |

`audioProgrammeReferenceScreen` 是 `screenRef` 完整建模的所需输入，用于填入
`ear::ObjectsTypeMetadata::referenceScreen`。但当前 libear `GainCalculatorObjects`
对 `screenRef=true` 直接抛出 `not_implemented`，因此近期只能按 P2 策略处理：
warn + 降级为 `screenRef=false`，而非尝试完整渲染。

---

## 6. AudioPackFormat / AudioChannelFormat 层

importer 仅从这两层提取路由信息（TypeDescriptor、对 AudioChannelFormat 的引用）。
其余字段（absoluteDistance、frequency、typeLabel 文本）目前未被提取，
也未被用于 `inspect` 子命令输出的增强。

---

## 7. ADM typeDefinition 覆盖

| typeDefinition | 作为渲染输入 | 渲染/编码输出 |
|---|---|---|
| Objects | ✅ EAR / VBAP / HOA-encode | Objects→HOA3 16ch WAV（encode，非 ADM 输出） |
| DirectSpeakers | ✅ EAR（polar，标签路由）/ VBAP | — |
| HOA | ❌ ADM HOA 块静默丢弃 | — |
| Matrix | ❌ 静默丢弃 | — |
| Binaural | ❌ 静默丢弃 | — |

当前项目没有 ADM/BW64 写出能力；所有输出均为普通 WAV 文件。

### HOA typeDefinition 说明

`hoa_renderer.cpp` 的作用是将 **Objects 元数据编码**为 HOA3 球谐系数输出，
不是解码渲染 ADM 文件中的 `typeDefinition=HOA` 块。两件事方向相反：

- **当前实现（Objects→HOA encode）**：读 Objects 块位置，输出 16ch HOA3 WAV。
- **ADM HOA 输入渲染（未实现）**：读 ADM HOA 块系数，用 `GainCalculatorHOA`
  解码为扬声器增益（HOA→扬声器 decode），目前 libear 的这条路径完全未使用。

`typeDefinition=HOA/Matrix/Binaural` 块在 importer 的 `populate_track_blocks()`
的类型分支中静默跳过，无警告、无报错。

---

## 8. HOA 渲染器特有缺口

HOA 渲染器当前功能：Objects 块 → SH3（SN3D，16ch）编码输出。

| 缺口 | 当前状态 |
|---|---|
| 块级时间门控 | ✅ 已实现（start\_sample / end\_sample） |
| jumpPosition / 插值斜坡 | ✅ 已实现（M8c：binary-search 块定位 + 5ms 线性插值，与 VBAP/EAR 一致） |
| diffuse 参数 | ❌ 编码时忽略 |
| 固定 SN3D 输出 | ⚠️ order/normalization 暂不可配置；不读取 ADM HOA 参数，因为该路径不是 ADM HOA 输入渲染 |

`normalization`、`nfcRefDist`、`screenRef` 是 `AudioBlockFormatHoa`
（ADM HOA 输入格式）的字段，不属于当前 Objects→HOA encode 路径的范围；
它们应归属于"ADM HOA 输入渲染（typeDefinition=HOA，未实现）"。

---

## 9. 平台无关 DSP / 渲染扩展

下列能力不完全等同于 ADM 字段覆盖，而是面向后续里程碑的渲染器能力扩展。
它们应尽量落在 `adm_render_*` 或独立 DSP 模块内，不反向污染 `AdmScene` 领域模型。

| 能力 | 当前状态 | 建议定位 |
|---|---|---|
| **HRTF / SOFA binauraliser** | ❌ 未实现；`binaural` alias 已删除（原映射到 `0+2+0`，无 HRTF） | 独立 binaural 后端或渲染后处理，读取 SOFA/HRTF 数据集 |
| **decorrelator / diffuse bus** | ✅ EAR 已实现 BS.2127 去相关 FIR + 延迟补偿（M4）；VBAP 忽略 ADM `diffuse` | — |
| **reverb / room simulation** | ❌ 未实现 | 可作为可选后处理，不应默认改变 ADM 合规渲染结果 |
| **更完整 VBAP 布局表** | ⚠️ 仅硬编码少量布局 | 抽出布局注册表，覆盖更多 BS.2051 / CICP 常用布局 |
| **VBAP 2D / 3D 配置** | ⚠️ 自动按布局高度判断；无用户可见配置 | 显式能力报告和配置开关，避免 2D 无 spread 参数时行为不透明 |
| **VBAP 插值策略配置** | ⚠️ 支持 ADM `jumpPosition/interpolationLength`，默认 5ms；不可配置 | 暴露默认 ramp、clamp、瞬时切换等策略选项 |

### HRTF / SOFA binauraliser

`binaural` 别名已删除（原行为：EAR / VBAP 将其映射到 `0+2+0`，输出普通立体声，无 HRTF 处理）。
真正的 binauraliser 尚未实现，需要：

- SOFA 文件加载、HRTF 数据集选择和采样率匹配；
- 方位插值、距离策略、头外化相关补偿；
- 分块卷积和延迟管理；
- 与 Objects / DirectSpeakers / HOA decode 输出之间的清晰串接方式。

### decorrelator / diffuse

EAR diffuse bus 已在 M4 中实现（见注②）：`designDecorrelators()` FIR 去相关 + `decorrelatorCompensationDelay()` direct 延迟补偿。VBAP 的 MDAP spread 对应 extent 几何扩散，与 ADM `diffuse` 语义无关，暂不计划实现 VBAP 侧去相关。

### reverb / room simulation

房间模拟不属于 BS.2076 / BS.2127 基础渲染要求，默认启用会改变合规输出。
若实现，应作为显式 opt-in 的后处理或单独 renderer option，并在测试中区分
"ADM 合规渲染"与"创作型空间效果"。

### VBAP 完整度

当前 SAF VBAP 后端已经覆盖 Objects / DirectSpeakers、2D / 3D gain table、
ADM 块插值和 MDAP extent spread，但仍偏工程内置：

- 布局表硬编码在 `vbap_renderer.cpp`，缺少外部可扩展布局注册；
- 2D / 3D 由布局是否存在非零 elevation 自动决定，CLI/能力报告没有显式说明；
- 3D spread 使用 MDAP，2D SAF API 无 spread 参数，行为需要在文档和能力报告中暴露；
- 默认插值 5ms、过长 ramp clamp 等策略尚未成为用户可配置选项。

---

## 10. 优先级分类

### P1 — 影响渲染正确性（当前输出有可测量偏差）

> 所有 P1 项已修复：diffuse bus 丢弃（M4）、AudioObject gain/mute/duration 未读（M3.1）、
> DS 块无时间窗（M3.2）、EAR 拒绝 Cartesian Objects（M5）。

### P2 — libear 字段暴露（已防御）

| 问题 | libear 行为 | 状态 |
|---|---|---|
| channelLock | flag=true 时 `not_implemented` | ⚠️ warn+degrade（M3.3） |
| objectDivergence | divergence≠0 时 `not_implemented` | ⚠️ warn+degrade（M3.3） |
| screenRef | screenRef=true 时 `not_implemented` | ⚠️ warn+degrade（M3.3） |

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
| VBAP 布局注册表 / 能力报告增强 | `vbap_renderer.cpp`、CLI |

### P3 — 功能完整性（原版也未实现）

| 问题 | 说明 |
|---|---|
| audioProgrammeReferenceScreen | screenRef 的前提 |
| AudioContent/Programme language & label | 多语言选择、UI 展示 |
| AudioContent dialogue / contentKind | 条件混音、对话分类 |
| AudioProgramme loudnessMetadata | 读取内嵌响度元数据 |
| ADM HOA 块解码（typeDefinition=HOA） | 需要 libear GainCalculatorHOA |
| ADM Matrix / Binaural 块 | 复杂度高，覆盖率低 |
| HRTF / SOFA binauraliser | 真正的耳机双耳渲染；`binaural` alias 已删除，HRTF 后端尚未实现 |
| VBAP 2D / 3D / 插值策略配置 | 当前自动选择且大多硬编码 |
| reverb / room simulation | 可选创作型空间效果，不属于默认合规渲染 |

### 暂不实现

| 参数 | 原因 |
|---|---|
| headLocked / headphoneVirtualise | libear 不支持；平台渲染器（SpatialMixer）职责 |
| importance | 资源调度策略，不影响增益数学 |
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
    ChannelLock channelLock;     // 🚫 flag=true 时 not_implemented
    ObjectDivergence objectDivergence; // 🚫 divergence≠0 时 not_implemented
    ZoneExclusion zoneExclusion; // 🚫 zones 非空时 not_implemented
    bool screenRef{false};       // 🚫 true 时 not_implemented
    Screen referenceScreen;      // screenRef 生效时使用
};
```

## 附录：libear `DirectSpeakersTypeMetadata` 完整字段

```cpp
struct DirectSpeakersTypeMetadata {
    std::vector<std::string> speakerLabels;   // ✅
    SpeakerPosition position;                  // polar ✅ | cartesian 🚫（运行时抛出，且未导入）
    // PolarSpeakerPosition 含 azimuthMin/Max, elevationMin/Max, distanceMin/Max
    // 当前只传 nominal 值，范围字段均为 boost::none
    ChannelFrequency channelFrequency;         // ❌ 未导入
    boost::optional<std::string> audioPackFormatID; // ✅
};
```
