# 原生 Dolby OAR 真正 22.2 的可行性验证

2026-09-25，macOS 27.0 / 26A428，系统 AudioCodecs arm64e。
证据位于 `local/atmos-222-20260925/`，研究工具位于 `scripts/research/atmos_playback/`。

## 结果

**本轮没有打通真正的 22.2。限制已经定位到当前 OAR 扬声器几何模型，而不只是 AudioConverter 的声道数量或布局白名单。**

已经绕过 AudioConverter 的格式选择，直接调用系统 OAR 的配置查询和初始化：

- 原生 24 声道实例初始化成功，所需内存 469,391 字节。
- 该实例的真实位置是 13 个平面点、10 个高度点和 1 个 LFE，不能标记成 22.2。
- 开启所有配置位后，扬声器集合有 35 个标识；排除 LFE 占位值后，空间点仍只有两个高度层。
- 这不是“24ch 音频已经正确生成”的声明；本控制验证了配置和初始化容量，没有把其他几何冒充 22.2。

本轮没有生成可交付的原生 22.2 音频，没有修改产品渲染器或系统二进制。

## 验证过程

先用只读 LLDB 观察实际 5.1.2、7.1.4、9.1.6 解码时传给 OAR 的 48 字节配置。
不同布局使用不同的扬声器组 mask，其余控制保持一致。

随后对本机系统组件的独立临时副本进行 IDA / Hex-Rays 分析，按函数名核对：

- `oar_query_memory` / `oar_init_safe`
- `speaker_config_init` / `speaker_config_count` / `speaker_config_positions_get`
- `room_config_init` / `point_panner_get_gains`

原生调用探针 `oar_layout_probe.cpp` 校验系统组件完整 SHA-256 后，
通过该组件自己的 Mach-O 符号表定位已有函数，只在自建进程中调用它们。
没有改写函数、返回值、位置表或其他进程。

ABI 控制先用真实 7.1.4 mask 验证得到 12 个扬声器；5.1.2 / 7.1.4 / 9.1.6 的原生内存查询结果
分别为 223,380 / 266,415 / 315,247 字节，与实时调用栈中观察到的分配量一致。

## 配置容量与几何是两个不同的问题

| 配置 | 扬声器数 | 原生内存查询 | 验证 |
| --- | ---: | ---: | --- |
| 5.1.2 | 8 | 223,380 B | 与真实解码配置一致 |
| 7.1.4 | 12 | 266,415 B | 与真实解码配置一致 |
| 9.1.6 | 16 | 315,247 B | 与真实解码配置一致 |
| 一个额外原生组合 | 24 | 469,391 B | `oar_init_safe` 成功；几何不是 22.2 |
| 全部配置位 | 35 | 692,727 B | 扬声器集合／位置与内存查询；未在此配置渲染节目 |

24 声道控制排除了“算法只能初始化到 16 声道”的解释。
但标准 22.2 是 **9 上层＋10 中层＋3 下层＋2 LFE**，不是任意 24 个输出槽位。

## 几何模型的具体限制

原生 `speaker_config_positions_get` 从固定位置表生成坐标。全配置位实测得到的 35 个标识中：

- 一个 LFE 使用非空间占位坐标，不能解释成下层扬声器。
- 非 LFE 的垂直坐标只有 Q15 的 0 和 32767，即两个层面。
- 高度点是五对左右点，没有 22.2 所需的顶部前中、顶中、顶部后中三个中轴点。
- 没有 22.2 的三个下层空间点，也没有第二个独立 LFE 标识。

这不只是一份白名单缺少 tag。`room_config_init` 按固定扬声器集合建立平面和高度两个分支，
`point_panner_get_gains` 在这两层之间计算垂直增益；源高度小于等于零时落在平面分支。
当前这组 OAR 配置字段中没有任意扬声器坐标数组入口。

因此，把额外 mask 填进 ACDDPAtmosDecoder 的布局表最多开放更多现有点位，
不能补出真正的三层 22.2。只把声道重新命名为 CICP 13 也不会改变对象 panning 的几何。

本结论限定于本机这份系统 OAR 实现和已核对接口，不外推为所有 Dolby 产品或未来系统版本的能力上限。

## 可继续的实现方向

真正的 22.2 需要支持相应几何的对象渲染环节：

1. 保留系统的 JOC 对象重建及对象元数据解析。
2. 将重建对象和元数据交给支持真实 22.2 的渲染器，或扩展自有 panner。
3. 明确处理单 LFE 输入到双 LFE 输出的策略，并验证下层、上层中轴和对象运动。

这条方向能保留原生解码，但需要更换／扩展扬声器渲染模型；本轮还没有实现该对象桥接。
它也不再等同于“完全沿用现有 Dolby OAR 输出未支持的布局”。

### 同源 ADM 校准自有 22.2

另一条独立路线是以 Dolby Atmos Renderer 的 7.1.4、9.1.6 **离线 re-render**
校准本项目的通用对象 panner，再把该模型应用到已有的三层 22.2 布局。
该 Renderer 的用户指南明确支持读取 ADM BWF 并离线导出 re-render；
这比拿 ADM 对照 E-AC-3 JOC 播放更容易隔离 JOC 有损编码和对象重建的差异。
两端必须读取同一份 ADM，先核对节目时间、声道顺序、归一化／限幅、bed/LFE 路由和扬声器位置，
然后分别测静态点源、size/extent、增益与移动包络。

