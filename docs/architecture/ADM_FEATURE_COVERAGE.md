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
| position（cartesian） | ✅ | 🚫 见注① | ✅ | ✅ | 🚫 见注① | ✅ |
| gain | ✅ DefaultParam | ✅ | ✅ | ✅ | ✅ | ✅ |
| diffuse | ✅ DefaultParam | ✅ 见注② | ✅ | ✅ | ⚠️ 见注② | — |
| width / height / depth | ✅ DefaultParam | ✅ | ✅ | ✅ | ✅ | ✅（MDAP） |
| rtime / duration | ✅ Default/Opt | — | start/end\_sample | ✅ | ✅ | ✅ |
| jumpPosition + interpolationLength | ✅ DefaultParam | — | ✅ | ✅ | ✅ | ✅ |
| **channelLock** | ✅ DefaultParam | 🚫 flag=true 时抛出 | ❌ | ❌ | ❌ | ❌ |
| **objectDivergence** | ✅ DefaultParam | 🚫 divergence≠0 时抛出 | ❌ | ❌ | ❌ | ❌ |
| **zoneExclusion** | ❌ libadm 标注 unsupported | 🚫 zones 非空时抛出 | ❌ | ❌ | ❌ | ❌ |
| **screenRef** | ✅ DefaultParam | 🚫 screenRef=true 时抛出 | ❌ | ❌ | ❌ | ❌ |
| importance | ✅ DefaultParam | — 结构体无此字段 | ❌ | ❌ | — | — |
| headLocked | ✅ DefaultParam | — | ❌ | ❌ | — | — |
| headphoneVirtualise | ✅ DefaultParam | — | ❌ | ❌ | — | — |
| screenEdgeLock | ✅ Opt（在 position 内） | — | ❌ | ❌ | — | — |

### 注①：libear Cartesian Objects 在运行时抛出

libear `GainCalculatorObjects` 的第一行检查：

```cpp
if (metadata.cartesian) throw not_implemented("cartesian");
```

`not_implemented` 继承 `std::runtime_error`，被 EAR 渲染器的
`catch (std::exception&)` 捕获后以 `ErrorCode::render_failed` 返回。
因此，包含笛卡尔坐标 Objects 块的 ADM 文件送入 EAR 渲染器会渲染失败。
VBAP 渲染器自行处理坐标变换，不依赖 libear，故 VBAP 侧笛卡尔坐标正常工作。

### 注②：diffuse bus 被丢弃（P1 合规缺陷）

libear 的增益公式为：

```
directGains  = pv_gains × √(1 − diffuse)
diffuseGains = pv_gains × √(diffuse)
```

标准要求 diffuse bus 经过 `designDecorrelators()` 提供的 FIR 去相关滤波器，
延迟补偿后与 direct bus 混合。当前实现调用 `calculate()` 但丢弃 `diffuseGains`：

```cpp
std::vector<double> diffuse_gains(num_out, 0.0);
objects_calc.calculate(meta, bg.gains, diffuse_gains);
// diffuse_gains 未使用
```

实际后果：

- `diffuse = 0`：direct gains 不受影响（√1 = 1），渲染正确。
- `0 < diffuse < 1`：direct gains 按 √(1−diffuse) 衰减，能量丢失；
  diffuse bus 的去相关贡献完全缺失。
- `diffuse = 1`：direct gains 乘以 √0 = 0，渲染结果为**静音**。

这是 EAR 渲染器目前最严重的 BS.2127 合规偏差。完整修复需要：
① 从 libear 取 `designDecorrelators()` 和 `decorrelatorCompensationDelay()`；
② 维护逐声道 FIR 卷积状态；③ 混合时对 direct bus 补偿延迟。

VBAP 渲染器目前忽略 `diffuse` 参数；`width/height/depth` 驱动的 MDAP 扩散
基于 extent 几何分布，与 ADM diffuse 语义无关。

---

## 2. DirectSpeakers 块参数（`AudioBlockFormatDirectSpeakers`）

| 参数 | libadm 建模 | libear `DirectSpeakersTypeMetadata` | `SceneDirectSpeakersBlock` | importer |
|---|---|---|---|---|
| speakerLabels | ✅ | ✅ | ✅ | ✅ |
| SphericalSpeakerPosition（nominal） | ✅ | ✅ | ✅（polar only） | ✅ |
| **position ranges（azMin/Max, elMin/Max, distMin/Max）** | ✅ | ✅ | ❌ | ❌ |
| **CartesianSpeakerPosition** | ✅ | 🚫 运行时抛出 | ❌ | ❌ |
| gain | ✅ DefaultParam | 手动应用 | ✅ | ✅ |
| **rtime / duration**（时域块） | ✅ Default/Opt | — | ❌ | ❌ |
| **channelFrequency**（在 AudioChannelFormat） | ✅ | ✅ | ❌ | ❌ |
| importance / headLocked | ✅ DefaultParam | — | ❌ | ❌ |

### DS 时域块缺失

`AudioBlockFormatDirectSpeakers` 有 `rtime`（DefaultParam）和 `duration`（OptParam），
语义与 Objects 块一致。当前 importer 完全忽略这两个字段，`SceneDirectSpeakersBlock`
也没有 `start_sample` / `end_sample`。所有 DS 块被当作全程静态处理，时变床层布局无法
正确还原。

---

## 3. AudioObject 级参数

