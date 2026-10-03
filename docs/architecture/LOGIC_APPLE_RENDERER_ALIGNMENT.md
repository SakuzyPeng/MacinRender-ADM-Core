# Logic Pro Apple Renderer 对齐：本机静态证据

2026-09-26；Logic Pro Creator Studio **12.3.1 (6682)**；macOS **27.0 / 26A428**；arm64。

最初一轮直接读取本机应用二进制和项目源码，没有查找外部渲染说明，没有启动或附加 Logic，
没有生成试听音频，也没有修改产品渲染代码。以下区分静态控制流、配置写入与尚未实测的行为。

后续针对 diffuse / divergence 完成了字段引用追踪、独立进程中的原生解析器验证和导出辅助函数验证，
见 [Logic ADM diffuse / divergence 调查](LOGIC_ADM_DIFFUSE_DIVERGENCE.md)。

## 结论

**Logic 的 AppleSpatializer 和本项目确实选择同一个 AudioComponent 身份：
`aumx / 3dem / appl`，即 AUSpatialMixer。两边的输入组织和算法配置不同。**

Logic 的 DolbyMixer 代码先执行其 AUSinkPlugIn 渲染路径，再在监听模式要求时将输出交给
AppleSpatializer；后者配置为 AmbienceBed。这支持“先生成多声道声场，再由 Apple 空间化”
的两级路径，而不是把每个原始 ADM 对象直接作为 SpatialMixer PointSource。
具体项目运行时的中间布局、实际生效参数和最终 PCM 仍需采集，不能仅凭静态代码断定固定为 7.1.4。

我们当前 Apple 双耳路径则将 ADM Objects 展开为独立 PointSource bus，直接使用 HRTFHQ；
extent 和 divergence 已在主机侧变成多个相干点源。仅替换一个 HRTF 选项不足以证明整体对齐。

## 组件和配置证据

二进制：
`/Applications/Logic Pro Creator Studio.app/Contents/Frameworks/MAAudioEngine.framework/MAAudioEngine`。
以下地址为 arm64 文件虚拟地址，不是运行时加过 ASLR 的地址。

| 位置 | 静态观察 |
| --- | --- |
| `AppleSpatializer::CreateRemote`，`0x8da760` | 组装 `aumx/3dem/appl`，经插件工厂创建远端 AU |
| `AppleSpatializer::SetCustomAudioUnitProperties`，`0x8d9f5c` | 枚举 factory presets；找到编号 1 后写 `PresentPreset`（36） |
| 同函数，`0x8da020` 起 | `SpatialMixerOutputType`（3100）= 1，Headphones |
| 同函数，`0x8da040` 起 | input element 0 的 `SpatializationAlgorithm`（3000）= 7，UseOutputType |
| 同函数，`0x8da064` 起 | input element 0 的 `SpatialMixerSourceMode`（3005）= 3，AmbienceBed |
| 同函数，`0x8da088` 起 | input element 0 的属性 3214 = 1；本轮未确认其含义 |
| 同函数，`0x8da0a8` 起 | `SpatialMixerPersonalizedHRTFMode`（3113）初始化为 0 |
| 同函数，`0x8da0c8` 起 | 头追属性 3111 从配置状态取值 |
| `AppleSpatializer::Load`，`0x8d947c` | 实际调用上述配置函数 |
| `AppleSpatializer::SetControlValue`，`0x8d87dc` | 后续控制路径还会写 preset 和 3113；初始化值不能代表全部 UI 模式 |

这些是二进制中的调用和写入意图；尚未观察实际调用返回值或最终属性回读。
常量名称按本机 SDK 头文件对照，未将不同算法名称等同为不同/相同的实测脉冲响应。

项目对应位置：

- `src/adm_apple/spatial_mixer_renderer.cpp`：`create_spatial_mixer_unit()` 选择同一组件。
- `configure_spatial_mixer_unit()`：默认不启用 factory preset；每个对象 bus 为 mono。
- `AppleRenderer::render_window()` 与 `AppleStream::create()`：双耳算法均固定为 HRTFHQ（6）。
- `build_bus_plans()` / `object_block_events()`：Objects 为 PointSource；divergence × extent 展开为相干 bus；
  双耳 DirectSpeakers 默认逐轨使用 AmbienceBed，并非已确认与 Logic 相同的多声道单总线布局。

