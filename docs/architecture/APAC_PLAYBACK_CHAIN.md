# APAC 原生回放链路与人声对象缺失

2026-09-25 整理：原始抓包、WAV、MP4、PCM 和调试数据库已按用户要求从仓库工作区删除。
本页保留当时观察结论；挑选的结构化结果见[数值证据](evidence/apac_objects/README.md)。
以下 `local/` 路径是历史运行位置，不表示文件仍存在。

2026-09-23，macOS 27.0 / 26A428，QuickTime Player 10.5 (1268.17)。
结果根目录：`local/apac-object-experiments/playback-chain/`。

## 当前结论

**2026-09-24 交付确认：用户确认无扩散对象版本播放正常。**
当前全长、48 秒片段及已有 M4A 副本统一列在 [对象交接文档](APAC_OBJECT_HANDOFF.md)。
以下 diffuse 故障描述针对旧配置；当前采用版本已按用户要求移除 diffuse。

**未经修改的 QuickTime 能渲染位置对象，但现有 Tsuioku `pos-diffuse` 文件不能判为完整回放通过。**
AirPods Max USB 输出实测确认了两个不同结果：

- 一个纯位置对象的左、右稳态分别有 **+8.2157 / −8.0676 dB** 的耳间能量差；用户也确认“先左后右”。
- 7.1.2 BED＋32 个非扩散测试对象，以原始 `7+7+7+7+4` 分组播放，**32 条独立对象频率均进入 USB 输出**。
- 真实歌曲的 8 条 `VOCAL/BV` 对象在当前 `diffuse=1` 映射下，只在开始约 **85 ms** 有输出，
  随后的稳态 USB PCM **逐采样精确为零**。相同音频仅将这 8 条的 diffuse 改为 0，恢复约 **3.009 s** 连续输出。
- 原生逐轨解码的人声仍完整，故缺失发生在元数据参与的空间回放阶段；提高码率或改为 32 个单对象组件不能解决它。

关闭 diffuse 是改变语义的诊断退路，不能称作完整 ADM 转码修复。原来省略 22 个对象尺寸的限制也仍存在。
本轮没有修改 QuickTime、系统解码器、渲染器、默认设备、音量或空间音频设置，没有新增产品 API／GUI。

用户随后明确要求“抛弃扩散，做一版本”，已完成全长 `pos-only_12000kbps` 版本，
目录为 `local/apac-object-experiments/tsuioku-12000kbps-no-diffuse/`。
该版本保留音频和位置，删除全部对象的 diffuse，尺寸继续沿用前版的省略方式。
全长逐轨解码、位置／diffuse=0、CAF／MP4 一致性均通过，实际压缩包平均 11,741.898 kbps。
已验证原生 AVPlayer 的 57–63 秒片段输出，并在 QuickTime 正常播放期间取得 AirPods Max USB 输出；
验证记录见新目录的 `playback-verification.json`；用户确认单独记入 `accepted-object-release.json`。
这是用户选定的语义转换，不改变以上对旧含扩散配置的结论。

## 更正此前离线转换的解释

`AVAssetReaderTrackOutput` 请求两声道时，原始分组只混入索引 `0,7,14,21,28`，
单对象分组则将全部对象按 `0.5 / 0.5` 居中混入。这个结果可复现，但属于**离线声道转换**。
曾将它外推为 QuickTime 的丢对象和居中行为，这一解释不成立。

实际实时回放使用元数据解码和空间混音。原始分组的 32 条位置测试音已经在 QuickTime 的 USB 输出中全部检出，
与自建 AVPlayer 中观测到的耳间频率增益一致。单对象分组因此仅保留为离线转换兼容性对照，
不再推荐为 QuickTime 人声修正版。

旧证据保留在 `quicktime-object-playback/`；新结论取代旧报告中的回放外推，不否定原来的逐轨解码指标。

## 实际调用链

在自建、优化编译的 AVPlayer 进程中，通过只读断点观察到：

```text
AVPlayerItem / MediaToolbox
  → AudioQueueObject::CheckIfContentRequiresAUSpatialMixer
  → containsSpatialContent 查询 imrd；containsMetadata 查询 mdpf
  → MEMixerChannel 配置 AUSpatialMixerV2
  → AudioCodecProduceOutputBufferList
  → ACAPACBaseDecoder::GetOutputBufferListWithMetadata
  → MetadataBase::Deserialize / APACMetadataSink
  → AudioMetadataFrame
  → ScheduledSlicePlayer2::ScheduleMetadata
  → AudioMetadataTimeline_AP::addEvent
  → APAC::MetadataBitStreamParser
  → AUSpatialMixer / HRTF
  → 系统输出链路
  → AirPods Max USB endpoint 0x05 OUT
```

