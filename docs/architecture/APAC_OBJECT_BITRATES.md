# APAC 对象与 BED 混合编码：码率配置与实测边界

2026-09-25 本地产物已清理。本文的 `local/` 路径为历史运行位置；
精选结构化结果见[数值证据](evidence/apac_objects/README.md)。

验证日期：2026-09-23。macOS 27.0 / 26A428，AudioCodecs.component 7.0，arm64e。
组件切片 SHA-256：`82fb858cdbcf9146b1740a5cccd25dbf843ab9fbcdb88e05bbbbaed3e8e9cdd2`。
全部样本为 48 kHz；编码能力表与解码器均保持默认。测试沿用 `csrc=7`、`cdrc=0`、`aspf=75`。

**当前私有 ASC 制作路径应显式设置各输出组件的 `Bit Rate`，再将全局 `brat` 设为预算之和。
单独设置全局值，不保证各对象／BED 实际使用该预算。**
所有码率数值的单位均为 **bit/s**，不是 byte/s；组件值是整个组件的预算，不是组件内每路的预算。

## 1. 配置方法

全局属性为 `kAudioCodecPropertyCurrentTargetBitRate`，FourCC 为 `brat`，数据类型 `UInt32`。
先设置 PCM／APAC 格式与私有 `acs ` 字典，再设置全局码率、模式等属性，最后初始化并实际送入音频。

私有字典中，在 `Codec Configurations` 的每个 `ASComponents` 输出组件中加入：

```json
{"key": "Bit Rate", "current value": 1792000}
```

该字段放在对应的 `Object` 或 `Channel Bed` 数组内；不放进输入侧 `Audio Scene Components`。
其余对象数量、标签、通道映射和元数据设置延续已验证协议。

已经验证的 **7 对象＋独立 LFE** 配置：

| 项目 | 配置值 |
| --- | ---: |
| 一个 `Object Count=7` 组件 | `7 × 256000 = 1792000` bit/s |
| 一个单声道 `LFEScreen` BED | `16000` bit/s |
| 全局 `brat` | `1808000` bit/s，即 1.808 Mbps |
| `kAudioCodecPropertyBitRateControlMode` / `acbf` | `1`，长期平均码率模式 |

对象再分成多个组件时，逐组件求预算。例如 23 个对象分为 `7+7+7+2`，
按每对象 256 kbps、LFE 16 kbps，四个对象组件分别填 `1792000、1792000、1792000、512000`，
LFE 组件填 `16000`，全局合计 `5904000` bit/s。

普通声道床也是整个组件填一个预算。本轮另验证一个 12 声道 BED（11 个全频通道＋1 LFE）加一个对象：
按全频通道／对象 128 kbps、LFE 16 kbps，BED 填 `1424000`、对象填 `128000`、全局填 `1552000`。
13 路音频的帧数、非静音与独立频率对应均通过。
这是已测分配方法，不表示编码器保证逐声道平均分配，也不是对最佳听感码率的推荐。

## 2. 全局值、组件值和自动分配

只读断点观察 `APACCoreEncoder::Initialize(apac::ConfigParam&)` 在选择配置前后的组件预算：

| 配置 | 组件选择前 | 组件选择后 | 结果 |
| --- | ---: | ---: | --- |
| 单对象，组件未填，全局 256 kbps | 0 | **96000** | 编解码通过 |
| 单对象，组件显式 256 kbps | 256000 | **256000** | 编解码通过 |
| 独立 LFE，组件为 0；另一个对象组件显式 256 kbps | 0 | **0** | 首次出包 `-50` |
| 独立 LFE，显式 16 kbps | 16000 | **16000** | 编解码通过 |

这补全了此前独立 LFE 自动分配失败的证据：该测试配置实际选择了零预算。
`Bit Rate=0` 不能理解为禁用组件或无损模式；普通对象用它会触发自动分配，独立 LFE 在本轮配置下会失败。
不要依赖所有组件的自动行为相同。

