# Dolby Atmos Renderer 5.5 的 22.2 可行性初查

2026-09-25，macOS 上安装的 Dolby Atmos Renderer **5.5.0 release**。
本轮研究的是接受 ADM/DAMF 的独立 Renderer 产品，不沿用系统 AudioCodecs OAR 的能力结论。
原始记录、导出的设置和临时配置位于 `local/dar-222-20260925/`。

## 当前结论与证据边界

**标准 re-render 不能直接选择 22.2；底层影院／物理扬声器路径是否能实现真正 22.2，尚未验证。**
9.1.6 是当前 re-render 的最大常规声道布局，不能据此把整个 Renderer 引擎的能力上限也定为 16 声道。

后续已通过运行中 Renderer 的 Qt 后端网关，在**不显示导出窗口**的情况下直接读取同一份 ADM，
离线导出真正的 7.1.4（12ch）和 9.1.6（16ch）PCM。这证明标准 re-render
的无窗口导出路径可用，但没有增加 22.2 选项。调用和定量对照见
[原生 22.2 可行性记录的同源 ADM 校准节](ATMOS_NATIVE_222_FEASIBILITY.md)。

与此前系统 OAR 的直接函数调用研究不同，本轮未对独立 Renderer 的内部 panner 做同等级别的几何模型验证。
没有输出 22.2 音频，不能把生成了 24 个扬声器条目的配置文件当作渲染成功。

## 已确认的产品入口

1. **Re-renders 窗口**：实际打开 Layout 下拉框，包含 5.1、5.1.2、5.1.4、7.0、7.0.2、7.1、
   7.1.2、7.1.4、9.1.4、9.1.6、BIN、AmbiX、Loudness 等条目，没有 22.2。
   同版本 HTML 用户指南 `t_config_outputs.html` / `c_simultaneous_outs.html` 也没有列出 22.2。
2. **Room Setup**：普通物理房间界面提供 22 个可选扬声器位置，包含一个 LFE。
   “22 个可选位置”不是“22 个全频声道＋2 LFE”。当前界面没有标准 22.2 的三个下层扬声器。
3. **Array mode**：临时打开未提交的阵列预览，确认可以增加侧面／后方阵列成员；随后取消。
   这扩展物理输出数量，但没有直接提供三层 22.2 几何。
4. **影院／物理布局线索**：应用包内存在 Cinema `ISpeakerLayout` 相关类型，保存的房间数据也有
   `cinemaPhysicalLayout`、`cinemaRoomMetadata` 等字段。随附 XSD 允许为逐个 speakerEndpoint 指定 XYZ。
   这些是值得继续研究的入口线索，不是 22.2 已可用的证明。

《Dolby Atmos Renderer Installation and Configuration Guide》v5.5 第 92–93 页说明并展示普通房间的
22 个可选扬声器；第 115–119 页说明阵列的成员和用途。本轮阅读了相关完整页面并检查第 93 页布局图。

## DAC 导入控制实验

HTML 用户指南 `t_loading_sys_settings.html` 写明，Import Global Settings 可以导入 `.atmoscfg` 或影院 `.dac`。
然而本机实际文件窗口及应用内嵌 QML 的导入过滤均只接受 `*.atmoscfg`。

先通过应用导出当前全局设置，得到 `dar-global-before.atmoscfg`，实际文件为 SQLite 设置库。
研究脚本 `scripts/research/dar_layouts/make_dac_probe.py` 依据应用随附 XSD 生成两份临时 XML：

- `control-714.dac`：12 个物理扬声器的普通 7.1.4 控制。
- `target-222.dac`：24 个物理扬声器，按项目 `9+10+3` 的标称角度设置 9 上层、10 中层、3 下层和 2 LFE。

两份均通过 `xmllint --schema AtmosConfigSuiteData.xsd` 验证。
这只证明 XML 符合格式描述，不代表已满足 Renderer 的全部语义约束。

实际 UI 结果：

- 选择 `control-714.dac` 时，系统文件窗口提示要求 `.atmoscfg` 扩展名，尚未进入可观察的房间渲染验收。
- 将同一 XML 内容复制为 `control-714.atmoscfg` 作扩展名控制，未取得成功导入状态。
- 因普通布局控制也未通过，不能把本次结果归因于“22.2 被拒绝”，更不能据此判定底层算法绝不支持它。
- `target-222.dac` 没有作为成功导入的房间运行，未执行 ADM → 22.2 渲染。

