# adm_apple 后端实现说明（AUSpatialMixer）

> 状态：已实现（macOS-only）。本文记录 `adm_apple` 平台渲染后端的当前能力、ADM 语义映射、AUSpatialMixer 边界与后续缺口。
>
> 相关：ADR 0003（自有领域模型与后端边界）、ADR 0005（错误处理）、ADR 0007（C ABI）、`docs/architecture/CPP_ADM_PLATFORM_REWRITE.md`、`docs/architecture/ADM_FEATURE_COVERAGE.md`。

## 1. 定位与范围

`adm_apple` 是一个 **macOS-only 平台渲染后端**，以 Apple `AUSpatialMixer`（AudioToolbox，`kAudioUnitSubType_SpatialMixer`）实现 `IRenderer`，与 `adm_render_ear` / `adm_render_vbap` / `adm_render_hoa` / `adm_render_binaural` 平级。

当前支持：

- **双耳**：Headphones HRTF → 2ch（输出布局 `binaural`，CoreAudio 容器使用 `kAudioChannelLayoutTag_Binaural`）。
- **多声道扬声器**：VBAP → `5.1`、`7.1`、`5.1.2`、`5.1.4`、`7.1.4`、`9.1.6`、`22.2`。
- **输入内容**：Objects 与 DirectSpeakers；Objects 支持 position / gain / interpolation / objectDivergence；
  DirectSpeakers 支持 `label` 主机侧直达与 `position` AmbienceBed 空间化，并支持动态块目标。普通布局的
  LFE 保留 SpatialMixer 旁路，22.2 使用独立双 LFE side bus。
- **按需窗口渲染**：支持 `RenderPlan::render_window`，CLI `--start` / `--end` 不再需要先渲染完整时间线再裁切。
- **Apple binaural factory preset**：`--apple-spatial-preset headphone-default|headphone-movie` 映射到
  `kAudioUnitProperty_PresentPreset` 的 headphone media playback factory preset #1/#2。默认关闭，只对 Apple
  binaural 生效。
- **扬声器 rendering flags**：默认对每个输入 bus 写入
  `kAudioUnitProperty_SpatialMixerRenderingFlags=0`，避免 InterAuralDelay / DistanceAttenuation 额外改变
  ADM 对象的低中频和增益。`--apple-speaker-rendering-flags` 显式开启两个 Apple flags，用于兼容旧渲染。
- **DirectSpeakers 路由**：`--direct-speakers-routing auto|label|position`。Apple 扬声器的 `auto`
  解析为 `label`（这是相对旧版 AmbienceBed 默认的有意调整）；Apple binaural 的 `auto` 仍解析为
  `position`，显式 `label` 返回 unsupported。

不支持或暂不等价：

- HOA 输出：`supports_hoa=false`。
- diffuse：SpatialMixer 没有 ADM direct/diffuse 能量拆分与去相关器，`supports_diffuse=false`。
- channelLock：✅ 已实现（扬声器输出）。`prepare()` 从 `render_layouts::find_speaker_layout` 构建输出 speaker set，`render_common::apply_channel_lock` 把对象吸附到最近非-LFE 扬声器，`supports_channel_lock=true`。binaural 输出无离散扬声器，speaker set 为空 → channelLock 自然 drop。
- screen reference / screenLock / headLocked：离线路径不支持。
- extent：✅ 已实现。复用项目共享的 17 点 disk cloud（`render_common::extent_disk_cloud`，与 binaural / HOA 同源），把 width/height/depth 展开为多个相干点源 bus，`supports_spread=true`。**与 VBAP 的有意分歧**：cloud 是纯点源，对所有 CoreAudio layout（含 2D 的 5.1 / 7.1）都生效；VBAP 的 SAF 2D API 无 spread 参数，故其 2D 路径不 spread，而 SpatialMixer 无此限制，因此 `automatic` 在 2D 上也会 spread（横向宽对象正确铺开）。`--speaker-spread-mode` / `--binaural-spread-mode none` 强制点源。仍属 approximation（相干点云有梳状滤波，不与 libear spreadingPanner 逐测匹配）。
- transaural / BuiltIn / External 设备串扰消除：设备绑定且更适合实时预览，不作为离线交付路径。

**定位（重要）**：AUSpatialMixer 是 Apple 黑盒，HRTF / VBAP 细节和版本行为不可 bit-exact。`adm_apple` 是平台风味渲染器，适合 Apple 原生生态预览与交付验证；规范级对照仍应使用 libear / SAF / binaural 等可审计后端。