24 秒确定性宽带噪声对照也确认，未写组件码率时，仅将全局 `brat` 设为 32000、128000、256000，
实际中段压缩包码率都约为 **106361 bit/s**，解码信号误差测量也相同。
全局 getter 会原样回读这些请求值，不能把 getter 值当作实际组件预算。

反过来，固定 LFE 16 kbps、对象 256 kbps 时，全局设为 1 bit/s 仍能编码并默认解码，
实际中段压缩包约 140.6 kbps。因此本路径也没有用全局值严格约束组件预算之和。
工程上仍建议将全局设为各组件之和，保持配置含义清楚。

此前纯对象报告中的 `256000*N` 仅是全局属性写入值；那些未显式设置组件码率的实验，
不能据此推算实际组件码率、压缩比或听感质量。既有音频、位置和数量验收结果不受这个区分影响。

## 3. 模式与实际压缩数据码率

`kAudioCodecPropertyBitRateControlMode` / `acbf` 的默认值为 1；本轮 0～3 均通过完整音频编码与默认解码：

| 值 | SDK 定义 | 本轮说明 |
| ---: | --- | --- |
| 0 | Constant，CBR | 包大小仍可变化；不能将其理解为容器固定字节速率 |
| 1 | LongTermAverage，ABR | 当前实验默认，按长期平均控制 |
| 2 | VariableConstrained，受约束 VBR | 码率随内容变化 |
| 3 | Variable，VBR | 使用 `SoundQualityForVBR` / `vbrq` 控制质量 |
| 4 | 非法模式 | 属性设置返回 `!dat`，未开始编码 |

`vbrq` 按 SDK 定义使用 0～127。本轮 0、32、64、96、127 均成功并改变输出。
128 也被该组件接受，但样本结果与 127 相同；这不是可依赖的额外档位，配置仍应限制在文档范围内。
未设置时 getter 返回 `0xffffffff`，它表示未显式指定的状态，不能当作最大质量值。

对于同一份 24 秒单声道白噪声，保持全局 128 kbps，测得以下实际中段压缩包速率：

| 组件预算 | 模式 | 实测压缩包码率 |
| ---: | --- | ---: |
| 64 kbps | ABR | 74.35 kbps |
| 128 kbps | ABR | 139.31 kbps |
| 256 kbps | ABR | 275.64 kbps |
| 128 kbps | CBR | 134.47 kbps |
| 128 kbps | 受约束 VBR | 160.61 kbps |
| 128 kbps | VBR，默认质量 | 90.83 kbps |

中段统计去掉首尾各 4 个编码包，以其余包字节数除以对应的 1024 点帧时长。
结果还保存按有效输入时长及全部编码帧时长计算的两个平均值，避免短样本前导填充影响比较。
压缩包包含对象元数据和帧头，模式本身也有波动；不能保证实测速率等于预算之和。
容器还会增加自己的头部与索引开销。

内容影响非常明显：一个 256 kbps 预算的纯音对象，中段只产生约 147.38 kbps；
7 个纯音对象＋LFE 的名义预算是 1.808 Mbps，1 秒样本全部压缩包按有效时长计算约 **0.899 Mbps**。
这些信号用于检验码率控制，不用于音乐听感评分，也没有证明高码率等于无损。

## 4. 限制与失败边界

不能把属性列表或整数类型的最大值当作当前私有路径的可靠硬上限。
本轮预初始化查询 `AvailableBitRateRange` 返回 6 kbps～1.28 Mbps 的离散档位；
`ApplicableBitRateRange` 返回 64～320 kbps、`RecommendedBitRateRange` 返回 96／112／128 kbps。
这些结果在本轮不同对象／BED 通道配置下没有随私有组件配置正确变化，初始化后后两项又返回错误。
实测 1.808 Mbps 混合配置可以通过，272 kbps 等不在列表中的总目标也被接受，故该列表不能充当混合导出白名单。

