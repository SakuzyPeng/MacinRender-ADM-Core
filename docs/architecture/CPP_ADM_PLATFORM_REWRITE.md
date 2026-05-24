# C++ ADM 渲染平台化重构规划

> 状态：草案  
> 日期：2026-05-16  
> 目标：记录将当前 macOS 优先的 ADM CLI（命令行界面）工具重构为跨平台 C++ 渲染平台的方向、边界和迁移顺序。GUI（图形用户界面）暂不进入近期实现范围，但核心接口需要为未来 GUI、脚本绑定和其他宿主调用预留稳定边界。

## 1. 背景

当前项目已经具备完整的 ADM BWF（Audio Definition Model Broadcast Wave Format，音频定义模型广播波形格式）渲染能力，主要入口包括：

- `app/adm_unified.m`：CLI 入口，包含命令解析、帮助文本、单文件、批处理、诊断和渲染分派。
- `Core/API/*`：已有 `ADMRenderRequest`、`ADMRenderOptions`、`ADMRenderPlan`、`ADMRenderService` 等服务层雏形。
- `Core/Rendering/*`：包含 SpatialMixer、AVAudioEngine、VBAP、HOA 等渲染路径。
- `Utilities/*`：包含 CLI、IO、DSP（Digital Signal Processing，数字信号处理）、响度、峰值、日志和性能监控。

现有结构的优势是功能已经跑通，并且已经开始把 CLI 过程式逻辑收敛到 `ADMKit` API（应用程序接口）层。主要问题是平台依赖和业务边界仍然耦合：

- `Foundation`、`AVFoundation`、`AudioToolbox`、`CoreAudio`、`Accelerate` 等 Apple 平台能力渗透到解析、IO、DSP、布局和渲染编排中。
- CLI 仍承担过多业务职责，后续 GUI 或非交互式服务复用成本较高。
- ADM/BW64/渲染后端没有形成清晰的 C++ 领域模型和可替换后端边界。
- 项目目标正在从“macOS CLI 工具”扩展到“跨平台 ADM 渲染能力”，现有工程结构需要重新组织。

因此，本次规划不应被视为简单的 CLI 重写，而应被视为 C++ ADM 渲染平台化重构。

语言路线的核心判断是：选择 C++ 主要是为了利用现有 ADM/BW64/BS.2127/SAF 生态，而不是排斥 Rust。Rust 仍可作为后续渐进引入方向，但第一阶段不一步到位 Rust；详见 `docs/adr/0002-cpp-first-rust-later.md`。

## 2. 重构目标

### 2.1 近期目标

- 暂时不实现 GUI，但设计可被未来 GUI 直接调用的核心接口。
- 新建跨平台 C++ 核心，优先服务新的 CLI。
- 新核心采用 C++20，代码风格保持朴素、明确、可调试；详见 `docs/adr/0001-cpp-standard.md`。
- 采用“先 C++ 地基、后续渐进引入 Rust”的语言路线；详见 `docs/adr/0002-cpp-first-rust-later.md`。
- 使用 CMake（跨平台构建系统）作为新内核的主要构建入口。
- 接入 C++ 生态中的 ADM 相关库，优先考虑：
  - `libbw64`：BW64/RF64/WAV 容器、`axml` 和 `chna` chunk 读写。
  - `libadm`：BS.2076 ADM XML 解析和序列化。
  - `libear`：BS.2127 标准渲染后端。
  - `Spatial_Audio_Framework`，简称 SAF：VBAP、HOA、HRTF、扩散和去相关等空间音频 DSP 能力。
- 保留现有行为作为回归基线，避免重构过程中音频输出和默认行为不可控漂移。

### 2.2 长期目标

- 将项目核心从 macOS 专用实现演进为跨平台 ADM 渲染内核。
- 让 CLI 成为薄壳，只负责参数解析、配置加载、进度展示和退出码。
- 让渲染器以后可以按后端替换或组合，例如 `libear`、SAF、Apple SpatialMixer、AVAudioEngine、自研后端。
- 通过稳定 C ABI（Application Binary Interface，应用二进制接口）为 Swift、Objective-C、Python、Node.js 或其他宿主预留绑定能力。
- 允许项目结构大幅调整，以清晰、可维护、可扩展为优先。