## 2. 模块边界

- 模块：`src/adm_apple/`，target `mr_adm_apple`，别名 `MacinRender::ADMRenderApple`。
- 工厂：`create_apple_renderer()`，公共边界为 `include/adm/render_apple.h`。
- Apple 框架类型仅出现在 `src/adm_audio/` 与 `src/adm_apple/` 内部；`include/adm/*` 不暴露 AudioToolbox / CoreAudioTypes。
- `render_service.cpp` 在 `__APPLE__` 下 dispatch `RendererSelection::apple`；非 Apple 平台保持 unsupported。
- CLI 使用 `--renderer apple` 选择该后端。

后端不重解析 ADM，只消费 `RenderService` 生成的 `RenderPlan::scene` / `AdmScene`，并复用共享语义预处理函数：

- `scene_position_to_polar()`
- `render_common::prepare_object_block()`
- `render_common::direct_speakers_block_is_lfe()`
- `expand_object_divergence()` 等由共享路径间接完成

## 3. IRenderer 结构

`IPreparedRender` 必须不可变、可共享；AUSpatialMixer 实例有内部 DSP 状态，所以不能放进 prepared。Apple 后端采用“不可变配方 + 每次 render_window 新建 AU”的结构：

```cpp
struct OutputProfile {
    uint16_t channels;
    bool binaural;
    AudioChannelLayoutTag layout_tag;
    std::string_view writer_layout;
};

struct BusEvent {
    uint64_t start_sample;
    uint64_t end_sample;
    float azimuth;   // SpatialMixer 约定，已从 ADM 符号翻转
    float elevation;
    float distance;
    float gain;      // linear
    std::optional<std::size_t> direct_output_channel;
};

struct BusPlan {
    uint16_t source_channel;
    UInt32 source_mode; // PointSource / AmbienceBed / Bypass
    bool is_lfe;
    bool is_direct;
    std::vector<BusEvent> events;
};

struct ApplePrepared final : IPreparedRender {
    OutputProfile profile;
    std::vector<BusPlan> buses;        // SpatialMixer inputs
    std::vector<BusPlan> direct_buses; // label one-hot side buses
    std::vector<BusPlan> lfe_buses;    // 22.2 side buses
    LfeRoutingPlan lfe_routing;
};
```

`prepare()` 解析输出布局、路由模式、分配 bus、展开对象语义并生成事件表。label DirectSpeakers 从
空间 bus 拆为 direct side bus；22.2 LFE 也拆为专用 side bus 并完成单/双语义 LFE 校验。
`render_window()` 仅在存在空间 bus 时创建 SpatialMixer；AU 输出后、写出和计量前，主机逐样本混入
direct bus 与 LFE bus。纯 label bed 或纯 22.2 LFE 场景只创建 reader，不创建 AU。

## 4. 渲染管线

### 4.1 拉模型桥接

AUSpatialMixer 通过 input render callback 拉取每条空间 bus 的 PCM。项目渲染路径先按 block 从 BW64
读入 interleaved staging buffer；每条空间 bus 的 callback 从 staging buffer 拷贝自己绑定的源通道。
输出端由 `AudioUnitRender` 驱动；随后 direct/LFE side bus 从同一 staging buffer 按逐样本事件与实时
gain envelope 混入，最后写入项目统一的 `audio::WriterHandle`。所有 side-bus 游标、斜坡和 envelope
均在 prepare/stream 构造阶段预分配，实时回调不分配内存。

### 4.2 按需窗口

当 `RenderPlan::render_window` 存在时，Apple 后端：

1. 将源 reader seek 到请求窗口前一个对齐 render block（如果窗口起点足够靠后）。
2. 预滚该 block 来更新 SpatialMixer 内部状态。
3. 仍使用绝对 sample time 匹配 ADM 事件。
4. 只写出请求窗口内的帧。

这与其他支持 `supports_render_window=true` 的后端对齐，避免 `--start` / `--end` 先全量渲染再裁切。由于 SpatialMixer 是黑盒状态机，pre-roll 是保守折中，不承诺与全量渲染逐样本一致。

### 4.3 响度与 True Peak 测量

Apple 后端和其他主要后端一样在渲染过程中内联测量响度 / True Peak。当前实现使用双输出缓冲加 `render_common::SerialWorker` 异步调用 `ebur128_add_frames_float()`，使写文件 / AU render 与 meter 更新重叠。