## 两级处理的控制流

`SpatialAudio::DolbyMixer::Load`（`0x7fa3d8`）在 `0x7fa58c` 构造
`AppleSpatializer`，并在 `0x7fa590` 将其保存到对象字段 `+0x390`。

`SpatialAudio::DolbyMixer::Process`（`0x7f9ef4`）：

1. 在 `0x7f9fa4` 调用 `AUSinkPlugIn::Process`。
2. 随后读取监听模式表的 `+0x30` 标志；同一标志也由
   `DolbyMixerParameterFormat_MonitoringFormatV0::UseAppleSpatializer` 读取。
3. 标志启用且 `+0x390` 非空时，把前一阶段的输出列表作为后一级输入，调用其 Process 虚函数。
4. `AppleSpatializer::Process`（`0x8d8de4`）再委托远端 AU。

应用中同时存在 Dolby Atmos SDK、`Noon::Renderer`、对象元数据缓存和 BRM 管理代码。
这些符号不能单独证明每个监听模式执行了哪些 Dolby DSP 内核。
本轮确认两级串接结构，没有完整反推上游声像、size 滤波、trim 和 downmix 数学。

## ADM 对象语义：已定位的导入映射

二进制：`.../Frameworks/Logic.framework/Logic`。

`-[ADMImporter createAutomationEvents:forAudioBlockFormats:withClockOffset:startTime:]`
位于 `0x621de4`。Objective-C selector stub 已按指令和 selector reference 解析，
注释后的反汇编保存在本地证据目录。

- **坐标与时间**：读取 block `rtime`，按坐标表示生成位置自动化。Cartesian 分支写三个参数；
  polar 分支写方位、仰角、距离对应的三个参数。尚未验证坐标转换、量化和运动包络。
- **Cartesian extent**：`0x622000` 起比较 width/height/depth；不相等时打印警告，随后仍取 width，
  写入参数 19（size）。没有 width 时写 0。`initialSize`（`0x61edf8`）同样读取首块 width。
- **gain**：`0x622110` 起读取 `hasGain`、`gainUnit` 和 gain，创建音量自动化；还存在
  Dolby metadata 相关诊断分支。未验证有效幅度、对象级 gain 及与其他增益的叠加关系。
- **diffuse / objectDivergence**：后续调查确认 diffuse 可被 MAFiles 解析，但未进入已追踪的工程自动化；
  objectDivergence 在所测 Cartesian / polar 输入中被解析器忽略。Logic 导出的 diffuse 标记由 size
  经迟滞判断生成，详见上方专项调查。后续六组 Cartesian 夹具的真实 Logic PCM 对照也支持
  diffuse / divergence 在本导入路径不影响输出，而 size 影响输出；具体边界见专项调查。
- **其他字段**：上述导入映射中未发现 channelLock、screenRef、headLocked、headphoneVirtualise、
  jumpPosition / interpolationLength 对应的自动化处理。MAFiles 的解析类确实包含其中一些字段，
  但“能解析”不能证明“参与渲染”；仍需字段隔离实验。

尺寸警告字符串位于 `Logic` 的 `0x1d7fc33`，已直接读取：

> ImportADM: Warning - width/height/depth should be identical, using width and ignoring height/depth

这条结论适用于已检查的 Cartesian ADM 导入分支。它不证明所有坐标表示、原生 Logic 工程或
其他版本均采取同样映射，也没有确定 Dolby size 内核的全部行为。

## 建议的验证顺序

1. **冻结目标条件**：同一 Logic/macOS、监听模式、48 kHz、标准或个性化 HRTF、头追状态、
   normalization 和输出链。区分 Logic 内部双耳 PCM 与设备侧最终输出。
2. **先对齐床层空间化**：以完全相同的多声道 PCM 输入两边，确认 Logic 实际输入 layout、
   channel order、preset、属性回读和延迟。项目侧用一个有正确 channel layout 的多声道
   AmbienceBed bus，试验 UseOutputType 与对应 preset；记录属性 3214，不猜测其含义。
   用单声道轮流激励、长 PRBS/扫频与充分预滚分离通道映射、频响、耳间时差和房间尾音差异。