## 3. 非目标

- 近期不重做 GUI。
- 近期不要求一次性移除现有 Objective-C/Objective-C++ 实现。
- 近期不追求所有 ADM 特性一次覆盖到新内核。
- 不把第三方库类型直接暴露为项目公共 API。
- 不以“代码逐行翻译”为迁移策略。

## 4. 架构原则

1. C++ core 不能依赖 Apple 平台框架。
   - 禁止依赖 `Foundation`、`AVFoundation`、`AudioToolbox`、`CoreAudio`、`Accelerate`。
   - Apple 专属能力只能位于 `adm_apple` 或平台适配后端。

2. CLI 不能直接触碰第三方渲染库。
   - CLI 不直接调用 `libadm`、`libbw64`、`libear`、SAF。
   - CLI 只构造 `RenderRequest`，调用 `RenderService`，订阅进度和日志。

3. 第三方库类型不能污染全链路，且不能塑造 `adm_core` 的领域模型。
   - `libadm`、`libbw64` 等输入结果需要转换成项目自己的领域模型。
   - 项目内部使用 `AdmScene`、`AudioObject`、`DirectSpeaker`、`HoaElement`、`SpeakerLayout` 等自有类型。
   - `libear`、`libadm`、SAF、SpatialMixer、AVAudioEngine 的类型只能存在于各自适配层或后端内部。
   - 详细约束见 `docs/adr/0003-owned-domain-model-and-backend-boundaries.md`。

4. 渲染器后端化。
   - 每个渲染实现通过统一接口暴露能力、限制和执行入口。
   - 后端选择由 `RenderPlanner` 和 capability report（能力报告）决定，而不是散落在 CLI 字符串判断中。

5. 未来 GUI 只依赖稳定接口。
   - GUI 不直接依赖内部 C++ 类图。
   - 对 Swift/Objective-C GUI 建议通过 C ABI 或 ObjC++ wrapper（Objective-C++ 包装层）桥接。

6. 回归测试先行。
   - 项目结构可以大改，但行为需要由 golden fixtures（黄金样本）保护。
   - 解析摘要、布局摘要、输出声道、时长、响度、True Peak 和音频误差阈值都应纳入回归基线。

7. C++20 是语言基线，但不追求语法炫技。
   - 推荐使用 `std::span`、`std::filesystem`、`std::optional`、`std::variant`、`std::string_view`、`std::jthread` 和 `std::stop_token`。
   - 谨慎使用 concepts（概念）和 ranges（范围库），只在能明显降低复杂度时使用。
   - 暂不使用 modules（模块）、coroutines（协程）、复杂 ranges 链式写法和 C++23-only 标准库能力。
   - CMake 应使用标准 C++20，而不是 GNU 扩展模式。

8. C++ 地基需要 Rust-friendly。
   - 公共边界优先使用 C ABI，不暴露 STL 容器、异常或复杂模板类型。
   - 领域模型、IO、渲染、后处理和平台适配保持清晰分层，避免未来 Rust 模块只能通过 CLI 文本集成。
   - Rust 可以先进入 CLI、工具链、fixture 管理、报告生成、小型 DSP 或 metadata 诊断模块。
   - 不规划一次性“C++ 完成后全量 Rust 重写”；优先让 Rust 按模块自然长出来。

## 5. 建议项目结构

目标结构可以在后续迁移中逐步形成：

```text
MacinRender-ADM-Tool/
  CMakeLists.txt
  cmake/
  src/
    adm_core/
      纯 C++ 领域模型、错误、日志、进度、配置、能力报告
    adm_io/
      libbw64 + libadm 适配，负责 ADM BWF/BW64/WAV 读写
    adm_render/
      渲染抽象接口、渲染计划、布局、后端注册和调度
    adm_render_ear/
      基于 libear 的标准渲染后端
    adm_render_saf/
      基于 SAF 的 VBAP/HOA/扩散/去相关后端
    adm_process/
      响度、True Peak、limiter、gain、格式后处理
    adm_cli/
      新 CLI 入口，只做参数解析和调用核心
    adm_c_api/
      稳定 C ABI，供 GUI、脚本绑定和其他宿主调用
    adm_apple/
      macOS-only 适配层，未来接 SpatialMixer/AVAudioEngine/Swift GUI
  include/
    adm/
      adm.h
      render.h
      options.h
      progress.h
      errors.h
      c_api.h
  tests/
    unit/
    integration/
    golden/
    fixtures/
  tools/
    adm/
  third_party/
  vendor/
  docs/
    architecture/
    adr/
```

