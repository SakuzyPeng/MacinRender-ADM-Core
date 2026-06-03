# adm_apple 后端设计草案（AUSpatialMixer）

> 状态：草案（design draft）。本文定义 `adm_apple` 平台渲染后端的目标、边界、`IRenderer` 落地、ADM 语义映射、坐标系转换与测试策略。落地前的所有能力数据均在 macOS SDK 26.5 上实测确认。
>
> 相关：ADR 0003（自有领域模型与后端边界）、ADR 0005（错误处理）、ADR 0007（C ABI）、`docs/architecture/CPP_ADM_PLATFORM_REWRITE.md`、`docs/architecture/ADM_FEATURE_COVERAGE.md`。

## 1. 目标与范围

`adm_apple` 是一个 **macOS-only 平台渲染后端**，以 Apple `AUSpatialMixer`（`kAudioUnitSubType_SpatialMixer = '3dem'`，AudioToolbox）实现 `IRenderer`，与 `adm_render_ear` / `adm_render_vbap` / `adm_render_hoa` / `adm_render_binaural` 平级（ADR 0003）。

**首版范围**：

- **双耳**：Headphones HRTF → 2ch（输出布局 `binaural`，容器 tag `kAudioChannelLayoutTag_Binaural`）。
- **多声道扬声器**：VBAP / SoundField → `5.1.4` / `7.1.4` 等（复用现有 Atmos 布局 tag）。
- **不做** transaural（BuiltIn/External 机身扬声器串扰消除）：设备绑定、不可作为交付文件，只在实时播放有意义（见 §8）。
- **不做** HOA 输出（`supports_hoa=false`）。

**用途边界**：离线文件渲染为主，**架构为未来 GUI 实时预览预留**——AU 配置与 ADM→参数映射必须与离线拉循环解耦（见 §4），实时路径可复用前者。head-tracking / headLocked 不在离线输出范围，但接口不堵死。

**定位（重要）**：AUSpatialMixer 是黑盒，Apple 控制 HRTF / 声像数学，**无法 bit-exact**，且跨 macOS 版本 HRTF 库可变。`adm_apple` 是“平台风味”渲染器，面向 Apple 原生预览与交付，**不取代** libear/binaural 做规范级渲染。交付与回归对照仍走 EAR/binaural。

## 2. 边界（ADR 0003）

- 新模块 `src/adm_apple/`，CMake target `mr_adm_apple`，别名 `MacinRender::ADMRenderApple`。
- **Apple 框架（AudioToolbox / CoreAudioTypes）只允许出现在 `src/adm_audio/` 与 `src/adm_apple/` 内部**（CLAUDE.md 既有约定）。`include/adm/*` 公共头不得出现任何 Apple 类型。
- 后端**不重解析 ADM**：只消费 `RenderService` 填好的 `RenderPlan::scene`（`AdmScene`），复用 `scene.h` 的纯函数（`expand_object_divergence` / `apply_channel_lock` / `apply_position_offset` / `scene_position_to_polar`）。
- 错误经 `mradm::Result<T>` 返回；内部可 `throw`，但不得跨公共边界（ADR 0005）。

## 3. 模块与构建接入

**接入点现状**：`RendererSelection::apple` 枚举、CLI `--renderer apple` 解析与校验、`renderer_name()` **均已就绪**。当前 `--renderer apple` 可解析，但 `render_service.cpp` 的 dispatch 缺 `apple` 分支，落入 `else` 返回 `unsupported`（"not available in this build"）。

**需新增**：

1. `src/adm_apple/spatial_mixer_renderer.{h,cpp}` + `create_apple_renderer()` 工厂（仿 `create_binaural_renderer()`）。
2. CMakeLists.txt：`add_library(mr_adm_apple)` + 别名；`target_link_libraries(... PRIVATE libbw64 ebur128 MacinRender::ADMAudio MacinRender::ADMRenderCommon ${MR_ADM_AUDIOTOOLBOX_FW})`；仅 `if(APPLE)` 编译。
3. `render_service.cpp` dispatch 增加 `else if (sel == RendererSelection::apple) renderer = create_apple_renderer();`，并以 `#ifdef __APPLE__` 守护；非 Apple 平台保持现有 `unsupported` 行为。
4. 构建开关（暂定）：跟随 APAC——macOS 始终编译、Linux 自动 skip、**不加 flag**。若后续需要可引入 `MR_ADM_ENABLE_APPLE`。