最终查询到 `mdpf=1`、`mdcf=1`、`mdfs=4096`，队列的 spatial-content、metadata 标志均为 1。
`imrd` 描述通过 AU 属性 3231 进入空间混音器；观测到输出类型为耳机。
60° 的输入位置在渲染器侧读到 59.765625°，移动样本的解码位置也随时间更新。
普通 ABL 的 buffer 数不能判断有无元数据：该路径使用带附加元数据的 ABL。
cookie 初始化前第一次 `mdpf` 查询可短暂返回 `!stt`，随后成功，不能把早期失败当作原因。

QuickTime 本身的只读进程采样也出现 `AQMetadataAudioConverter`、
`GetOutputBufferListWithMetadata`、APAC 解码及 AudioDSP 卷积调用。
静态检查确认其使用 AVPlayer 和 AVMutableComposition；在自建探针中复现 composition、
unity audio mix 和二者组合均没有破坏位置渲染。
没有调整 QuickTime 的“首选直通”或其他偏好。AU 属性 21 的一条可疑写入经核对属于 TimePitchBypass，
不能当成空间混音器被旁路的证据。

## 观察方法与边界

### 自建实时探针

`player_chain_probe.m` 使用系统 AVPlayer，静音仅作用于这个自建进程。
`observe_spatial_output.cpp` 在调用原始 AU 函数后复制其 PCM；不改参数、返回值、格式、增益或音频，
不把该观察库注入 QuickTime。文件保存发生在退出时，音频线程只写入有界内存。
这比持续暂停 LLDB 更适合定量分析；早期 LLDB PCM 片段有时序扰动，只保留为调用链诊断。

这个观察点在系统最终输出之前，不能单独替代 QuickTime 端到端验收。
但相同人声故障在这里已出现，随后又在真正 QuickTime USB 输出中复现。

### AirPods Max USB 最终输出

方法参考用户提供的 `Downloads/airpods-max-usbc-audio-capture/README.md`，
实际设备重新枚举后，本次接口为 **XHC2**，位置 ID `0x02100000`，音频端点为 `0x05 OUT`。
没有沿用参考材料旧机器运行中的 XHC0 假设。用户启用了 USB 抓包接口，并提供了“USB 音频”连接截图；
System Profiler 仍报告 Bluetooth，不能用这个字段否定实际 USB 传输。

仅提取指定设备、端点、成功完成的等时传输。每个 payload 长度与 `io_len` 一致，
I/O completion ID 无重复。正式采集均未丢包。数据为 48 kHz、双声道、有符号小端 32-bit 容器，
最低字节全零，导出 24-bit WAV 时没有归一化或另做空间渲染。
USB 回放没有麦克风采集，也没有上传媒体。

公开 Core Audio 单进程 tap 另有研究工具 `capture_process.mm`。早期零回调不能当作音频静音；
后续使用单进程 stereo mixdown 能采到数据。本报告最终结论以 USB 端点采集为准。

## 位置与数量的正例

| 验证 | 结果 | 证据 |
| --- | --- | --- |
| 纯对象 12 秒噪声，左右各 3.5 秒，100 ms 淡入淡出 | 稳态 +8.2157 / −8.0676 dB；主观先左后右 | `usb-airpods/qt-pure-object/` |
| 原分组的 BED＋32 个独立测试音 | 32/32 检出；频率拟合残差能量比例约 3.02e−6 | `usb-airpods/qt-groups7/analysis.json` |
| 移动对象，自建实时 AVPlayer | 左侧约 +7.07 dB，中间 +0.046 dB，右侧 −6.66 dB | `continuous-moving/trajectory-verification.json` |
| 原分组与单对象分组，自建 AVPlayer | 非扩散对象的频率增益基本相同 | `continuous-spectral-verification.json` |

32 路 USB 样本使用 13.7–15.7 秒的稳态采集窗口；这次抓包末尾没有覆盖整个音频尾部，
因此该记录用于逐频率存在性和定位，不用它重新声称逐轨全长帧数通过。
全长帧数、音频身份由此前默认原生解码的独立验证覆盖。
本轮没有独立验收高度听感、头部跟踪或完整 ADM 空间一致性。