现有 `Core/`、`Utilities/`、`app/`、`GUI/` 可以在迁移期保留，作为旧实现和回归基线。新代码不必被现有目录限制。

## 6. 核心模块边界

### 6.1 `adm_core`

职责：

- 领域模型：`AdmScene`、`AdmProgramme`、`AdmContent`、`AudioObject`、`AudioPack`、`AudioTrack`、`BlockFormat`。
- 空间模型：`Position`、`Extent`、`ChannelLock`、`ZoneExclusion`、`SpeakerLayout`。
- 配置模型：`RenderOptions`、`OutputOptions`、`PostProcessOptions`。
- 结果模型：`RenderResult`、`AnalysisResult`、`Warning`、`Diagnostic`。
- 基础设施：错误、日志、进度、取消、能力报告。

限制：

- 不依赖平台框架。
- 不直接读写文件格式。
- 不直接调用任何具体 renderer（渲染器）。

### 6.2 `adm_io`

职责：

- 读取 ADM BWF/BW64/WAV。
- 从 `axml`、`chna`、音频流等输入构造 `AdmScene` 和音频源索引。
- 写出 WAV/BW64 及必要元数据。
- 隔离 `libbw64`、`libadm` 的 API 变化。

原则：

- `libadm` 类型只出现在该模块内部或适配边界。
- 读入后统一转换为 `adm_core` 自有模型。
- 若某些 ADM 特性暂不支持，应输出结构化 warning，而不是静默丢弃。

### 6.3 `adm_render`

职责：

- 定义 `IRenderer` 接口。
- 定义 `RenderPlan` 和 `RendererInfo`。
- 维护 renderer registry（渲染器注册表）。
- 根据输入特性、输出布局和用户选项选择后端。
- 给出能力报告，包括支持、降级、拒绝原因和建议。

接口草案：

```cpp
class IRenderer {
public:
    virtual ~IRenderer() = default;

    virtual RendererInfo info() const = 0;
    virtual CapabilityReport supports(const RenderPlan& plan) const = 0;
    virtual RenderResult render(const RenderPlan& plan, IProgressSink& progress) = 0;
};
```

### 6.4 `adm_render_ear`

职责：

- 基于 `libear` 实现标准 ADM 渲染路径。
- 优先覆盖 `DirectSpeakers`、`Objects`、`HOA` 等 `libear` 已支持能力。
- 对 `libear` 未覆盖的 ADM 特性输出明确 warning 或 fallback（回退）建议。

注意：

- `libear` 是标准路径的重要基础，但不应被假设为覆盖全部项目需求。
- 需要建立 feature matrix（特性矩阵），记录每类 ADM 特性在 `libear`、SAF、Apple 后端中的支持状态。

### 6.5 `adm_render_saf`

职责：

- 基于 SAF 实现 VBAP、HOA、扩散、去相关、HRTF 等空间音频 DSP 能力。
- 接管或重写当前 `Core/Rendering/VBAP` 中的可跨平台逻辑。
- 保持 SAF optional module（可选模块）许可证边界清晰。

注意：

- 需要继续维护 SAF 链接和许可证审计。
- macOS 可使用 Accelerate 后端，但跨平台路径应支持 OpenBLAS 或其他 BLAS/LAPACK 后端。

### 6.6 `adm_process`

职责：

- 响度测量。
- True Peak 检测。
- 峰值限制。
- 响度标准化。
- 最终增益。
- FLAC/Opus MKA/WAV/CAF 等后处理策略。

FLAC 输出策略：

- 当前固定写出 24-bit integer FLAC，作为渲染结果的无损压缩交付格式。
- 16-bit FLAC 主要服务消费级体积/兼容性诉求，列为后续 CLI 选项（如 `--flac-bit-depth 16|24`），不阻塞首发。
- 如需保留 0 dBFS 以上 headroom 或 float 工作流，应使用 WAV/CAF float 输出，而不是 FLAC。