**离线机制**：裸 `AudioUnit` + `AudioUnitRender` 拉（已实测可离线运行、非废弃 API）。**不使用**旧实现的 `AUGraph`（已废弃）。

## 4. IRenderer 落地

`IPreparedRender` 契约要求**不可变、可共享、无 per-window 可变状态**（一个 `PreviewSession` 跨窗口复用）。AUSpatialMixer 实例带内部 DSP 缓冲，是可变体——**配置好的 AU 不能进 prepared**。分层：

```cpp
// 不可变“渲染配方”，跨 window 复用。无 Apple 类型泄漏到公共头：本结构定义在 .cpp 内部。
struct ApplePrepared final : IPreparedRender {
    struct BusConfig {
        uint32_t bus;                 // 动态分配的输入 element 索引
        uint16_t source_channel;      // BWF 源通道（拉输入用）
        uint32_t spatialization_alg;  // HRTFHQ / VBAP / SoundField / UseOutputType
        uint32_t source_mode;         // PointSource / AmbienceBed / Bypass
        bool is_lfe;
        // 静态位置（bed）或位置时间线引用（object）
        std::optional<StaticDir> static_dir;        // bed：固定 az/el/dist
        std::optional<size_t> object_block_ref;     // object：指向 scene 的 block 时间线
        float gain;
    };
    OutputProfile output;             // 双耳(Binaural)或扬声器布局 + 算法/OutputType
    std::vector<BusConfig> buses;     // divergence/extent 已展开
    // ... 由 scene + layout + options 推导，全部 immutable
};
```

- **`capabilities()`**：返回 §10 的 `CapabilityReport`。
- **`prepare(plan, logs)`**：从 `plan.scene` + `plan.output_layout` + options 推导 `ApplePrepared`——分配 bus、展开 divergence/extent、解析 channelLock、确定每 bus 算法/sourceMode/静态位置。**不创建 AU 实例**。
- **`render_window(prepared, plan, ...)`**：downcast `ApplePrepared` → 创建 AUSpatialMixer 实例 → 套用配方（逐 bus 设属性/参数）→ 打开 BW64 reader → 按 render quantum 拉 + 写输出（经 `MacinRender::ADMAudio` writer）→ 销毁 AU。

此分层与 binaural 后端（prepared 存 HRTF 表、render_window 建 OLA/spreader）同构，且**天然把 AU 配置与离线拉循环解耦**，满足“为实时预览预留”。

## 5. 渲染管线

### 5.1 输入“拉模型”桥接

AUSpatialMixer 经 render callback 拉输入，而项目后端是“自己读帧”。桥接：`render_window` 开 BW64 reader 顺序读块到暂存缓冲，每条输入 bus 挂一个**平凡 callback**，把已读好的对应源通道块递给 AU。`AudioUnitRender` 在输出端驱动整链。

### 5.2 bus 分配

实测 AUSpatialMixer 默认 32 输入 element，可提至 **≥1024**（无实际约束）。

- 基础：每个 object 源通道、每条 bed 通道各占一条 bus。
- **膨胀**：divergence 展开为 3 源（占 3 bus）、extent 展开为 N 个铺开点源（占 N bus）。所需 bus 数 **超过源声道数** → 动态分配，按场景最大并发分解源数预留（仍远低于 1024）。

### 5.3 Object vs Bed（两套处理，对齐旧实现）

| | Object | Bed / DirectSpeakers |
|---|---|---|
| SourceMode | `PointSource`（含距离建模） | `AmbienceBed`（远场，无近场/in-head） |
| 位置 | 动态，逐 block 随时间 | 静态扬声器标称位置 |
| LFE | — | `Bypass` + `LFEScreen` 声道布局，不空间化 |

项目 `scene.h` 已天然二分 `SceneObjectBlock` vs `SceneDirectSpeakersBlock`，后端按 block 类型 dispatch；`RenderService` 已填好，无需自行辨别。ADM 理论上 DirectSpeakers 可时变，旧实现一律静态——首版对齐旧静态行为，动态 bed 留作可选增强。

## 6. ADM 语义映射（四档）

