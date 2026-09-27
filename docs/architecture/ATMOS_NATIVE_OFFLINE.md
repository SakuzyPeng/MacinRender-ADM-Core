# 系统 Atmos 解码器的指定布局与离线渲染

2026-09-25，macOS 27.0 / 26A428。研究源码：
`scripts/research/atmos_playback/offline_probe.cpp`；原始证据：
`local/atmos-offline-20260925/`。

后续已进一步检查原生 OAR 的配置容量和扬声器几何：24 声道可以初始化，但现有两层位置模型
不能直接表达真正的 22.2，见 [22.2 可行性验证](ATMOS_NATIVE_222_FEASIBILITY.md)。

## 已实现的范围

**E-AC-3 JOC → 系统 Dolby 对象渲染 → 指定标准布局 PCM，可以脱离 AVPlayer 同步离线执行。**
本轮已制作可运行探针，离线 7.1.4 与此前实时 tap 在同一节目片段上非常接近。
此前“实时 tap 能取得 7.1.4”不意味着对象渲染本身必须依赖实时设备时钟。

这条路径以 E-AC-3 JOC 压缩包为输入，尚不是“将原始 ADM 对象 PCM 和元数据直接交给 Dolby panner”的接口。
本轮未接入产品 Core / GUI，也未改变现有 VBAP 后端。

## 普通离线创建失败的原因与实际入口

源文件通过公开 `AudioFile` 的 format list 同时声明 `ec-3` 兼容层和多种 `ec+3` Atmos 展现格式。
但是独立进程默认没有注册 `adec / ec+3` 组件，直接 `AudioConverterNew` 或
`AudioConverterNewSpecific` 均返回 `fmt?`（1718449215）。

对自建 AVPlayer 的只读断点显示，MediaToolbox 会调用 `AudioComponentRegister`，在**当前进程内**注册：

```text
type          adec
subtype       ec+3
manufacturer  appl
flags         2 = kAudioComponentFlag_SandboxSafe
factory       AudioCodecs!ACAC3DecoderNewFactory
```

之后 AVPlayer 使用 `AudioConverterNewSpecific`，codec selector 是 `adec / ec+3 / manufacturer=0`。
记录见 `registration.log`、`codec-selection.log`。这些是实际调用参数，不是根据符号名猜测。

独立探针在自己的进程中加载系统 AudioCodecs 组件、取得该工厂导出、复现上述注册，随后使用：

```text
AudioFile：原始压缩包、magic cookie、format list
  → AudioConverterNewSpecific（ec+3 → float PCM）
  → 设置 decompression cookie / output channel layout
  → AudioConverterFillComplexBuffer 循环
      → ACDDPAtmosDecoder
      → JOC + OAMD + Dolby OAR / OMG point、size panner
  → PCM / CAF
```

没有创建 AVPlayer、AudioQueue、AudioUnit 输出设备或运行实时播放循环。
离线进程的调用栈确实命中 JOC、`oar_process_safe`、`omg_panner_process`、
`point_panner_get_gains`、`size_panner_get_gains`，见 `offline-chain.log`。
没有修改系统组件、机器级组件注册或其他进程，也没有修改解码器寄存器、返回值或媒体。

工厂导出属于 Apple 私有实现，并非受到公开 SDK 稳定性承诺的 Atmos 解码接口。
公开的 `AudioComponentRegister` / AudioConverter API 不会使被借用的工厂变成公开 API。
目前只能视为经过本机验证的 macOS 研究／参考后端。

## 与实时 7.1.4 对照

输入仍为 `Tsuioku_SS2RPV4_ec3_1024K_drc_none.m4a`，48 kHz、E-AC-3 JOC。
离线从文件开头顺序解码到 64 秒，仅保存 decoded timeline 的 55–64 秒，避免大量临时文件。

本次优化编译探针的转换循环耗时 **0.702683 秒**，转换 3,072,000 帧（64 秒），其中写出 9 秒。
这是一次运行的观察，用于确认不受实时速度限制，不是完整的性能基准。

对照前一轮经过重复验证的 AVPlayer 57–62 秒参考：

| 指标 | 实测 |
| --- | ---: |
| 采样率 / 输出 | 48 kHz / 7.1.4 float32 |
| 对照长度 | 240,000 帧 × 12 声道 |
| 额外时间偏移 | 1536 帧，等于文件 packet table 的 primingFrames |
| 最大绝对误差 | 1.4901161193847656e−8 |
| RMS 误差 | 9.301076935260813e−11 |
| 信号／误差能量比 | 170.009 dB |
| 不同 float32 采样数 | 2940 / 2,880,000 |

结果不是逐位相同；本轮未定位微小浮点差异的具体原因。
但它将离线输出与真实 AVPlayer tap 定量对应起来，远强于仅验证初始化成功、12ch 标签或非零能量。

已交付对齐后的 `tsuioku-57s-62s-native-offline-714.caf`，5 秒、240,000 帧。
`afinfo` 确认布局、顺序、帧数；封装数据重新读回与对齐后的 float PCM 相同。
机器指标、逐声道差异计数及哈希见 `realtime-comparison.json`。