Opus MKA 输出策略：

- 当前写出 Matroska Audio（`.mka`）容器 + Opus 编码，输入采样率固定要求 48 kHz。
- 1-2 声道使用 Opus mapping family 0；标准 5.1 / 7.1 会重排到 Opus/Vorbis 声道顺序并使用 family 1；HOA3 使用 Opus ambisonics mapping family 2。
- family 255 是透明多流编码，不携带标准扬声器布局语义；22.2、9.1.6 等布局只作为容器 metadata 记录，播放器不保证自动识别。
- HOA 直接回放目前只确认 macOS 上的 CAF 与 APAC 可行；Opus HOA3 虽可写入 ambisonics mapping，但常见播放器兼容性不足，不作为通用直接监听格式。VLC 4.0 可回放 Opus HOA3，但需要在音频选项中将 mix node 从 `original: ambisonics` 改为 `binaural`。
- Opus 为有损交付格式，响度/峰值/增益等后处理必须在 float 中间文件上完成，最后一步再编码。

原则：

- DSP 算法尽量使用跨平台实现。
- 平台加速作为可选后端，而不是核心依赖。

### 6.7 `adm_cli`

职责：

- 解析命令行参数。
- 读取配置文件。
- 构造 `RenderRequest`。
- 订阅进度、日志和诊断事件。
- 打印结果并返回退出码。

禁止：

- 直接解析 ADM XML。
- 直接读写 BW64 chunk。
- 直接选择具体第三方渲染库实现。
- 复制核心校验规则。

### 6.8 `adm_c_api`

职责：

- 提供稳定 C ABI。
- 面向 GUI、脚本绑定、插件宿主和跨语言集成。
- 隐藏 C++ 内部对象生命周期和异常。

接口草案：

```c
typedef struct adm_context_t adm_context_t;
typedef struct adm_render_request_t adm_render_request_t;
typedef struct adm_render_result_t adm_render_result_t;

typedef void (*adm_progress_cb)(double fraction, const char* stage, const char* message, void* user_data);

adm_context_t* adm_create_context(void);
void adm_destroy_context(adm_context_t* context);

int adm_render_file(
    adm_context_t* context,
    const adm_render_request_t* request,
    adm_progress_cb progress,
    void* user_data,
    adm_render_result_t** result);

void adm_destroy_render_result(adm_render_result_t* result);
```

## 7. 数据流

目标数据流：

```text
CLI args / config
  -> RenderRequest
  -> RequestValidator
  -> IO probe
  -> libbw64/libadm import
  -> AdmScene normalize
  -> RenderPlan
  -> Renderer selection
      -> libear backend
      -> SAF backend
      -> future Apple backend
  -> PCM render
  -> WAV/BW64/CAF/FLAC writer
  -> loudness / limiter / metadata
  -> RenderResult
```

关键点：

- `RenderRequest` 是用户意图。
- `RenderPlan` 是核心执行计划。
- `AdmScene normalize` 是第三方库和项目内部模型的隔离层，不能做薄或做漏。
- `AdmScene` 是项目自有领域模型，不允许被 `libadm` 或 `libear` 的类型系统塑形。
- 后处理应明确位于渲染之后，并纳入结果报告。

## 8. 第三方依赖定位

| 依赖 | 角色 | 使用边界 |
| --- | --- | --- |
| `libbw64` | BW64/RF64/WAV 容器和 ADM BWF chunk 读写 | 仅在 `adm_io` 适配层直接使用 |
| `libadm` | BS.2076 ADM XML 解析和序列化 | 仅在 `adm_io` 适配层直接使用 |
| `libear` | BS.2127 标准渲染路径 | 仅在 `adm_render_ear` 后端直接使用 |
| `Spatial_Audio_Framework` | VBAP、HOA、HRTF、扩散、去相关等空间音频 DSP；SOFA reader 默认开启用于用户 HRIR，NetCDF 关闭 | 仅在 `adm_render_saf` / `adm_render_binaural` 后端直接使用 |
| `libebur128` | EBU R128 响度与 True Peak 相关测量 | 可在 `adm_process` 适配层使用 |
| `libsndfile` | 普通音频 IO 候选 | 可选，不替代 ADM BWF 主路径 |
| `OpenBLAS` / `Accelerate` | BLAS/LAPACK 后端 | 作为平台加速实现，不进入核心接口 |