## 人声缺失的受控复现

从源文件只读扫描人声对象活动，选择原曲 57–60 秒。对象为源声道索引 10–17：
`VOCAL ESS OBJ L/R`、`BV OBJ L/R`、`BV OBJTOP L/R`、`BV FRONT OBJ L/R`。
它们都带源 ADM `diffuse=1`；原音频、位置、距离和组件预算保持相同。

先制作完整混合／仅 BED／仅人声三段的一个文件，再制作人声单独文件排除片段切换影响。
完整场景和 42 路映射始终保留，只将非目标音频置零。另一个诊断文件只改变这 8 条的 diffuse。

| 检查 | 保留 diffuse=1 | 诊断 diffuse=0 |
| --- | --- | --- |
| 原生逐轨音频 | 人声完整 | 相同源人声 |
| QuickTime USB 活跃区间，阈值 1e−6 | 0.085354 秒 | 3.009333 秒 |
| 初始输出后 0.3–2.8 秒 USB PCM | 每个采样都为零 | 持续非零 |
| 自建 AVPlayer 空间 AU 输出 | 初始瞬态后归零 | 持续非零 |
| 32 单对象组件的原 diffuse 场景 | 稳态仍为零 | — |

三段对照中的 8 路原生解码与输入同轨相关系数最低 **0.99999837**，
RMS 增益偏差最大约 **0.00195 dB**，最低 SNR 约 **54.81 dB**。
所以“对象音频没有编码进去”不能解释该现象。隔离人声 USB 输出归零也不能用 BED 掩蔽解释。

历史短样本说明（媒体已清理）：

- `usb-airpods/vocal-controls/original/vocals.mp4`：原位置／diffuse，复现人声瞬态后丢失。
- `usb-airpods/vocal-controls/diagnostic-diffuse0/vocals.mp4`：仅关闭人声 diffuse，声音恢复，**语义已改变**。
- `usb-airpods/vocal-controls/source-vocal-pairs-unrendered.wav`：原始四对人声的左右求和，
  没有 APAC 或空间渲染，只用于确认源内容，不是空间参考母版。
- `usb-airpods/qt-vocal-original/audio.wav`、`qt-vocal-diffuse0/audio.wav`：未经归一化的实际 USB 输出。

### 扩散增益与配置边界

对相同人声及 42 路场景改变 diffuse，自建原生播放器的稳态输出满足：

| diffuse 输入 | 相对 diffuse=0 的幅度 | 相对能量 |
| ---: | ---: | ---: |
| 0 | 1 | 1 |
| 0.25 | 0.866025 | 0.75 |
| 0.5 | 0.707107 | 0.5 |
| 0.75 | 0.5 | 0.25 |
| 0.99 | 0.108253 | 0.01171875 |
| 1 | 0 | 0 |

这是直达分量按 `sqrt(1−diffuse)` 衰减、而扩散分量没有进入最终输出的强证据。
0.99 的结果带量化影响。尚未定位控制扩散支路启用的具体内部条件，
也不能仅凭这些实验把问题归因为 macOS 实现错误；私有编码配置的配套要求仍可能相关。

数量与参数组合会改变结果：当前控制里一个全扩散对象、2/7/8 个全扩散对象均可出声，
9 个及若干更大的全扩散场景归零。但合成的 8 对象场景中，仅第一条设 diffuse=1、其他对象为普通点对象，
该扩散测试音也归零；同一音频改 diffuse=0 后恢复。
32 对象场景只保留一个 diffuse 对象、其余为点对象，仍可复现。
因此 **8 不是已证实的扩散对象上限，9 也不是总对象数量限制**。
这些反例阻止把单一成功／失败数量外推成产品能力表。

源音频独立的最小反例在 `usb-airpods/diffuse-synthetic-boundary/`：
中心＋LFE BED 保持静音，只有第一个对象播放 1 kHz 单音，其他对象也保持静音；
`bed2-objects9-first1` 稳态为零，`bed2-objects9-first1-point` 恢复。
系统编解码与播放器都保持默认，仅生成的元数据不同。
两者默认原生解码均恢复 49152 个有效帧；首对象与合成音频相关系数均超过 0.999999989，
RMS 均为约 0.021213，其他十路精确静音，排除了编码器遗漏测试音。

## 复现工具

源码均在 `scripts/research/apac_objects/`，不加入默认构建或 CTest。
原生探针使用 `-O2 -g`；普通用例 30 秒，LLDB 60 秒，并记录最后阶段。

