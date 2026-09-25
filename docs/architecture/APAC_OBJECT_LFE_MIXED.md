# APAC 对象＋LFE 混合管线实测

2026-09-25 本地产物已清理。本文的 `local/` 路径为历史运行位置；
精选结构化结果见[数值证据](evidence/apac_objects/README.md)。

验证日期：2026-09-23。macOS 27.0 / 26A428，AudioCodecs.component 7.0。

**混合管线可行。已实测通过 1 对象＋1 LFE，以及 23 对象＋1 LFE 的编码、默认原生解码和位置读回。**
编码使用私有 `acs ` 设置字典，编码能力表和解码器均保持系统默认，实际 Profile 为 5 / Level 0。
LFE 保存在带明确 LFE 标签的 Channel Bed 组件中，普通对象保存在 Object 组件中。

后续 [混合数量边界](APAC_OBJECT_MIXED_CAPACITY.md) 已验证默认编码的 `24 BED＋69 对象`、`127 BED＋1 对象`，
并分别报告单 BED 上限、多个 BED 的总量、组件数和诊断编码结果；所有解码保持系统默认。
后续 [码率研究](APAC_OBJECT_BITRATES.md) 已确认本轮独立 LFE 自动分配为零预算，
而显式值按组件生效；16 kbps 不是唯一可用值或最低可编码值。

## 1. 最小可用配置

1 对象＋1 LFE 共两个音频通道，外加一个 PCM 元数据输入通道：

| 输入索引 | 类型 | 设置 |
| --- | --- | --- |
| 0 | 单通道 Channel Bed | `ChannelLayoutLabel = ["kAudioChannelLabel_LFEScreen"]` |
| 1 | Object | `Object Count = 1`，位置走独立元数据 |
| 2 | APAC Metadata | AIA 1.4 PCM 元数据，不计入输出音频通道 |

`ChannelLayoutLabel` 的元素必须是该版本接受的完整常量名称字符串，不能用简写 `"LFE"`。
本轮确认 `kAudioChannelLabel_LFEScreen` 对应标签 4。

`Audio Scene Components` 声明输入组件及互不重叠的通道范围；`Codec Configurations` 声明同样的输出组件。
在后者为每个组件明确设置 `Bit Rate`：LFE 组件 16,000 bit/s，单对象组件 256,000 bit/s；
全局目标码率设为两者之和 272,000 bit/s。输入组件字典不添加输出专用的 Bit Rate 字段。

对照显示，单独 LFE 组件采用本轮自动分配设置时，初始化成功，但首次 ProduceOutput 返回 `-50`，
日志定位到 bitstream preparation。只把全局码率降至同样的 272,000 bit/s 仍失败；
显式分配组件码率后编码成功。尚未穷举所有码率选项，因此不宣称 16 kbps 是唯一值或最低值。

另外，自动分配下的「C＋LFE 两通道床＋对象」也已通过；这并不意味着必须额外占用一个主声道。
独立 LFE 组件的显式码率路径已经解决本轮最小配置的失败。

## 2. 对象元数据和音轨身份

混合场景的 renderer metadata 包含一个声道床组，随后是各对象组。
本轮 AIA `apdd` 更新对床组写“不更新”，保持其固定声道标签；对象组从 ID 1 开始，分别写位置和时间。
床中即使含 C 和 LFE 两个音频通道，也仍是一个 renderer group，不能按所有音频通道数生成对象位置列表。

原生解码器只读取压缩数据与 magic cookie，未从容器附加坐标或原输入轨迹恢复标签／位置。
1 对象＋LFE 的解码布局为 `[4, 262144]`，即 LFE 和对象；C＋LFE＋对象为 `[3, 4, 262144]`。
实际调用观察同时命中 `WriteLFE` 与 `APACLFEElement::Deserialize`，证明使用了专用 LFE 编解码路径。

较多对象仍采用每个 Object 组件不超过 7 个的拆分方式，以使用默认能力表。
23 个对象拆为 `7+7+7+2`，加一个 LFE 声道床后，输出正好 **24 个音频通道**，输入另需一条元数据通道。
目标码率为 LFE 16 kbps 加每对象 256 kbps，共 5.904 Mbps；这是实验目标值，不保证实际文件平均码率相同。