## 9. GUI 预留接口

虽然近期不做 GUI，但新架构需要从第一天开始为 GUI 保留以下能力：

- 结构化进度：阶段、百分比、当前文件、当前任务、总任务数。
- 结构化日志：级别、模块、消息、可选上下文。
- 可取消：长任务需要可响应取消请求。
- 可恢复错误：输出冲突、格式不支持、特性降级、缺失依赖等需要可展示和可选择。
- 非交互式请求模型：GUI 不应该模拟 stdin，也不应该依赖 CLI 输出文本。
- 稳定 C ABI：Swift/Objective-C 层通过薄 wrapper 调用，不绑定内部 C++ 类。

## 10. 测试与回归基线

迁移前应先固定 golden fixtures，避免重构导致行为漂移。

每个 fixture 建议保存：

- 输入文件基本信息：采样率、位深、声道数、时长。
- ADM 解析摘要：programme/content/object/pack/track 数量。
- 空间元素摘要：objects、direct speakers、HOA、动态 block 数量。
- 输出布局摘要：布局 ID、声道数、声道顺序。
- 渲染结果摘要：输出时长、输出声道、峰值、响度、True Peak。
- 音频误差阈值：按 renderer 和布局设置允许误差。
- 诊断 warning 列表：用于确认特性降级是否符合预期。

测试分层：

```text
tests/unit/
  领域模型、选项解析、布局规则、能力报告
tests/integration/
  libbw64/libadm 导入、renderer 选择、后处理链
tests/golden/
  固定输入样本的输出摘要和音频误差回归
tests/fixtures/
  小型 ADM BWF 样本、边界样本、布局样本
```

## 11. 迁移阶段

### M0：文档与决策基线

- 新增本规划文档。
- 后续重大技术取舍写入 `docs/adr/`。
- 记录 C++20 作为新核心语言标准。
- 记录先 C++、后续渐进引入 Rust 的语言路线。
- 梳理现有 CLI 选项和默认行为。

验收：

- 文档可作为后续任务拆分依据。
- 没有代码行为变化。

### M1：C++ skeleton

- 新增 CMake 工程骨架。
- 新增 `adm_core`、`adm_cli`、`adm_c_api` 的空实现和最小测试。
- 新 CLI 支持 `--version`、`--help`、`probe` 之类最小命令。

验收：

- macOS 本地可构建。
- Linux CI 可至少编译空核心。
- 旧 ObjC CLI 不受影响。

### M2：IO 与 ADM scene import

- 接入 `libbw64` 和 `libadm`。
- 实现 ADM BWF probe。
- 实现 `AdmScene` import。
- 输出 scene dump，用于和现有解析器对比。
- 明确 `libbw64/libadm -> adm_io -> AdmScene` 的单向边界，禁止 `libadm` 类型进入 `adm_core`、`adm_render`、CLI 或 C ABI。
- 建立 `AdmScene` import 的 golden 摘要，用于发现适配层遗漏。

验收：

- 固定样本的 ADM 摘要和现有实现一致或差异可解释。
- 所有不支持特性进入 warning 列表。
- `include/adm/*` 不 include `libadm`、`libbw64`、`libear`、SAF 或 Apple 音频框架头。
- `IRenderer` 接口只接受项目自有类型，例如 `RenderPlan`、`AdmScene`、`SpeakerLayout`。
- SpatialMixer 以 `adm_apple` 后端形式实现 `IRenderer`，与 `adm_render_ear` 平级，不作为核心特殊路径。

### M3：标准渲染最小闭环

- 接入 `libear` 后端。
- 完成最小 speaker 或 binaural 输出闭环。
- 写出 WAV/BW64。
- 引入基础响度和峰值摘要。

验收：

- 至少一个 golden fixture 可端到端渲染。
- 输出摘要可回归。

### M4：SAF 后端迁入

- 将当前 VBAP/Spreader 的跨平台逻辑迁移或重写到 `adm_render_saf`。
- 建立 SAF feature matrix 和许可证审计。
- 支持主要多声道布局。