3. **再对齐对象到中间声场**：用同源 ADM 逐个测位置、size、运动、gain 和 LFE。
   已有 DAR room-compat 点源/size 研究是候选起点，但其参考是独立 Renderer 5.5，
   不能未经比较就宣称等于 Logic 内嵌 Dolby SDK。
4. **字段隔离**：尤其比较只改 width、只改 height、只改 depth，以及 diffuse 0/1、
   divergence、锁定和 BRM；区分 Logic 导入时丢弃、映射为其他参数、以及渲染器内部处理。
   本项目侧优先使用 semantic policy 和 semantic report，保留源 ADM。
5. **最后做节目 A/B**：上述层级通过后，再比较完整作品的定位、宽度、外化和音色。
   所有项目生成的比较音频使用 Release；不能只凭同响度或整体 EQ 接近判断空间语义对齐。

## 本地证据

`local/logic-apple-renderer-20260926/`（忽略目录，约 300 KiB）：

- `binary.json`、`logic-binary.json`：原二进制路径、版本/构建号和 SHA-256。
- `static-disassembly.txt`：MAAudioEngine 相关函数的原始 LLDB 静态反汇编。
- `adm-import-disassembly.txt`：Logic 导入函数的原始反汇编。
- `adm-import-annotated.txt`：增加 selector 名称的导入反汇编。

未复制应用、媒体或 HRTF 资源；没有新建工作区或构建缓存。上述证据是下一轮运行时验证的起点，
不是已完成听感或 PCM 对齐的报告。

## 后续：无窗口 bounce 的候选入口

**更新：已实际打通驻留 Logic 的无对话框并轨，并完成 GUI 输出对照与新 ADM 探针验证。**
使用方法和证据见 [Logic 无对话框并轨](LOGIC_HEADLESS_BOUNCE.md)。下面保留最初入口定位。

本机 `sdef` 返回的 AppleScript 字典只包含 Standard Suite、Text Suite 和类型定义，
命令为 close/count/delete/duplicate/exists/get/make/move/open/print/quit/save/set；
没有 bounce/render/export 命令。应用文件和已扫描入口中没有找到现成的独立 bounce CLI。
这不排除尚未发现的私有自动化通路。

静态定位到以下候选：

- Logic.framework，`0x9d20a8`：
  `-[DfDocument doBounceToFile:startClock:endClock:addEffectTail:useNormalizer:format:useCurrentSongSampleRate:use24BitsIfPossible:createFades:aacMetaData:outNormalizeCompensatedVolume:aacSilenceFramesAtStart:]`。
  能直接接收目标路径、时间范围及导出选项。函数还访问真实工程、主轨与自动化，并有临时设置和恢复逻辑；
  它不是仅传一个文件名即可在空进程中使用的静态工具函数。
- MAAudioEngine，`MD::OfflineBounce`（`0x6ef1d0`）与 `MTEngine::OfflineBounce`：
  现成的离线引擎处理入口，但依赖已初始化的宿主/工程状态。
- MAAudioEngine，`SpatialAudio::PlugInFactory::RegisterInternalAUs`（`0x8e1368`）：
  配置允许时通过 `AudioComponentRegister` 注册 `RenderPlugFactory`。
  这是独立进程宿主复用内嵌渲染器的线索，不是已完成的独立 Logic 工程 bounce。

优先路线是让 Logic 在用户会话中驻留，将任务排到其主事件循环，绕过导出对话框调用实际
工程 bounce。已有 DAR 私有网关批处理可参考调度、结果核验和状态恢复方式，不能直接复用其 ABI。
首先需要用独立探针工程验证目标格式、Apple Renderer 是否在 bounce 链中生效、导出文件完整性，
再与正常界面 bounce 对照。后续实际落地调用的是下面一层共用并轨入口，已完成该对照，
并未直接把 DfDocument 的便捷导出方法当作全部并轨设置的等价接口。

另一条路线是独立加载内嵌 Dolby 渲染器并接系统 SpatialMixer，制作离线命令行宿主。
它更适合大量 DSP 探针，但需另外验证与 Logic 的语义导入、自动化、路由和预设一致，
不能自动当作完整 Logic bounce 的证据。

本地补充材料为 `scripting-definition.txt`、`Logic-bounce-symbols.txt`、
`MAAudioEngine-bounce-symbols.txt`、`document-bounce-annotated.txt`、`offline-entry-disassembly.txt`。