在同一 release 构建、同一 ADM BWF、float32 WAV 输出上的基线测速：

| 输出 | 同步测量均值 | 异步测量均值 | 改善 |
|---|---:|---:|---:|
| Apple 22.2 | 9.297s | 7.357s | 20.9% |
| Apple binaural | 5.013s | 4.387s | 12.5% |

测得 LUFS / True Peak 保持一致；测速仅用于本地性能基线，不代表跨机器稳定结果。

## 5. ADM 语义映射

### 5.1 原生直映

- Object position → SpatialMixer Azimuth / Elevation / Distance。
- Object 与 position DirectSpeakers gain → `kSpatialMixerParam_Gain`（linear → dB，静音落到 -120 dB）；
  label DirectSpeakers gain 在主机 direct side bus 以线性标量逐样本应用。
- Object 插值 / 平滑 → 按事件块更新参数；SpatialMixer 自身会对控制变化做平滑。项目级 `--object-smoothing-frames` / `RenderOptions::object_smoothing_frames` 当前不影响 Apple 后端；该参数只由 EAR / VBAP / HOA / binaural 等自有控制率路径消费。
- Apple binaural factory preset → 在 AudioUnit 创建后、输出格式 / bus 算法 / source mode / ADM 参数写入前应用
  `kAudioUnitProperty_PresentPreset`。这是刻意顺序：PresentPreset 会重置若干 SpatialMixer 参数，后端随后重新写入
  ADM 驱动配置，避免预设把床层或对象声像重置到中心。preset 模式不等同于 `ReverbRoomType`，也不对扬声器布局开放。
- DirectSpeakers `position` → `AmbienceBed` 远场床层，保留旧版路径；忽略非 LFE 标签，使用标称坐标、
  零扩散、无插值。缺坐标使用 `(0°,0°)` 并 warning。
- DirectSpeakers `label` → 共享路由器先精确匹配输出标签，再尝试 RoomCentric/DAW 别名（如
  `L` → `M+030`）；命中后绕过 AU one-hot 写入目标槽位。未命中时进入 AmbienceBed 空间化并
  warning：已知 BS.2051 / 别名标签使用标签方向，否则使用 ADM 标称坐标；需要坐标回退但缺坐标时
  使用 `(0°,0°)`。
- 非 22.2 LFE → `Bypass`，mono input bus 标为 `kAudioChannelLabel_LFEScreen`，保持既有行为。
- 22.2 LFE → AU 外 side bus：`direct` 将 LFE1/LFE2 严格独立送入 ch3/ch9；`split-power` 将单一语义 LFE 以 `sqrt(0.5)` 同时送入两路。事件 gain、对象 gain 与实时 override 采样斜坡均在 side bus 混音中生效。

### 5.2 项目层预处理

- Cartesian position → `scene_position_to_polar()`。
- objectDivergence → 共享语义路径展开为并行点源 bus。
- screenRef warning / unsupported 降级 → 共享 `prepare_object_block()` 路径处理。
- LFE 识别 → 共享 `render_common::direct_speakers_lfe_target()`：`LFE2` / `LFER` 为第二路，其他 LFE alias 与仅 lowPass 块为第一路；拓扑判定只读元数据，不依赖样本能量、gain 或 mute。
- DirectSpeakers 标签/方向回退 → 共享
  `direct_speaker_index_for_labels()` / `direct_speaker_position_for_labels()`，与 SAF 使用同一别名及
  fallback 规则。

### 5.3 不支持或降级

- diffuse：drop；不使用 SpatialMixer reverb 伪装 ADM diffuse。
- channelLock：✅ 扬声器输出已启用（speaker set 由 render_layouts 构建，apply_channel_lock 吸附最近扬声器）；binaural 无离散扬声器仍 drop。
- extent：✅ 已实现为 approximation。共享 `render_common::extent_disk_cloud`（17 点 disk cloud：中心 + 内环 8 + 外环 8，权重 Σ=1）把每个 divergence 源展开为相干点源 bus；depth 已并入半角映射（depth*20）。受 `--speaker-spread-mode` / `--binaural-spread-mode` 门控；对所有 layout（含 2D）生效（见 §1，与 VBAP 2D 不 spread 的差异）。
- screenLock / headLocked / headphoneVirtualise / importance / dialogue：不参与 Apple 离线渲染数学。

## 6. 坐标系

