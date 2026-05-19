# ADM 特性覆盖审计

本文档记录 MacinRender ADM Core 对 BS.2076 / BS.2127 特性的当前覆盖状态，
以及与 libadm / libear 能力边界之间的差距。

最后更新：2026-05-19

---

## 图例

| 符号 | 含义 |
|---|---|
| ✅ | 已实现并经过测试 |
| ⚠️ | 部分实现或有已知缺陷 |
| ❌ | 未实现 |
| N/A | 该层不适用 |
| — | 第三方库本身不支持 |

---

## 1. Objects 块参数（`AudioBlockFormatObjects`）

| 参数 | libadm | libear `ObjectsTypeMetadata` | `SceneObjectBlock` | importer | EAR 渲染器 | VBAP 渲染器 |
|---|---|---|---|---|---|---|
| position（polar / cartesian） | ✅ | ✅ | ✅ | ✅ | ✅ | ✅ |
| gain | ✅ DefaultParam | ✅ | ✅ | ✅ | ✅ | ✅ |
| diffuse | ✅ DefaultParam | ✅ | ✅ | ✅ | ⚠️ 见注① | N/A |
| width / height / depth | ✅ DefaultParam | ✅ | ✅ | ✅ | ✅ | ✅ MDAP |
| rtime / duration | ✅ Default/Opt | N/A | start/end_sample | ✅ | ✅ | ✅ |
| jumpPosition + interpolationLength | ✅ DefaultParam | N/A | ✅ | ✅ | ✅ | ✅ |
| **channelLock** | ✅ DefaultParam | ✅ | ❌ | ❌ | ❌ | ❌ |
| **objectDivergence** | ✅ DefaultParam | ✅ | ❌ | ❌ | ❌ | ❌ |
| **zoneExclusion** | ✅ | ✅ | ❌ | ❌ | ❌ | ❌ |
| **screenRef** | ✅ DefaultParam | ✅ | ❌ | ❌ | ❌ | ❌ |
| importance | ✅ DefaultParam | — 库不支持 | ❌ | ❌ | N/A | N/A |
| headLocked | ✅ DefaultParam | — 库不支持 | ❌ | ❌ | N/A | N/A |
| headphoneVirtualise | ✅ DefaultParam | — | ❌ | ❌ | N/A | N/A |
| screenEdgeLock | ✅ Opt（在 position 内） | ✅ in Position | ❌ | ❌ | ❌ | ❌ |

### 注①：diffuse bus 未使用（严重合规缺陷）

`GainCalculatorObjects::calculate()` 同时输出 `directGains` 和 `diffuseGains`。
标准要求 diffuse bus 经过 `designDecorrelators()` 提供的 FIR 滤波器处理后再与 direct bus
混合，补偿延迟由 `decorrelatorCompensationDelay()` 给出。

当前实现只取 `directGains`，`diffuseGains` 被丢弃：

```cpp
std::vector<double> diffuse_gains(num_out, 0.0);
objects_calc.calculate(meta, bg.gains, diffuse_gains);
// diffuse_gains 未使用
```

后果：`diffuse > 0` 的对象（弥漫声源）去相关特性完全缺失，在多声道输出上听起来像
点源而非弥散声场。这是 EAR 渲染器目前最显著的 BS.2127 合规偏差。

VBAP 渲染器通过 SAF spreader（`--diffuse-mode spreader`）提供类似效果，但仅限于
`extent ≈ 1` 的情况，且与标准 diffuse bus 路径无关。

---

## 2. DirectSpeakers 块参数（`AudioBlockFormatDirectSpeakers`）

| 参数 | libadm | libear `DirectSpeakersTypeMetadata` | `SceneDirectSpeakersBlock` | importer |
|---|---|---|---|---|
| speakerLabels | ✅ | ✅ | ✅ | ✅ |
| SphericalSpeakerPosition（nominal） | ✅ | ✅ | ✅ | ✅ |
| **position ranges（azMin/Max, elMin/Max, distMin/Max）** | ✅ | ✅ in PolarSpeakerPosition | ❌ | ❌ |
| **CartesianSpeakerPosition** | ✅ | ✅ | ❌ | ❌ |
| gain | ✅ DefaultParam | 手动应用 | ✅ | ✅ |
| **rtime / duration** | ✅ Default/Opt | N/A | ❌ | ❌ |
| **channelFrequency**（在 AudioChannelFormat） | ✅ | ✅ | ❌ | ❌ |
| importance / headLocked | ✅ DefaultParam | — | ❌ | ❌ |

### DS 时域块（重要缺口）

`AudioBlockFormatDirectSpeakers` 有 `rtime`（DefaultParam）和 `duration`（OptParam），
语义与 Objects 块完全一致。当前 importer 的 `append_direct_speakers_blocks_from_cf()`
完全不读这两个字段，`SceneDirectSpeakersBlock` 也没有 `start_sample` / `end_sample`。

后果：含有时变 DS 块的文件（例如床层换布局）所有 DS 块会全程叠加，而非按时间窗激活。

---

## 3. AudioObject 级参数

| 参数 | libadm | `SceneObject` / `SceneTrackRef` | importer |
|---|---|---|---|
| id / name | ✅ Required | ✅ | ✅ |
| start（含父子层级累计） | ✅ DefaultParam | ✅ | ✅ |
| **duration**（对象有效时长上限） | ✅ Opt | ❌ | ❌ |
| **gain**（对象级增益，叠加到所有块） | ✅ DefaultParam | ❌ | ❌ |
| **mute** | ✅ DefaultParam | ❌ | ❌ |
| headLocked | ✅ DefaultParam | ❌ | ❌ |
| **positionOffset**（polar / cartesian） | ✅ Opt | ❌ | ❌ |
| interact | ✅ Opt | ❌ | ❌ |
| disableDucking | ✅ Opt | ❌ | ❌ |
| audioObjectInteraction | ✅ Opt | ❌ | ❌ |
| dialogue / dialogueId | ✅ Opt | ❌ | ❌ |