验收：

- `wav71`、`atmos714`、`atmos916`、`cicp13` 等布局有明确支持状态。
- SAF 链接不引入禁止模块。

### M5：新 CLI 替代旧 CLI

- 新 CLI 覆盖主要用户命令。
- 旧 CLI 进入兼容或弃用阶段。
- README 和批处理文档同步更新。

验收：

- 常用命令行为兼容。
- 旧 CLI 与新 CLI 的输出差异有记录。

### M6：未来 GUI/API 接入

- 基于 `adm_c_api` 或 ObjC++ wrapper 接 GUI。
- GUI 调用同一套 `RenderService`。
- GUI 不复制 CLI 业务规则。

验收：

- GUI 可以不通过 CLI 完成一次渲染。
- 进度、日志、取消和错误展示均来自结构化接口。

## 12. 风险

### 12.1 标准覆盖风险

`libear` 并不等于完整项目需求。部分 ADM 特性可能需要 fallback 或自研补充。必须用 feature matrix 明确：

- 支持。
- 不支持。
- 部分支持。
- 可降级。
- 必须拒绝。

### 12.2 行为漂移风险

不同渲染库的算法和默认参数可能导致输出差异。需要用 golden fixtures 和误差阈值管理，而不是追求所有路径 bit-exact（逐位一致）。

### 12.3 许可证风险

SAF 的 permissive module（宽松许可证模块）和 GPL optional module（GPL 可选模块）需要严格隔离。任何新增链接源都应经过审计。

### 12.4 平台后端风险

macOS 现有路径依赖 Apple 框架。跨平台后端需要替代：

- 音频文件 IO。
- BLAS/LAPACK。
- 音频格式标签。
- CAF 等 Apple 格式能力。

### 12.5 工程规模风险

项目结构允许大改，但应分阶段落地。每个阶段都要保持旧工具可用，避免长期处于不可发布状态。

## 13. 已决策问题

### 13.1 C++ 标准

- 决策：新 C++ 核心采用 C++20。
- 约束：使用“C++17 心智 + C++20 基础设施”的保守写法。
- 构建建议：

```cmake
set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
```

- 详细记录：`docs/adr/0001-cpp-standard.md`。

### 13.2 语言路线

- 决策：第一阶段不一步到位 Rust，先以 C++20 建立跨平台核心地基。
- 原因：当前 ADM/BW64/BS.2127/SAF 的成熟生产生态集中在 C++，直接服务本项目近期目标。
- 约束：C++ 地基必须保持 Rust-friendly，包括 C ABI、模块化后端、清晰领域模型和 golden fixtures。
- Rust 进入方式：优先从 CLI、工具链、测试、报告、小型 DSP 或 metadata 模块渐进引入。
- 详细记录：`docs/adr/0002-cpp-first-rust-later.md`。

### 13.3 领域模型与后端边界

- 决策：`adm_core` 拥有自有 ADM 领域模型，不能被 `libadm`、`libear` 或任何后端类型系统塑形。
- 输入边界：`libbw64/libadm -> adm_io 适配层 -> AdmScene -> RenderPlan -> IRenderer`。
- 后端边界：各后端内部将 `AdmScene`/`RenderPlan` 转换为自己的私有输入类型。
- SpatialMixer 定位：`adm_apple` 后端实现，与 `adm_render_ear`、`adm_render_saf` 平级。
- 详细记录：`docs/adr/0003-owned-domain-model-and-backend-boundaries.md`。

### 13.4 第三方依赖管理