| 参数 | libadm 建模 | `SceneObject` | importer |
|---|---|---|---|
| id / name | ✅ Required | ✅ | ✅ |
| start（含父子层级 BFS 累计） | ✅ DefaultParam | ✅ | ✅ |
| **duration**（对象有效时长上限） | ✅ Opt | ❌ | ❌ |
| **gain**（对象级增益，乘到所有块） | ✅ DefaultParam | ❌ | ❌ |
| **mute** | ✅ DefaultParam | ❌ | ❌ |
| headLocked | ✅ DefaultParam | ❌ | ❌ |
| **positionOffset**（polar / cartesian） | ✅ Opt | ❌ | ❌ |
| interact / disableDucking | ✅ Opt | ❌ | ❌ |
| audioObjectInteraction | ✅ Opt | ❌ | ❌ |
| dialogue / dialogueId | ✅ Opt | ❌ | ❌ |
| importance | ✅ Opt | ❌ | ❌ |
| audioObjectLabel | ✅ VectorParam | ❌ | ❌ |
| audioComplementaryObjectGroupLabel | ✅ VectorParam | ❌ | ❌ |

`gain` 和 `mute` 是 P1 级别缺口：BS.2076-2 第 11.1 节要求 AudioObject 的 `gain`
乘到该对象所有块的最终增益上；`mute=true` 的对象应产生全零输出，当前两者均被忽略。

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
| jumpPosition / 插值斜坡 | ❌ 未实现（VBAP / EAR 均已有） |
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
| **HRTF / SOFA binauraliser** | ❌ 未实现；`output_layout=binaural` 目前只是 stereo alias | 独立 binaural 后端或渲染后处理，读取 SOFA/HRTF 数据集 |
| **decorrelator / diffuse bus** | ❌ EAR 已计算 diffuse gains 但未渲染；VBAP 忽略 ADM `diffuse` | 优先补 EAR 的 BS.2127 去相关 FIR；VBAP 可另设扩散策略 |
| **reverb / room simulation** | ❌ 未实现 | 可作为可选后处理，不应默认改变 ADM 合规渲染结果 |
| **更完整 VBAP 布局表** | ⚠️ 仅硬编码少量布局 | 抽出布局注册表，覆盖更多 BS.2051 / CICP 常用布局 |
| **VBAP 2D / 3D 配置** | ⚠️ 自动按布局高度判断；无用户可见配置 | 显式能力报告和配置开关，避免 2D 无 spread 参数时行为不透明 |
| **VBAP 插值策略配置** | ⚠️ 支持 ADM `jumpPosition/interpolationLength`，默认 5ms；不可配置 | 暴露默认 ramp、clamp、瞬时切换等策略选项 |

### HRTF / SOFA binauraliser

`binaural` 目前不是 HRTF 渲染器：EAR 和 VBAP 都会把它映射到 `0+2+0`，
输出普通双声道扬声器布局。真正的 binauraliser 需要：

- SOFA 文件加载、HRTF 数据集选择和采样率匹配；
- 方位插值、距离策略、头外化相关补偿；
- 分块卷积和延迟管理；
- 与 Objects / DirectSpeakers / HOA decode 输出之间的清晰串接方式。

### decorrelator / diffuse

EAR 路径应优先实现 libear `designDecorrelators()` 对应的 diffuse bus，
这是当前 Objects 渲染最明确的合规缺口。VBAP 的 MDAP spread 只对应 extent 几何扩散，
不能替代 ADM `diffuse` 的去相关语义。

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

| 问题 | 影响 | 涉及文件 |
|---|---|---|
| diffuse bus 丢弃（EAR） | diffuse=1 静音；0<d<1 能量损失；需补去相关 FIR 和延迟补偿 | `ear_renderer.cpp`、DSP FIR 状态 |
| AudioObject.gain 未读 | 对象级增益控制失效 | `scene.h`、importer |
| AudioObject.mute 未读 | 静音对象仍正常渲染 | `scene.h`、importer、渲染器 |
| AudioObject.duration 未读 | 对象超时仍渲染 | `scene.h`、importer |
| DS 块无时间窗 | 时变床层全程叠加 | `scene.h`、importer |
| EAR 拒绝 Cartesian Objects | 文件级渲染失败 | `ear_renderer.cpp`（需坐标转换兜底） |

### P2 — libear 字段暴露但渲染实现缺失（传入会在运行时抛出）

| 问题 | libear 行为 | 涉及文件 |
|---|---|---|
| channelLock | flag=true 时 `not_implemented` | importer、scene.h、EAR |
| objectDivergence | divergence≠0 时 `not_implemented` | importer、scene.h、EAR |
| zoneExclusion | zones 非空时 `not_implemented` | importer、scene.h、EAR |
| screenRef | screenRef=true 时 `not_implemented` | importer、scene.h、EAR、programme 解析 |

对于 P2 的近期策略：在传入 libear 前做检查，遇到非零值记 warning 并静默降级
（置零 / 忽略），而非让整个渲染失败。这是防御性处理，不等于完整支持。

### P2b — importer 缺口（与 libear 支持无关）

| 问题 | 涉及文件 |
|---|---|
| DS 位置范围（azMin/Max 等） | `scene.h`、importer |
| DS CartesianSpeakerPosition | `scene.h`、importer |
| AudioObject.positionOffset | `scene.h`、importer、渲染器 |
| HOA jumpPosition / 插值 | `hoa_renderer.cpp` |
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
| HRTF / SOFA binauraliser | 真正的耳机双耳渲染；当前 `binaural` 只是 stereo alias |
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