## 3. 音频、位置与低通对照

测试源给 LFE 输入同时放入 60 Hz 和 1 kHz、各峰值 0.05 的合成信号，后者专用于识别低通行为。
对象使用互不相同的全频单音；另有把同一低频测试输入标为普通 Right 声道的对照。
以下幅值检查在稳定的 0.2～0.8 秒窗口完成，音频帧数检查覆盖整个输出。

| 用例 | 有效音频帧 | LFE 60 Hz 增益 | LFE 1 kHz 增益 | 对象位置 |
| --- | ---: | ---: | ---: | --- |
| 1 对象＋独立 LFE，显式组件码率 | 48,000 | 约 -0.087 dB | 约 -90.43 dB | 读回 59.765625°，输入 60° |
| 移动对象＋独立 LFE，显式组件码率 | 144,000 | 约 -0.087 dB | 约 -90.43 dB | 3 秒轨迹，最大方位误差 0.3475° |
| C＋LFE 床＋1 对象，自动分配 | 48,000 | 约 +0.0005 dB | 约 -88.54 dB | 左右位置对照均正确 |
| C＋LFE 床＋移动对象 | 144,000 | 基本不变 | 明显抑制 | 3 秒轨迹，最大方位误差 0.3475° |
| C＋普通 Right 床＋对象 | 48,000 | 约 -0.0017 dB | 约 -0.0014 dB | 对象位置正确，无 LFE 分支 |
| 22 对象＋C/LFE 床 | 48,000 | 基本不变 | 明显抑制 | 全部 1034 条有效位置更新通过 |
| 23 对象＋独立 LFE | 48,000 | 约 -0.087 dB | 约 -90.43 dB | 全部 1081 条有效位置更新通过 |

23 对象用例最低预期对象频率能量占比约 99.99985%，最大方位误差约 0.346467°。
这些结果验证了独立 LFE 身份、低频音频、对象独立音轨和位置保持；不是把低音轨当普通定位对象处理。
工具没有对 LFE 人为加 10 dB。实际播放系统的 LFE 校准、低音管理和耳机折叠属于渲染端，本轮未验证。

## 4. 容器和产品边界

代表性样本封装为 CAF 和 MP4；重新读取后，压缩包、magic cookie、有效帧数及前导／尾部填充均与原始编码产物一致。
CAF 未另写声道坐标；LFE 标签和对象信息来自压缩配置及码流。
普通播放器是否正确采用混合元数据，以及 AVAssetReader 是否保持原始对象音轨，仍不能由封装成功推出。
此前已经观察到多对象组件的对象标签会重复，AVAssetReader 可重混这些音轨；本轮验收使用原生 AudioCodec。

对于 17.1.6，**23 个定位对象＋1 个独立 LFE** 的通道数构成已打通；
本轮对象角度使用合成测试分布，尚未接入用户确认的 17.1.6 具体标签、角度和顺序表。
也没有把混合管线接入 Core 的公开 C ABI、CLI 或 GUI。

## 5. 复现

源码为 `make_mixed_inputs.py`、`run_mixed.py`、`verify_mixed.py`，使用同目录的原生探测器和 LLDB 只读观察。
先把当前 `codec_probe.cpp` 按原有 `-O2 -g` 命令编译到参考目录的 `bin/codec_probe`，以包含原生声道标签读取。
然后在新目录运行：

```sh
python3 scripts/research/apac_objects/run_mixed.py \
  --output local/apac-object-experiments/new-lfe \
  --reference local/apac-object-experiments/lfe
python3 scripts/research/apac_objects/verify_mixed.py \
  --output local/apac-object-experiments/new-lfe
```

本轮结果保存于 `local/apac-object-experiments/lfe/`：`mixed.json` 为用例清单，
`verification.json` 为语义验收，`containers.json` 为容器一致性，`cases/` 保存完整命令和事件。
元数据、音频探测都是自建进程；编码能力表、解码能力表、解码缓冲区及系统组件文件均未修改。
