# adm_apple 后端实现说明（AUSpatialMixer）

> 状态：已实现（macOS-only）。本文记录 `adm_apple` 平台渲染后端的当前能力、ADM 语义映射、AUSpatialMixer 边界与后续缺口。
>
> 相关：ADR 0003（自有领域模型与后端边界）、ADR 0005（错误处理）、ADR 0007（C ABI）、`docs/architecture/CPP_ADM_PLATFORM_REWRITE.md`、`docs/architecture/ADM_FEATURE_COVERAGE.md`。

## 1. 定位与范围

`adm_apple` 是一个 **macOS-only 平台渲染后端**，以 Apple `AUSpatialMixer`（AudioToolbox，`kAudioUnitSubType_SpatialMixer`）实现 `IRenderer`，与 `adm_render_ear` / `adm_render_vbap` / `adm_render_hoa` / `adm_render_binaural` 平级。

当前支持：

- **双耳**：Headphones HRTF → 2ch（输出布局 `binaural`，CoreAudio 容器使用 `kAudioChannelLayoutTag_Binaural`）。
- **多声道扬声器**：VBAP → `5.1`、`7.1`、`5.1.2`、`5.1.4`、`7.1.4`、`9.1.6`、`22.2`。
- **输入内容**：Objects 与 DirectSpeakers；Objects 支持 position / gain / interpolation / objectDivergence；DirectSpeakers 支持静态方位与 LFE 旁路。
- **按需窗口渲染**：支持 `RenderPlan::render_window`，CLI `--start` / `--end` 不再需要先渲染完整时间线再裁切。

不支持或暂不等价：

- HOA 输出：`supports_hoa=false`。
- diffuse：SpatialMixer 没有 ADM direct/diffuse 能量拆分与去相关器，`supports_diffuse=false`。
- channelLock：当前 Apple 后端没有构建输出 speaker set，相关语义在预处理阶段降级，`supports_channel_lock=false`。
- screen reference / screenLock / headLocked：离线路径不支持。
- extent：当前没有 Apple 后端专用的 extent 点云铺开实现，不应标成已支持。
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
};

struct BusPlan {
    uint16_t source_channel;
    UInt32 source_mode; // PointSource / AmbienceBed / Bypass
    bool is_lfe;
    std::vector<BusEvent> events;
};

struct ApplePrepared final : IPreparedRender {
    OutputProfile profile;
    std::vector<BusPlan> buses;
};
```

`prepare()` 只解析输出布局、分配 bus、展开对象语义并生成事件表。`render_window()` 创建 SpatialMixer，设置输出布局、输入 bus、source mode、spatialization algorithm、LFE layout 与 render callback，然后拉取输出并写入目标文件。

## 4. 渲染管线

### 4.1 拉模型桥接

AUSpatialMixer 通过 input render callback 拉取每条 input bus 的 PCM。项目渲染路径先按 block 从 BW64 读入 interleaved staging buffer；每条 bus 的 callback 从 staging buffer 拷贝自己绑定的源通道。输出端由 `AudioUnitRender` 驱动，写入项目统一的 `audio::WriterHandle`。

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
- Object / DirectSpeakers gain → `kSpatialMixerParam_Gain`（linear → dB，静音落到 -120 dB）。
- Object 插值 → 按事件块更新参数；SpatialMixer 自身会对控制变化做平滑。
- DirectSpeakers → `AmbienceBed` 远场床层。
- LFE → `Bypass`，并把 mono input bus 标为 `kAudioChannelLabel_LFEScreen`，不参与空间化。

### 5.2 项目层预处理

- Cartesian position → `scene_position_to_polar()`。
- objectDivergence → 共享语义路径展开为并行点源 bus。
- screenRef warning / unsupported 降级 → 共享 `prepare_object_block()` 路径处理。
- LFE 识别 → 共享 `render_common::direct_speakers_block_is_lfe()`，同时识别 `channelFrequency.lowPass` 和 `RC_LFE` / `R-LFE` / `LFE1` / `Subwoofer` 等 LFE 标签。

### 5.3 不支持或降级

- diffuse：drop；不使用 SpatialMixer reverb 伪装 ADM diffuse。
- channelLock：当前无输出 speaker set，drop。
- extent：尚未实现后端专用铺开。后续可考虑复用 binaural spreader 几何或构建与输出布局相关的点云，但需要明确标注为 approximation。
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
supports_channel_lock      = false
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
- LFE 标签识别与 LFE bus 旁路配置。
- render window 输出帧数。

不做 bit-exact golden。若后续增加 golden，应按 macOS / SDK 版本钉住容差。

## 10. 后续事项

- extent：设计 Apple 专用 spread approximation，并用 semantic report 验证 effective metadata。
- channelLock：为扬声器输出构建 Apple 输出 speaker set 后再启用。
- diffuse：如要做近似，必须先定义可解释的能量 / 去相关策略，不能简单使用 SpatialMixer reverb 代替。
- realtime preview：复用 prepared 配方与 AU 参数映射，另建实时驱动循环；head tracking / transaural 仅适合该方向。