- 决策：`FetchContent` + `find_package(CONFIG)` 兜底为主路径；SAF 因许可证模块化复杂走 vendored submodule。
- `libbw64`/`libadm`/`libear` 通过 `cmake/MRDependencies.cmake` 接入，与 `fmt`/`spdlog`/`CLI11` 共用 find-or-fetch 模式。
- `MR_ADM_CORE_FETCH_DEPS=OFF` 必须保持可用，支持 Linux 发行版打包与离线构建。
- `libFLAC` 采用三档策略：开发构建可优先系统库，正式分发默认 vendored static，包管理器可通过 `MR_ADM_FLAC_PROVIDER=SYSTEM` 或 `MR_ADM_USE_SYSTEM_FLAC=ON` 强制系统库。
- `libopus` 采用同样的三档策略：开发构建可优先系统库，正式分发默认 vendored static，包管理器可通过 `MR_ADM_OPUS_PROVIDER=SYSTEM` 或 `MR_ADM_USE_SYSTEM_OPUS=ON` 强制系统库。
- ALAC 暂不进入首个全平台核心格式集合。若实现，短期可用 AudioToolbox 作为 macOS-only 快路径；全平台路线需另行评估 FFmpeg/libavformat provider 或 vendored Apple ALAC encoder + MP4 muxer，避免把 MP4 容器复杂度过早拉进核心。
- 详细记录：`docs/adr/0004-third-party-dependency-management.md`。

### 13.5 错误处理模型

- 决策：核心 C++ 用 `tl::expected`（通过 `adm::Result<T>` alias），未来标准库 `std::expected` 普及后迁移成本为零。
- 公共边界（`adm_c_api`、`adm_core` 公共 API）禁止 throw；C++ 异常仅限模块内部。
- `adm::ErrorCode` 与 `adm_error_code_t` 一对一映射，`static_assert` 强制保证。
- 详细记录：`docs/adr/0005-error-handling-model.md`。

### 13.6 CLI 参数库

- 决策：采用 CLI11，与 M1 既成事实一致。
- CLI11 类型不出 `src/adm_cli/`，符合 ADR 0003 边界。
- 旧 ObjC CLI 选项名兼容延迟到 M5 单独 ADR。
- 详细记录：`docs/adr/0006-cli-argument-library.md`。

### 13.7 C ABI 稳定性

- 决策：两阶段稳定模型——M1~M3 为实验期（任意修改），M3 完成首个端到端渲染后切 v1 稳定期。
- 稳定期遵循 semver + SOVERSION；append-only struct、enum 末尾追加、deprecated 函数至少保留 2 个 minor 版本。
- opaque 指针、字符串所有权、callback 生命周期约定写入 `include/adm/c_api.h` 注释。
- 详细记录：`docs/adr/0007-c-abi-stability-policy.md`。

## 14. 待决策问题

下列问题推迟到对应里程碑实施前再补 ADR，避免在缺乏上下文时空泛决策：

- **M3 决策**：`libsndfile` 是否进入默认依赖（M2 只走 BW64/WAV，`libbw64` 已足够）。
- **M3 决策**：CAF 是否继续作为跨平台核心能力，还是移入 `adm_apple`（取决于 IO 后端实现深度）。
- **M3 实现前**：`IRenderer::supports()` 返回的 `CapabilityReport` 接口形状（第一个后端接入前必须定型）。
- **M3 实现前**：Golden fixture 与回归测试策略（M2 scene import 用 dump 文本对比即可；M3 出音频后才需正式策略）。
- **M3 完成后**：Rust 首批试点模块选择（ADR 0002 已列候选区域，需要 M3 跑通后才有依据）。

下列问题已不再悬而未决：

- **新旧 CLI 二进制名称**：正式 CLI 二进制名为 `mradm`，避免占用通用标准名 `adm`；不保留 `adm` 兼容入口。

## 15. 下一步建议

1. 继续在 `docs/adr/` 记录依赖管理、CLI 参数库和错误处理模型选择。
2. 梳理当前 CLI 所有选项，生成兼容性表。
3. 选取 3 到 5 个小型 ADM BWF 样本作为第一批 golden fixtures。
4. 新建 CMake skeleton，不替换现有 XcodeGen 工程。
5. 先实现 `probe` 和 `scene dump`，再进入渲染闭环。

## 16. 参考资料

- `libbw64`: https://github.com/ebu/libbw64
- `libadm`: https://github.com/ebu/libadm
- `libear`: https://libear.readthedocs.io/
- `Spatial_Audio_Framework`: https://github.com/leomccormack/Spatial_Audio_Framework
- 本仓库 SAF 链接策略：`docs/SAF_LINKING_AND_LICENSE.md`
- 语言路线 ADR：`docs/adr/0002-cpp-first-rust-later.md`
- 领域模型与后端边界 ADR：`docs/adr/0003-owned-domain-model-and-backend-boundaries.md`