```sh
xcrun clang -O2 -g -fobjc-arc -Wno-deprecated-declarations \
  scripts/research/apac_objects/player_chain_probe.m \
  -framework Foundation -framework AVFoundation -framework CoreMedia \
  -framework AudioToolbox -framework CoreAudio -o PLAYER
xcrun clang++ -std=c++20 -O2 -g -dynamiclib -Wall -Wextra -Werror \
  -Wno-deprecated-declarations scripts/research/apac_objects/observe_spatial_output.cpp \
  -framework AudioToolbox -o OBSERVER.dylib
```

`PLAYER`、`OBSERVER.dylib` 应放到本地研究目录。观察时只给自建 PLAYER 设置
`DYLD_INSERT_LIBRARIES` 和 `APAC_SPATIAL_OBSERVER`；不能把观察环境传给 afconvert 或系统播放器。
本轮矩阵首轮曾因环境泄漏使 arm64e afconvert 拒绝加载 arm64 观察库，运行器已修正作用域；
失败记录保留，完整结果以 `diffuse-matrix-v2` 为准。

```sh
python3 scripts/research/apac_objects/make_playback_audition.py NEW_AUDITION \
  --codec local/apac-object-experiments/lfe/bin/codec_probe
python3 scripts/research/apac_objects/make_vocal_controls.py VOCAL_INPUT NEW_CONTROLS \
  --codec local/apac-object-experiments/lfe/bin/codec_probe
python3 scripts/research/apac_objects/run_diffuse_playback.py VOCAL_INPUT NEW_MATRIX \
  --codec local/apac-object-experiments/lfe/bin/codec_probe \
  --player PLAYER --observer OBSERVER.dylib --single-settings SINGLE_SETTINGS
```

`VOCAL_INPUT` 由 `make_adm_input.generate` 以 start=2736000、frames=144000、
`omit_fully_diffuse_extent=True`、total_bitrate=12000000 生成。
`SINGLE_SETTINGS` 对应中心＋LFE BED、一个对象和一个元数据通道。
附加 `--layout-controls`、`--counts 9 12 15 16 17`、`--diffuse-limit-controls` 或
`--synthetic-boundary` 可复现保存的不同控制；每次使用不存在的新目录。

USB 采集须先确认当前设备／接口，不能机械复制本次 XHC2。用户启用抓包接口后：

```sh
dumpcap -q -i XHC2 -B 32 -a duration:22 -a filesize:18000 -w CAPTURE.pcapng
python3 scripts/research/apac_objects/extract_usb_audio.py CAPTURE.pcapng NEW_EXTRACT \
  --location 0x02100000
python3 scripts/research/apac_objects/analyze_usb_playback.py NEW_EXTRACT
```

在采集窗口正常播放指定 QuickTime 样本。`extract_usb_audio.py` 拒绝 payload 长度不符、
重复 completion 和无目标音频包的文件。`analyze_usb_playback.py --tones EXPECTED --window START END`
在明确的稳态窗口逐频率验证。`summarize_playback_evidence.py` 汇总本次固定采集窗口，
窗口不可直接套用到另一轮抓包。

## Core 接入建议与剩余工作

解码器的默认能力继续维持。对象导出的验收需要区分压缩数据、元数据读回、实时空间 AU 和最终设备输出；
原生逐轨音频完整、初始化成功或离线立体声能出声，都不能替代最后两层。

当前可以确认点对象位置路径；不能把 ADM diffuse 字段数值读回正确当成扩散声音已经正确播放。
应先查明 APAC 场景描述与系统扩散支路的配套配置，再处理此前未打通的尺寸映射。
当前已采用位置优先的对象转换，并明确标注“移除 diffuse／尺寸”；完整语义模式仍未打通。
本轮未把这种退路接入产品。

AudioCodecs arm64e SHA-256：
`82fb858cdbcf9146b1740a5cccd25dbf843ab9fbcdb88e05bbbbaed3e8e9cdd2`；
QuickTime arm64e SHA-256：
`74609b244d6685a6679b758637d781694005ae3160184afbebbe3d604f6ac9d5`。
原始用户 WAV 未改写。源码、报告及少量数值 JSON 留在仓库；
短样本、原始 pcapng、WAV、压缩包、分析数据库和其他本地生成物均已清理。
依赖 PCM 或抓包的脚本需要用用户保留的源文件重新采集。
