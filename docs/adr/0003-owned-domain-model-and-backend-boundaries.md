# ADR 0003：自有 ADM 领域模型与后端边界

> 状态：已接受  
> 日期：2026-05-17  
> 适用范围：`adm_core`、`adm_io`、`adm_render_*`、`adm_apple` 以及所有未来渲染后端。

## 背景

MacinRender ADM Core 计划接入 `libbw64`、`libadm`、`libear`、Spatial Audio Framework（空间音频框架，简称 SAF）以及未来 Apple SpatialMixer/AVAudioEngine 等后端。`libadm` 和 `libear` 都对 ADM（Audio Definition Model，音频定义模型）有自己的类型系统和建模方式。初期开发时，很容易为了快速跑通而直接把 `libadm::AudioObject`、`libadm` document 节点或 `libear` 的渲染输入类型当作项目内部领域对象传递。

这条捷径会破坏长期架构目标：

- `adm_core` 会被某个库的类型系统塑形。
- 新增后端时，不得不依赖 `libadm`/`libear` 类型，或者在每个后端重复写大量转换逻辑。
- “多后端可替换”会退化成“以某个后端为中心的适配堆叠”。
- 未来 Rust、GUI（图形用户界面）或脚本绑定都会继承这些耦合。

因此，M2（IO 与 ADM scene import）的边界比 M1 skeleton（骨架）更关键。M2 不是简单接入 `libbw64` 和 `libadm`，而是要建立项目自己的 ADM scene（场景）模型。

## 决策

`adm_core` 必须拥有自有 ADM 领域模型。`libbw64`、`libadm`、`libear`、SAF、SpatialMixer、AVAudioEngine 等第三方或平台类型不得塑造或泄露进核心领域模型。

输入路径必须遵循：

```text
libbw64/libadm
  -> adm_io 适配层
  -> adm::AdmScene（项目自有类型）
  -> RenderPlan
  -> IRenderer 后端
```

渲染后端必须遵循：

```text
adm::AdmScene / RenderPlan
  -> 后端私有 adapter
  -> libear / SAF / SpatialMixer / AVAudioEngine / 自研 renderer
```

也就是说：

- `libadm` 类型只允许出现在 `adm_io` 适配层内部。
- `libear` 类型只允许出现在 `adm_render_ear` 后端内部。
- SAF 类型只允许出现在 `adm_render_saf` 后端内部。
- Apple SpatialMixer/AVAudioEngine 类型只允许出现在 `adm_apple` 后端内部。
- `adm_core`、`adm_render` 抽象层、C ABI（Application Binary Interface，应用二进制接口）和 CLI（命令行界面）不得暴露这些类型。

## SpatialMixer 定位

SpatialMixer 不被视为“将被 libear 替换的旧实现”。它应被收纳为 Apple 平台后端：

```text
adm_apple::SpatialMixerRenderer : IRenderer
```

它与 `adm_render_ear`、`adm_render_saf` 平级，只是平台适用范围不同：

- `adm_render_ear`：标准 BS.2127 后端，跨平台优先。
- `adm_render_saf`：SAF 后端，承接 VBAP、HOA、扩散、去相关等 DSP 能力。
- `adm_apple`：macOS-only 后端，承接 SpatialMixer、AVAudioEngine 和未来 Apple 平台能力。

macOS 上可以同时存在多个后端，由 `RenderPlanner` 和 capability report（能力报告）决定使用、拒绝或降级。

## M2 验收约束

M2 完成时，至少需要满足：

- 存在 `adm::AdmScene` 或等价自有类型，位于 `adm_core`。
- `adm_io` 提供从 `libbw64/libadm` 到 `AdmScene` 的单向导入适配。
- `adm_core` 公共头文件不 include `libadm`、`libbw64`、`libear`、SAF 或 Apple 音频框架头。
- `adm_render` 的 `IRenderer` 接口只接受项目自有类型，例如 `RenderPlan`、`AdmScene`、`AudioBufferView`、`SpeakerLayout`。
- `adm_render_ear` 内部可以把 `AdmScene` 转为 `libear` 输入，但转换不能反向污染 `AdmScene`。
- `adm_apple` 后端以 `IRenderer` 实现出现，不以核心特殊路径出现。
- golden fixtures（黄金样本）应覆盖 `AdmScene` import 摘要，以便发现适配层遗漏。

## 代码组织建议

```text
include/adm/
  scene.h          自有 ADM scene 类型
  render.h         RenderPlan / IRenderer-facing 类型
  layout.h         SpeakerLayout / ChannelLabel

src/adm_io/
  bw64_reader.cpp
  libadm_scene_importer.cpp

src/adm_render_ear/
  ear_renderer.cpp
  ear_scene_adapter.cpp

src/adm_render_saf/
  saf_renderer.cpp
  saf_scene_adapter.cpp

src/adm_apple/
  spatial_mixer_renderer.mm
  avaudio_engine_renderer.mm
```

## 禁止事项

- 禁止在 `include/adm/*` 中 include 第三方 ADM/renderer 头。
- 禁止 `adm_core` 类型继承或包装第三方类型作为字段。
- 禁止 `RenderPlan` 持有 `libadm` document、`libear` scene、SAF handle 或 Apple object。
- 禁止为了某个后端便利，把后端专属字段塞进通用领域模型。
- 禁止 CLI 根据第三方库细节做功能判断；能力判断应来自 renderer capability report。

## 允许事项

- 允许 `adm_io` 内部使用 `libbw64`/`libadm` 并转换成 `AdmScene`。
- 允许后端内部持有后端专用 cache（缓存）、handle（句柄）或转换结果。
- 允许在 `AdmScene` 中表达标准 ADM 概念和项目需要的规范化结果。
- 允许无法完整表达的第三方特性进入 warning/diagnostic（诊断）列表，但不允许静默丢弃。

## 后果

优点：

- 多后端架构有真实边界。
- Apple 后端、`libear` 后端、SAF 后端可以平级共存。
- 未来 Rust 或其他语言绑定不会被 `libadm`/`libear` C++ 类型绑定。
- golden fixtures 可以围绕自有 `AdmScene` 建立稳定行为基线。

代价：

- M2 会比直接传递 `libadm` 类型更慢。
- 需要认真设计 `AdmScene` 的最小可用模型。
- 需要维护导入适配层和后端适配层。

这个代价是有意接受的，因为它保护的是后续所有后端和平台扩展。