注意探针参数 `FIRST_SECONDS` / `END_SECONDS` 表示**解码器输出时间轴**，不是自动处理完容器 edit list 的节目时间。
本样本的 1536 帧偏移已验证；通用文件导出还需要正确处理 priming、remainder、edit list 和 EOF。

## 指定布局的实测范围

同一个原生解码器能够选择不同布局。以下六种均完成转换，实际格式／布局读回符合请求，
所有输出声道都有有限的非零信号，且不同布局的声道能量发生相应变化：

| 布局 | 声道数 | CoreAudio tag | 证据级别 |
| --- | ---: | --- | --- |
| 7.1 | 8 | MPEG_7_1_C | 格式与逐声道信号通过 |
| 5.1.2 | 8 | Atmos_5_1_2 | 格式与逐声道信号通过 |
| 5.1.4 | 10 | Atmos_5_1_4 | 格式与逐声道信号通过 |
| 7.1.2 | 10 | Atmos_7_1_2 | 格式与逐声道信号通过 |
| 7.1.4 | 12 | Atmos_7_1_4 | 另有实时 tap 定量对照 |
| 9.1.6 | 16 | Atmos_9_1_6 | 格式与逐声道信号通过 |

其余五种尚未取得各自布局的独立实时参考，不能把上表等同于完整空间语义验收。
`aocl` 查询返回其中五个 8–12ch 布局；9.1.6 未列在该属性结果中，
但源文件 format list 明确声明它，且 16ch 离线输出实测有信号。

**任意布局不能靠“设置成功”来保证。** 额外反例：

- `DiscreteInOrder | 12`：设置和回读都成功，但 12 路全静音。
- CICP 13 / 22.2：设置和回读都成功，但只剩五路非零，LFE 等内容没有正确保留。
- stereo / 5.1：API 接受且有输出，但不在本次 JOC 布局能力列表内；部分声道指标显示出取子集／重映射迹象。
  本轮没有证明这是正确的全场景降混，不作为可交付模式。

因此探针默认只开放上表六个布局。`ATMOS_ALLOW_UNVERIFIED_LAYOUT=1` 仅用于复现其他布局的诊断反例。
如需 stereo / 5.1，可另行验证，或从已验证的多声道输出做明确的后级降混。
任意自定义扬声器位置仍不在本轮打通范围内。

### 后续定位：回退格式与按标签取子集

继续对同一源曲 57–58 秒做逐采样比较，并观察实际传入 `AudioCodecInitialize` 的格式。
证据位于 `layout-diagnosis/`，特别是 `pcm-routing.json` 和 `codec-format-*.log`。

- 请求 stereo（2ch）：外层返回 2ch，但实际 Atmos 解码器初始化为 **8ch**。
  最终两个声道分别与独立 5.1.2 输出的 L / R **48,000 个采样全部相同**；其余声道没有混入。
- 请求 5.1：六路分别与独立 5.1.2 输出的前六路 **逐采样相同**，没有把两路高度声道降混进去。
- 请求 CICP 13 / 22.2（24ch）：外层返回 24ch，实际 Atmos 解码器仍初始化为 **8ch**。
  仅目标索引 2、6、7、18、19 非零，分别逐采样等于 5.1.2 的 C、L、R、Ltm、Rtm；其他目标均为零。
  这符合按共同声道标签取子集的结果，而不是对 24 个扬声器重新进行对象 panning。
- 请求 Discrete12：实际解码器为 12ch，并收到了离散声道 tag，最终仍全静音。
  同声道数的 Atmos_7_1_4 能正常输出，已经排除“12 声道数量太多”这种解释。
  离散编号本身没有扬声器空间位置语义；本轮尚未逐级定位这一路最终归零的确切指令。

因此 stereo / 5.1 / 22.2 的现象已从“可疑的声道指标”推进到“回退／子集路由”的直接证据。
它表明这条入口对非目标布局缺少正确的布局渲染／降混适配，不能仅凭 API 成功返回使用它们。

目前没有证据表明这些结果来自授权封锁或商业性禁用。确定性的回退、标签不匹配和补零，
可以是实现中的默认处理策略；这与故意把本来支持的声场功能锁住是不同的判断。
也不能反过来断言底层 Dolby OAR 算法绝无 22.2 或自定义布局能力：本轮证实的是当前封装入口的行为边界。

## 对 ADM Core 的接入含义

当前成果可以发展为 **macOS 原生 E-AC-3 JOC 离线解码／参考渲染后端**，也能帮助校准现有 VBAP。
压缩包输入已经带有 JOC 和对象元数据，系统解码器负责对象重建和 panning。

如果目标是把 ADM 对象直接接到同一 Dolby panner，还要单独恢复它的初始化、布局、对象 PCM 与元数据接口。
`ACAC3DecoderNewFactory` 可通过 `dlsym` 取得，但本机的 `oar_process_safe`、`omg_process`、
`point_panner_get_gains` 不作为可查找的导出符号提供；它们在调试符号中可见，不等于有稳定可调用 ABI。
不能把已经打通的“Atmos 比特流离线输出”说成已经替换了 ADM 渲染器。

在产品化之前，还需处理系统版本／组件变化、容器时间轴与 EOF、输出布局白名单、失败回退，
以及其他真实文件的验证。本轮只复用本机系统组件，没有复制或分发 Apple/Dolby 二进制。