可直接复用相邻 `MacinDecode-AC4-Core` 仓库的测试向量链：
`scripts/gen_damf.py` 从 `case.json` 生成确定性对象 PCM 与 DAMF，
现有 ADM 规范化步骤生成 ADM BWF/WAVE。已存在的
`vectors/probe_axes_single_object/normalized/output.wav` 是 48 kHz、11 输入声道、6 秒的 ADM BWF，
其中静音 7.1.2 bed 后接一条六段坐标探针；本项目 Release `mradm inspect` 已正确读出对象与全部六个坐标。
该素材现已用于两端离线渲染。后续仍需增加固定方位、下层、顶中、size 与运动案例，
才能完整拟合声像算法。

7.1.4 与 9.1.6 的吻合只能验证这两种几何上的行为；两者均缺少 22.2 的下层空间点、
顶部中轴点和第二个 LFE，不能从其输出唯一推导 Dolby 22.2 增益。
实现上应校准可接受任意扬声器坐标的规则，而非拟合固定声道矩阵；
22.2 的下层三角化、顶部中轴、层间连续性与单 LFE 到双 LFE 的策略需要独立测试。
最终可以称为“以 Dolby 常规布局为参照的自有 22.2”，不能称为 Dolby 22.2 的逐声道复刻。

#### 同一 ADM 的无窗口离线导出：已成功

本机 Renderer 5.5 安装目录中未找到独立的 re-render 命令行程序；
其 OSC 文档只列传输与监听控制，没有打开 ADM 或离线导出命令。
Dolby Atmos Conversion Tool 的 `cmdline_atmos_conversion_tool --help` 提供的是
Atmos 母版格式／帧率转换，其 `wav` 输出是 ADM BWF 母版，不是 7.1.4／9.1.6 扬声器 re-render。

实际可用路径是让 `QMLGatewayFactory.createGateway(QString,QObject*)` 在 Renderer
自己的事件循环执行，以主窗口作为父对象创建**不显示窗口**的
`RerenderExporterGateway`。同步 LLDB 表达式曾报内部断点，排队到事件循环后创建成功。
内嵌 QML 的正常调用也确认 `MasterFileGateway.openMaster(fileUrl, "")`、
`RerenderExporterGateway.setOutputDir(QUrl)` 和 `startExport(QString)` 的参数。
整个导出只使用现成 ADM BWF；没有经由 Atmos 编码码流或播放器。

使用上述六段单对象 ADM，Renderer 5.5 实际离线导出：

| 布局 | WAV | 验证 |
| --- | --- | --- |
| 7.1.4 | `local/dar-calibration-20260925/probe-axes-dar-714_re-render 02.wav` | 48 kHz、12 声道、24-bit、288000 帧，6 秒 |
| 9.1.6 | `local/dar-calibration-20260925/probe-axes-dar-916_re-render 02.wav` | 48 kHz、16 声道、24-bit、288000 帧，6 秒 |

同一 ADM 经 Release `mradm render --renderer saf --speaker-geometry apple`
产生了两种对照 WAV，均关闭峰值限制、spread 和默认位置插值。
`scripts/research/dar_layouts/compare_adm_probe.py` 在各一秒片段的 0.1–0.4 秒稳定区间
计算逐声道 RMS；机器报告和四份 WAV 位于 `local/dar-calibration-20260925/`。

初测静态点源差异并非整体增益：两端多数片段总功率均约 `0.031548`，
但 X-min 在 Dolby 7.1.4 只进入 ch6（RMS `0.177617`），当前 SAF 还分给 ch0
（ch6/ch0 为 `0.165201/0.065243`）；9.1.6 中 Dolby 只进入 ch4，SAF 分给 ch4/ch8。
Z-max 顶点更明显：Dolby 7.1.4 平分顶部 ch8–11、9.1.6 平分顶部 ch12–13，
而当前 SAF 在两个布局的该片段几乎全静音。`origin=(0,0,0)` 在两端也明显不同，
但原点的方向语义不宜直接用来拟合点源 panner。
这些声道编号来自该次多声道 WAV 的实际槽位；9.1.6 文件没有被 CoreAudio 读出标准布局标签，
完整声道语义还需另行核实。独立 Dolby Atmos Renderer 的 ADM re-render
也不能自动等同于 Apple AVPlayer 内部 OAR 输出。

7.1.4 试验临时把当前唯一 re-render 条目从 9.1.6 改为 7.1.4，完成后改回；
持久化数据库的 re-render key 24 和房间 key 251 与修改前逐字节相同。
原导出目录和名称已恢复，无窗口导出网关已释放，实例计数为零；Renderer 仍运行。
清单、QML 源、状态快照和设置备份在 `local/dar-222-20260925/`。
桥接源码在 `scripts/research/dar_layouts/`，依赖本机应用私有 Qt 类型，只作为研究工具。

现已整理成 `scripts/research/dar_layouts/run_headless_rerender.py`：输入 ADM 路径和空输出目录，
一次命令自动完成两种布局的导出、WAV 检查及状态恢复。
用同一探针完整复测的 `local/dar-batch-smoke2-20260925/run.json` 报告 `success=true`；
两份 WAV 的 SHA-256 均与上述逐步试验产物**逐字节一致**。
最终母版路径、原 9.1.6 条目和导出网关释放状态均核对通过；持久化 key 24、251
与脚本启动前相同。脚本和调用边界见 `scripts/research/dar_layouts/README.md`。

原始观测：`config-512.log`、`config-714.log`、`config-916.log`、`native-layout-survey-final.jsonl`。
结构化摘要：`summary.json`。临时系统副本和 IDA 数据库在分析结束后清理，保留生成脚本及本地分析证据。