### AudioObject.gain（重要缺口）

BS.2076-2 第 11.1 节规定 AudioObject 的 `gain` 属性应乘到该对象所有 AudioTrackUid
所有块的增益上。当前实现完全忽略这一层，导致对象级增益控制失效。这在广播制作文件中
很常见（人声/效果的相对电平往往在对象层设置）。

### AudioObject.mute / duration

`mute=true` 的对象应产生全零输出；当前仍会正常渲染。
`duration` 限制了对象的有效时间窗——超过此时长的块理应静音，当前没有截断。

---

## 4. AudioProgramme 级参数

| 参数 | libadm | `SceneProgramme` | importer |
|---|---|---|---|
| id / name | ✅ Required | ✅ | ✅ |
| **audioProgrammeReferenceScreen** | ✅ Opt | ❌ | ❌ |
| loudnessMetadata（内嵌响度） | ✅ VectorParam | ❌ | ❌ |
| start / end | ✅ Default/Opt | ❌ | ❌ |
| maxDuckingDepth | ✅ Opt | ❌ | ❌ |

`audioProgrammeReferenceScreen` 是实现 `screenRef` 的前提：只有从这里读到屏幕参数，
才能把它填入 `ear::ObjectsTypeMetadata::referenceScreen`，libear 才能正确计算
屏幕相对位置的增益。

---

## 5. HOA 渲染器特有缺口

| 缺口 | 说明 |
|---|---|
| 时变块 | 静态增益矩阵，无 start/end_sample 门控 |
| jumpPosition / 插值 | 未实现（VBAP/EAR 均已支持） |
| diffuse | HOA 编码器未处理 diffuse 参数 |
| libear `GainCalculatorHOA` | 当前使用手写 SH3 编码器（Objects→HOA3）。libear 的 HOA calculator 方向相反（HOA→扬声器），与当前用途不冲突，但 HOA decode 路径未被利用 |
| normalization / nfcRefDist | `AudioBlockFormatHoa` 有这两个字段，手写编码器固定使用 SN3D，未读 ADM 值 |

---

## 6. 优先级分类

### P1 — 影响渲染正确性（当前输出有可测量偏差）

| 问题 | 文件 | 修复复杂度 |
|---|---|---|
| diffuse bus 丢弃（EAR） | `ear_renderer.cpp` | 高（需要 FIR decorrelator 路径） |
| AudioObject.gain 未读 | `scene_importer.cpp`、`scene.h` | 低 |
| AudioObject.mute 未读 | `scene_importer.cpp`、渲染器 | 低 |
| AudioObject.duration 未读 | `scene_importer.cpp` | 低 |
| DS 块无时间窗 | `scene_importer.cpp`、`scene.h` | 中 |

### P2 — libear 已全面支持，缺口在 importer 和 scene.h

| 问题 | 文件 | 修复复杂度 |
|---|---|---|
| channelLock | `scene.h`、importer、EAR | 低 |
| objectDivergence | `scene.h`、importer、EAR/VBAP | 低 |
| zoneExclusion | `scene.h`、importer、EAR | 中（结构嵌套较深） |
| DS 位置范围（azMin/Max 等） | `scene.h`、importer、EAR | 中 |
| DS CartesianSpeakerPosition | `scene.h`、importer | 低 |
| AudioObject.positionOffset | `scene.h`、importer、渲染器 | 中 |

### P3 — 功能完整性，原版也未实现

| 问题 | 说明 |
|---|---|
| screenRef + referenceScreen | 需要从 AudioProgramme 解析屏幕参数后透传到 libear |
| AudioProgramme.loudnessMetadata | 读取文件内嵌响度元数据（区别于主动 EBU R128 测量） |
| AudioProgramme start / end | 节目时间窗（超出范围的 object 静音） |
| HOA 时变块 + 插值 | 与 VBAP/EAR 对齐 |
| HOA normalization 读取 | 从 `AudioBlockFormatHoa` 读 normalization/nfcRefDist |

### 暂不实现（平台 / 交互策略相关）

| 参数 | 原因 |
|---|---|
| headLocked | libear 不支持；是平台渲染器（SpatialMixer）职责 |
| headphoneVirtualise | 播放设备控制，不适用于离线渲染 |
| importance | 资源调度策略，不影响增益数学 |
| interact / audioObjectInteraction | 交互层元数据，离线渲染不适用 |
| dialogue / disableDucking | 条件混音控制，超出当前范围 |

---

## 附录：libear ObjectsTypeMetadata 完整字段

```cpp
struct ObjectsTypeMetadata {
    Position position;          // PolarPosition or CartesianPosition
    double width{0.0};
    double height{0.0};
    double depth{0.0};
    bool cartesian{false};
    double gain{1.0};
    double diffuse{0.0};
    ChannelLock channelLock;             // flag + optional maxDistance
    ObjectDivergence objectDivergence;   // PolarObjectDivergence or CartesianObjectDivergence
    ZoneExclusion zoneExclusion;         // vector<ExclusionZone>
    bool screenRef{false};
    Screen referenceScreen;              // from audioProgramme
};
```

## 附录：libear DirectSpeakersTypeMetadata 完整字段

```cpp
struct DirectSpeakersTypeMetadata {
    std::vector<std::string> speakerLabels;
    SpeakerPosition position;            // PolarSpeakerPosition（含 min/max 范围）or Cartesian
    ChannelFrequency channelFrequency;   // lowPass / highPass
    boost::optional<std::string> audioPackFormatID;
};
```