| 系统 | 约定 |
|---|---|
| 本项目 ADM 极坐标 | azimuth +ve=左、elevation +ve=上、front=0 |
| Apple SpatialMixer | azimuth +ve=右、elevation +ve=上、distance=米 |

核心映射：

```text
sm_azimuth   = -adm_azimuth
sm_elevation =  adm_elevation  // clamp 到 [-90, 90]
sm_distance  =  max(adm_distance, 1e-3)
```

符号翻转已有 smoke test 覆盖：ADM 左侧对象应产生左声道更高能量，防止整声场左右镜像。

## 7. 输出布局

| CLI layout | 显示名 | 声道数 | CoreAudio tag |
|---|---|---:|---|
| `binaural` / `0+2+0` | binaural | 2 | writer 使用 `kAudioChannelLayoutTag_Binaural` |
| `0+5+0` | 5.1 | 6 | MPEG 5.1 A |
| `wav71` | 7.1 | 8 | WAVE 7.1 |
| `2+5+0` | 5.1.2 | 8 | Atmos 5.1.2 |
| `4+5+0` | 5.1.4 | 10 | Atmos 5.1.4 |
| `4+7+0` | 7.1.4 | 12 | Atmos 7.1.4 |
| `9.1.6` | 9.1.6 | 16 | Atmos 9.1.6 |
| `9+10+3` | 22.2 | 24 | CICP 13 |

双耳 tag 只在 CoreAudio 容器（CAF/APAC）中保留。WAV / FLAC 没有通用双耳标签，只能保留 2ch PCM 本身。

## 8. CapabilityReport

```text
backend_name = "apple"
supported_layouts = [
  binaural,
  0+5+0,
  wav71,
  2+5+0,
  4+5+0,
  4+7+0,
  9.1.6,
  9+10+3
]
supports_objects           = true
supports_direct_speakers   = true
supports_hoa               = false
supports_object_divergence = true
supports_channel_lock      = true   // 扬声器输出吸附最近扬声器；binaural drop
supports_diffuse           = false
supports_screen_ref        = false
supports_render_window     = true
```

## 9. 测试与回归

Apple smoke tests 覆盖：

- capability report 与支持布局。
- AUSpatialMixer 是否可创建；Linux / 非 Apple 自动 skip。
- 坐标符号：ADM 左侧对象不能渲染成右侧更强。
- gain dB floor：线性 0 / 极低增益不会变成 unity。
- 7.1.4 / 22.2 等布局声道数与写出。
- binaural 输出的 CoreAudio tag 归一化。
- LFE 标签识别、普通布局旁路，以及 22.2 direct/split-power side-bus 路由。
- 22.2 纯 LFE 无 AU、ch0 无泄漏、stream/offline 一致与实时 override gain ramp。
- DirectSpeakers `auto/label/position`、`L` → 22.2 ch6、非 LFE 的双 LFE 零泄漏，以及 Apple binaural
  `auto/position` 接受、显式 `label` 拒绝。
- 纯 label bed 无空间 bus；direct side bus 动态块目标、seek、loop reset、实时 gain/mute 与
  offline/stream bit-identical。
- render window 输出帧数。

不做 bit-exact golden。若后续增加 golden，应按 macOS / SDK 版本钉住容差。

## 10. 后续事项

- extent dedup：✅ 已完成（部分）。binaural / HOA / apple 共用 `render_common::k_extent_disk_samples` 采样表 + `extent_disk_radii` 半角映射（逐字节相同的部分）；几何环（normalize / direction / 输出）因 binaural 与 HOA 的实现真不同（双精度 vs 单精度、polar vs cartesian 分支、az/el vs SH 编码）无法合并，各保留本地。bit-exact 经 binaural / hoa / render_trim fixture 验证。
- bed / LFE 扬声器路由：✅ 已验证。7.1.4/22.2 的默认非 LFE bed 使用 label direct side bus；显式
  `position` 继续使用 AmbienceBed。普通布局 LFE 继续 Bypass；22.2 使用标准 CICP 13 与独立 side bus，
  覆盖 LFE1→ch3、LFE2→ch9、单 LFE 等功率复制、原生双 LFE 拒绝 split 及无全频声道泄漏。
- diffuse：如要做近似，必须先定义可解释的能量 / 去相关策略，不能简单使用 SpatialMixer reverb 代替。
- realtime preview：复用 prepared 配方与 AU 参数映射，另建实时驱动循环；head tracking / transaural 仅适合该方向。