本轮检查的是离散测试点，并未证明各点之间所有数值、所有内容和所有采样率都可用：

| 设置 | 实测结果 |
| --- | --- |
| 单对象显式 6、16、约 32、48、64、128、160、192、256、320、512、1000 kbps | 编码、完整帧数、非静音、预期频率均通过 |
| 单对象显式 1、100、500、1000、2000、3000 bit/s | 设置和初始化被接受，首次出包 `-50` |
| 单对象显式 2000000 或 4294967295 bit/s | 设置和初始化被接受，首次出包卡住，由独立进程的 **30 秒超时**终止 |
| 独立 LFE 显式 2、3、4、约 8、12、16、24、约 32、64、128、256 kbps | 与 256 kbps 对象混合，完整音频解码通过 |
| 独立 LFE 显式 0、1、100、500、1000 bit/s | 首次出包 `-50` |
| 全局 `brat` 从 0 到 `0xffffffff` 的已测点 | 均可受理；组件自动选择和实际结果说明这不是对应总速率的支持证明 |

因此 **16 kbps 不是 LFE 的唯一值或最低可编码值，256 kbps 也不是对象的硬上限**。
本轮尚未确定对全部内容都成立的连续有效区间或最高安全值。
特别是 1 Mbps 的简单单音成功，并不使它成为建议的产品上限；2 Mbps 的卡住反例说明需要应用侧检查和超时隔离。
生产配置目前可沿用已验证的明确分配，再针对实际素材和质量目标校验。

## 5. 工具、验收与复现

独立工具仍以 `-O2 -g` 构建，不修改生产 API、GUI、默认构建或常规 CTest。
`bitrate_probe.cpp` 复用 `codec_probe.cpp` 的音频／容器流程，仅在编码初始化前后查询属性，
可通过显式参数设置编码模式或 VBR 质量；解码流程没有增加任何私有设置。
`trace_bitrate.py` 经组件哈希校验后只读观察实际组件配置选择。

主矩阵 **86 组**，其中 72 组通过完整音频帧数、能量、有限值和合成单音频率检查，
14 组为核对阶段的预期失败，其中包括两组 30 秒超时。
6 kbps 单对象的 47 条位置记录、7 对象＋LFE 的 329 条位置记录均从默认解码元数据读回并通过量化误差检查。
后者 CAF／MP4 的压缩包、cookie 与 timing 提取结果均与原始编码一致。
使用原探测器、不增加码率属性查询的对照，24 秒噪声及显式 256 kbps 对象的压缩包也完全相同；
部分短纯音对照有很小的码流与 PCM 差异，本轮不宣称不同执行之间均逐字节确定。

结果目录：`local/apac-object-experiments/bitrates/`。
新目录复现（首条命令会构建小探测器、记录环境和验证组件哈希）：

```sh
python3 scripts/research/apac_objects/run_bitrates.py --output local/apac-object-experiments/new-bitrates --matrix global
python3 scripts/research/apac_objects/run_bitrates.py --output local/apac-object-experiments/new-bitrates --matrix object
python3 scripts/research/apac_objects/run_bitrates.py --output local/apac-object-experiments/new-bitrates --matrix lfe
python3 scripts/research/apac_objects/run_bitrates.py --output local/apac-object-experiments/new-bitrates --matrix mode
python3 scripts/research/apac_objects/finish_bitrates.py --output local/apac-object-experiments/new-bitrates
```

每个普通编解码进程限时 30 秒，调试观察限时 60 秒。已有结果使用最后一条命令加 `--verify-only` 重新核验。
`bitrates.json` 保存请求值、属性回读、压缩包速率及音频测量；`allocation.json` 保存实际组件预算选择；
`verification.json` 明确区分成功与预期失败。
`archive_bitrates.py --results 结果目录 --output 新ZIP路径` 可归档源码、结果与压缩样本，
生成并复核 SHA-256 清单；不归档原始 PCM 缓存、系统二进制或分析数据库。