后端只消费 `scene.h` 已降维的 position/gain 等渲染输入；SpatialMixer 本身不是 ADM 语义引擎，只暴露空间化参数、算法、source mode、距离衰减、reverb 与全局头部姿态/追踪。支持范围必须区分 **AU 原生能力**、**项目层预处理**、**近似降级** 与 **不支持**，避免把 Apple 平台输出模式误报为 ADM 字段覆盖。

**第一档 · AU 原生直映**：position→Azimuth/Elevation/Distance（§7）；gain→`kSpatialMixerParam_Gain`；jump/插值→render 回调驱动参数自动化（jump=瞬时设，非 jump=插值窗内 ramp）；距离仅在启用 `kSpatialMixerRenderingFlags_DistanceAttenuation` 且配置 `MixerDistanceParams` 后参与响度衰减。DirectSpeakers 可按 bus channel layout 走 `AmbienceBed` 远场床层；LFE 走 `Bypass` + LFE channel layout，不空间化。

**第二档 · 项目层预处理后喂 AU**：Cartesian position→`scene_position_to_polar()`；positionOffset→`apply_position_offset()`；divergence→`expand_object_divergence()` 拆 3 点源；channelLock→`apply_channel_lock()` 吸附 az/el（**仅扬声器输出**；双耳无离散扬声器集，drop）。这些不是 SpatialMixer 原生 ADM 语义，只能在 Capability/文档中标注为后端预处理支持。

**第三档 · 缺 ADM 等价原语，只能近似（降级，明确标注）**：extent(width/height/depth)→拆多个铺开点源覆盖（复用 binaural spreader 几何或独立点云）；DirectSpeakers position range→首版忽略，后续最多用于选择/校正标称位置。此类能力不应写成原生支持，只能写成 spread approximation。

**第四档 · 丢弃/不支持**：diffuse→SpatialMixer 无 ADM direct/diffuse 能量拆分与去相关器；`ReverbBlend` / internal reverb 是创作型房间效果，不能等同 ADM diffuse，首版 drop 或 warning，`supports_diffuse=false`。screenLock/screen_ref→缺 referenceScreen 几何，drop；headLocked→AU 只有全局 HeadYaw/Pitch/Roll 与全局 AirPods head tracking，无法表达 per-object headLocked，离线路径 drop；headphoneVirtualise→项目 importer 当前不建模该 ADM flag，SpatialMixer headphones/HRTF 只能算输出模式能力，不算字段语义支持；importance/dialogue→元数据/策略维度，不参与默认渲染数学。

**非路径选择**：`AUAudioMix` 的 `kAUAudioMixProperty_SpatialAudioMixMetadata` 是 file asset remix metadata，不是公开 ADM→SpatialMixer 语义映射 API；首版不依赖它弥补上述字段缺口。

## 7. 坐标系转换（最易出错，实测确认）

| 系统 | 约定 |
|---|---|
| 本项目 ADM（`scene.h`，极坐标） | azimuth **+ve=左**（BS.2051）、elevation +ve=上、front=0 |
| Apple SpatialMixer 参数 | azimuth **+ve=右**（实测 +90→右声道更响）、elevation +ve=上、distance 单位**米**（0..10000） |

**核心映射（ADM polar → SpatialMixer 参数）**：

```
sm_azimuth   = -adm_azimuth        // ⚠️ 符号取反，漏了整个声场左右镜像
sm_elevation =  adm_elevation       // 两侧 +ve=上，不翻；超 ±90 clamp
sm_distance  =  参考米数            // ADM 归一化→米；仅 DistanceAttenuation flag 开启才影响响度，
                                    // 否则取中性 1.0m（且 >1e-3 防 0）
```

直接从极坐标喂，**不绕 Cartesian 往返**（旧实现 `sphericalToCartesian` 用 `mathAz=90−az` 把 +方位当右，是符号混乱源）。

## 8. 工厂预设、算法与输出类型

**空间化算法**（`kAudioUnitProperty_SpatializationAlgorithm`，逐 bus）：0 EqualPowerPanning / 1 SphericalHead / 2 HRTF / 3 SoundField / **4 VBAP** / 5 StereoPassThrough / 6 HRTFHQ / 7 UseOutputType。

- 双耳输出：对象/床层用 **HRTFHQ**。
- 多声道扬声器输出：用 **VBAP**（或 SoundField），不依赖 OutputType。

**工厂预设**（`kAudioUnitProperty_PresentPreset`，实机确认 3 个）：`[0] Built-In Speaker Media Playback` / `[1] Headphone Media Playback Default`（≈ common media）/ `[2] Headphone Media Playback Movie`（≈ Apple TV 影院档）。