文档与本机导入菜单的实现存在差异；后续实验已确认菜单只进入 `.atmoscfg` 设置恢复路径。
影院配置入口为什么没有在当前界面提供，仍未查明；没有修改模式／授权字段来猜测。
当前保存的数据中 `homeOnlyMode` 为 true，本轮仅观察了该字段，未修改。

### 后续定位：失败发生在两个不同入口

用 Renderer **自己导出**的 `dar-global-before.atmoscfg` 作对照，同一菜单可以正常选择并完成重载。
该文件是含 `dbinfo` / `entries` 表的 SQLite 数据库。重载后界面恢复可操作，房间与 re-render
配置条目和备份逐字节相同；记录见 `baseline-reload.json`。

本机程序内嵌的当前菜单 QML 明确写着：

```text
nameFilters: [qsTr("filter:atmos_config_files") + " (*.atmoscfg)"]
onAccepted: importExportSettingsGateway.importSettings(fileUrl)
```

因此 `control-714.dac` **先在文件选择器被挡下**。macOS 的实际提示是必须使用 `.atmoscfg` 扩展名。
另一份与控制 DAC 内容逐字节相同、只改名为 `control-714.atmoscfg` 的 XML，
在正确选择文件后已经进入设置导入器，但 Renderer 返回 **“Unknown error / An unknown error has occurred.”**
应用导出的 SQLite 文件成功、XML 文件失败；用只读 SQLite 查询 XML 也得到 `file is not a database`。
这组对照表明当前菜单是 `.atmoscfg` 设置恢复路径，不能通过改扩展名来加载 XML 影院配置。

程序还包含另一个 `ImportAtmosConfigGateway`，其元对象声明了 `openAtmosConfig` 方法，
但当前菜单用的是 `ImportExportSettingsGateway`。这支持“影院配置有独立入口”的判断；
本轮尚未找到本机当前模式下能成功调用该入口的受支持 UI 路径。

后续已在运行中的 Renderer 进程内定位并调用该网关的 `openAtmosConfig(QUrl)`，
先提交普通 `control-714.dac` 控制配置。调用记录 `gateway-control-714-call.json`
确认对象存在且方法被调用；应用明确报出该文件不是有效的 Dolby Atmos 配置，未加载房间。
同一控制文件虽通过随应用提供的 XSD 校验，仍可能缺少产品语义字段或不适用于当前模式。
`target-222.dac` 未提交给网关，因而没有 22.2 渲染结论。
调用后的 `after-cinema-gateway.json` 确认房间与 re-render 设置仍与备份相同。
配置中的 `homeOnlyMode=true`，Preferences 界面呈现 Home theater 设置。
目前无法确定这一入口差异是模式、版本回归、产品许可，还是文档覆盖其他配置。
不应把导入失败归因为 XML 的 22.2 坐标或 Renderer 音频引擎的扬声器上限。

失败后再次只读核对，key 251 房间配置与 key 24 re-render 配置和备份相同；
其他变化仅记录为 key 81、306（本轮未解释其用途），见 `xml-rejection-state.json`。
Renderer 无打开的 master，未播放或生成音频。

## 状态保护

本轮开始时 Renderer 没有打开 master，未启动播放。
Room Setup / Re-renders 的预览均取消，未保存新的扬声器或 re-render 配置。
退出导入窗口后，用只读 SQLite 查询将当前持久化设置与应用导出的备份比较：

- 房间设置条目 key 251：字节完全一致。
- re-render 设置条目 key 24：字节完全一致。

见 `state-preservation.json`。这项检查针对上述两个配置条目，不声称文件浏览历史等所有应用数据也没有变化。

## 值得继续验证的路线

真正的候选路径是 **ADM → Renderer 的影院／物理扬声器布局 → 多声道输出采集**。
应先取得一个能被当前产品成功加载的普通影院配置，再尝试 22.2，并验证实际坐标、下层与顶部中轴输出、
双 LFE 处理及对象运动。这一路未必经过标准 re-render 导出。

如果继续研究内部接口，应单独确认 Cinema panner 的实际几何和调用条件，不能从一个配置 schema、
类型名或扩展名错误直接推导结论。当前可对外承诺的仍是已有 re-render 格式，不能承诺 22.2。