- **仅在双耳输出模式暴露**（如 `--apple-preset default|movie`）。预设是 2 声道重放语境的不透明内部调音，**多声道输出不暴露预设**（其对多声道 VBAP 仅有未定义副作用）。
- ⚠️ `PresentPreset` 会重置每输入 bus 的 az/el/sourceMode → 顺序必须「先设预设 → 再下发 ADM 参数」。

**OutputType**（Headphones/BuiltIn/External）都是 2ch 重放提示，不约束声道数；首版双耳走 Headphones，多声道不依赖它。

## 9. 输出布局与容器 tag

- **双耳 HRTF → 2ch**：布局 id `binaural`，容器 tag `kAudioChannelLayoutTag_Binaural`（106）。`caf_io.cpp` / `apac_io.cpp` 现状已对 `binaural` 打此 tag，与普通 `0+2+0` stereo 区分——直接复用。
- **多声道 → 扬声器布局**：复用现有 Atmos tag（`caf_io.cpp`：`4+5+0`→Atmos_5_1_4、`4+7+0`→Atmos_7_1_4 …）。
- 注意：双耳 tag 只在 CoreAudio 容器（CAF/APAC）存活；WAV/FLAC 无标准双耳标签，走这俩格式会丢失双耳元数据。

## 10. CapabilityReport（首版）

```
backend_name = "apple"
supported_layouts = [ binaural(is_binaural=true), 5.1.4(is_3d,lfe_count=1), 7.1.4, ... ]
supports_objects          = true
supports_direct_speakers  = true
supports_hoa              = false
supports_object_divergence= true   // expand_object_divergence
supports_channel_lock     = true   // apply_channel_lock；仅扬声器输出，双耳 drop
supports_diffuse          = false  // 无 ADM diffuse 去相关器；ReverbBlend 不等价
supports_screen_ref       = false
supports_render_window    = false  // 首版：全量渲染 + 文件裁剪（见 §12）
```

## 11. 测试与回归策略

无 bit-exact，改用基于性质/能量的断言（macOS-only，Linux 自动 skip，同 APAC）：

- **坐标符号回归（必加）**：ADM azimuth=+30°（左）对象渲染后**左声道能量 > 右声道**——一次性逮住整声场镜像翻转。
- **声道数 / 布局**：binaural=2ch、`7.1.4`=12ch 等正确。
- **object/bed/LFE 分路**：bed 静态居位、LFE 不被空间化（能量集中而非弥散）。
- **divergence/extent 展开**：开启后能量按预期向两侧/区域扩散。
- **smoke**：不崩、输出文件有效、时长正确、响度/True Peak 在合理范围。
- **跨平台**：Linux 上 `mr_adm_apple_*_tests` 自动 skip（仿 `mr_adm_apac_smoke_tests`）。

golden file 若用，必须容差化或钉 macOS 版本（Apple HRTF 跨版本可变）。

## 12. 分期

- **Phase 1（首版）**：离线，双耳 + 多声道扬声器；`supports_render_window=false`（全量渲染 + 文件裁剪，PreviewSession 仍可用，只是不省时）；预设仅双耳；坐标取反 + 回归断言；ADM 语义一/二档完整，三档近似且标注。
- **Phase 2**：窗口化（`supports_render_window=true`，AU 内部状态需 pre-roll/seek，同其它后端的纪律，但本就无 bit-exact，定容差策略）。
- **Phase 3**：实时预览路径（复用 prepare 的 AU 配置 + ADM→参数映射，换实时驱动循环；引入 head-tracking / headLocked / transaural builtin）。

## 13. 风险与未决

- **无 bit-exact / 跨版本不稳定**：Apple 黑盒 HRTF，回归只能容差/版本钉死。
- **ADM 语义缺口**：extent/diffuse 仅近似，screenLock/headLocked drop——坐实平台风味定位。
- **diffuse 近似手段**待定：`ReverbBlend` 代理 vs 直接 drop，需听感/能量评估后定。
- **extent 铺开几何**：复用 binaural spreader 还是另立，待实现时评估。
- **参数自动化粒度**：动态对象位置更新频率与 SpatialMixer 内部 ramp 的交互，待实测调参。
- **构建开关**：跟随 APAC 不加 flag（暂定），未最终拍板。
