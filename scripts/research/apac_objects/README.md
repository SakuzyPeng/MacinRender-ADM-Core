# APAC 对象研究工具

**当前保留源码与研究记录。** 本地生成的音频和大中间文件已于 2026-09-25 清理；
无扩散的 7.1.2 BED＋32 对象版本曾于 2026-09-24 获用户确认。
另已对《Man In the Mirror》ADM BWF 制作 7.1.2 BED＋26 对象的无扩散试编码，
历史结果目录为 `local/apac-object-experiments/man-in-the-mirror-12000kbps-no-diffuse/`；
这份新素材尚无用户试听确认。

## 固定 7.1.2 BED 的单命令入口

`encode_fixed_712.py` 接受源 ADM BWF 和一个**尚不存在**的结果目录，输出 APAC CAF 与 MP4。
BED 严格固定为 Core Audio `Atmos_7_1_2` 的十路标签及顺序；其余每轨为一个静态单声道对象，
自动按每组件至多 7 个对象分组。PCM24／48 kHz、ADM 字段和元数据容量由原有严格解析器检查；
不符合条件会停止，不自动猜测其他布局或动态对象的映射。

```sh
python3 scripts/research/apac_objects/encode_fixed_712.py \
  '/Users/Sakuzy/Downloads/ADMCUT - RADWIMPS - 前前前世_SSDX.wav/ADMCUT - RADWIMPS - 前前前世_SSDX.wav' \
  local/apac-object-experiments/zenzenzense-new
```

默认目标为整个场景 **12,000 kbps**，将对象 diffuse 置零并记录源值；源尺寸只在完全扩散的
已验证退路中省略。保留原 WAV、BED、LFE、各对象音频和位置。系统能力表与解码器不调整。
原生探针没有指定时，脚本只在本地 `local/apac-object-experiments/bin/` 构建／复用一个 `-O2 -g`
小工具，不创建独立工作区或 CMake 缓存。CAF→MP4 使用压缩包复制；默认删除大型暂存 PCM 和重复包。

日常命令只编码和封装，不进行解码或播放实验。需要之前那套全长逐轨、元数据和容器验收时，
显式追加 `--verify`；改变总目标码率用 `--bitrate-kbps N`，
需要保留暂存文件用 `--keep-intermediates`。
每次输出都含 `summary.json`、`entrypoint.json`、命令结果、哈希清单及源字段 sidecar；
目标码率与实际文件码率分别记录。
文件名、方法和数值证据见
[`APAC_OBJECT_HANDOFF.md`](../../../docs/architecture/APAC_OBJECT_HANDOFF.md)。
音频、现有 M4A 副本和本地整理目录不再保留。以下历史结果路径须先运行相应工具重新生成；
默认编码入口不会额外执行播放测试。

结论和协议边界见 [`APAC_OBJECT_EXPERIMENTS.md`](../../../docs/architecture/APAC_OBJECT_EXPERIMENTS.md)。
这些工具绑定 macOS 27.0 / 26A428 的 AudioCodecs arm64e 切片，用于研究，不属于公开导出接口。
后续多组件及默认解码边界见 [`APAC_OBJECT_BOUNDARIES.md`](../../../docs/architecture/APAC_OBJECT_BOUNDARIES.md)。
默认能力表的每组件 7 对象不等于整文件最多 7；当前原生默认解码验证到 70，71 触及元数据输出容量限制。
对象与 LFE 的混合验证见 [`APAC_OBJECT_LFE_MIXED.md`](../../../docs/architecture/APAC_OBJECT_LFE_MIXED.md)。
BED 与对象的数量组合见 [`APAC_OBJECT_MIXED_CAPACITY.md`](../../../docs/architecture/APAC_OBJECT_MIXED_CAPACITY.md)：
默认编码 `24 BED＋69 对象`、`127 BED＋1 对象` 均通过；诊断编码可进一步提高 BED 总量，解码端始终保持默认。
码率配置和边界见 [`APAC_OBJECT_BITRATES.md`](../../../docs/architecture/APAC_OBJECT_BITRATES.md)：
混合路径应显式设置输出组件预算；全局 `brat` 回读值不等于组件实际选择值或文件平均码率。
实时回放与 QuickTime USB 抓包见 [`APAC_PLAYBACK_CHAIN.md`](../../../docs/architecture/APAC_PLAYBACK_CHAIN.md)。
位置对象能够正常渲染，但真实歌曲当前 diffuse 映射会触发人声稳态静音；单对象分组不能修复这一问题。

## 完整复现

需要 Xcode 命令行工具、Python 3、NumPy，以及可调试自己编译程序的 LLDB 环境。
沿用现有 checkout，不需要额外工作区或 CMake 构建缓存：

```sh
python3 scripts/research/apac_objects/run_experiments.py \
  --output local/apac-object-experiments/new-run
```

输出目录必须不存在。完整流程会编译三个小工具，完成单对象静态／移动、12 组数量／能力表对照、
受控反例、CAF／MP4 封装、原生压缩数据校验及 AVAssetReader 读取，最后写 `verification.json`。
数分钟运行时间主要来自逐对象元数据断点观察。所有声音仅作离线分析，不向系统音频设备播放。

验证已有结果：

```sh
python3 scripts/research/apac_objects/verify_results.py \
  --output local/apac-object-experiments/repro
```

## 组成

| 工具 | 用途 |
| --- | --- |
| `encode_fixed_712.py` | 固定 7.1.2 BED 的单命令对象 APAC 编码；默认无扩散、12 Mbps 目标、清理大暂存 |
| `codec_probe.cpp` | 原生 AudioCodec 编码、原生解码、AudioFile 无损提取压缩包；显式保存 packet timing |
| `make_inputs.py` | 合成不同频率音轨及 AIA 1.4 元数据；创建私有对象设置 plist |
| `make_mixed_inputs.py` | 带 LFE 标签的声道床、对象、独立元数据及显式组件码率 |
| `run_mixed.py` / `verify_mixed.py` | 检查混合音频、原生 LFE 标签与编解码分支、低通行为和对象位置 |
| `run_mixed_capacity.py` / `finish_mixed_capacity.py` | 混合组件、音频总数、69／70 对象及默认解码边界；逐路验证、容器和反例 |
| `bitrate_probe.cpp` / `trace_bitrate.py` | 全局码率／模式查询和设置；只读观察组件实际预算选择 |
| `run_bitrates.py` / `finish_bitrates.py` | 码率、模式、LFE、24 秒噪声、超时反例及默认解码验收 |
| `archive_bitrates.py` | 独立打包码率研究材料，复核 ZIP CRC 与 SHA-256 清单 |
| `trace_codec.py` | 观察对象路径、实际 Profile、解码位置；经哈希校验后可替换自建编码实例的能力表 |
| `extract_positions.py` | 仅从解码观测导出位置 CSV／JSON，并按原生前导、尾部填充截取有效 PCM |
| `render_decoded.cpp` | 仅使用解码音频和位置的离线 AUSpatialMixer 双耳渲染 |
| `wrap_caf.py` | 将原始 APAC 包、cookie、timing 封装成 CAF，不加入外层声道坐标 |
| `read_asset.m` | 独立检查 AVAssetReader 的声道数、帧数和能量 |
| `run_case.py` | 独立进程组、30／60 秒超时、记录实际 inferior 退出状态及 JSON 事件 |
| `run_matrix.py` | 1／7／8／24 对象，默认及两个诊断上限，共 12 个组合 |
| `run_boundaries.py` | 32／33、64／65、127／128、多组件及 70／71 默认解码边界 |
| `finish_boundaries.py` | 追加只读元数据容量、容器帧数及 AVAssetReader 音频语义检查 |
| `verify_boundaries.py` | 区分编码完成、默认原生解码完整通过、失败及未验证项 |
| `verify_results.py` | 对照独立输入预期验证音频、位置、时间、反例和渲染方向 |
| `archive_results.py` | 打包源码、短样本和结构化证据，生成并逐项核验 SHA-256 清单 |
| `make_adm_input.py` / `run_adm_trial.py` | 严格限定静态 7.1.2 BED＋单轨对象 ADM 的真实文件试编码；尺寸退路须显式指定 |
| `trace_adm.py` / `verify_adm.py` | 默认解码后只读采集位置／尺寸／扩散；全长逐轨音频、身份和元数据验证 |
| `run_playback_controls.py` | 7.1.2 BED＋32 独立对象测试音，核对公开 AVFoundation 双声道输出是否漏对象 |
| `player_chain_probe.m` / `observe_spatial_output.cpp` | 自建系统 AVPlayer 和不改参数的 AU 输出观察；不向 QuickTime 注入 |
| `trace_playback_chain.py` / `inspect_decoder_format.cpp` | 只读追踪元数据交接、空间混音器选择及默认解码属性 |
| `make_playback_audition.py` | 无 BED、带淡入淡出的纯对象先左后右样本 |
| `make_music_playback_ab.py` / `make_vocal_controls.py` | 原曲短片段的音频遮罩、人声隔离和明确标记的 diffuse=0 对照 |
| `run_diffuse_playback.py` | 实时原生输出的扩散强度、分组、布局、数量及合成反例 |
| `extract_usb_audio.py` / `analyze_usb_playback.py` | 指定设备 USB OUT 提取、逐采样校验和独立测试音分析 |
| `summarize_playback_evidence.py` | 汇总本次 USB 固定采集窗口与人声输出证据 |
| `ida_extract.py` | 可选静态协议分析；输出应始终放在忽略的本地目录 |

## 单独调用

```text
codec_probe encode SETTINGS.plist INPUT.f32 INPUT_CHANNELS OBJECTS PREFIX CUSTOM_MODE BITRATE
codec_probe decode PREFIX DECODED.f32 OBJECTS
codec_probe import INPUT.caf_or_mp4 PREFIX
render_decoded DECODED.f32 DECODED_POSITIONS.csv OBJECTS BINAURAL.f32
```

编码输入包含额外元数据通道；成功用例使用 `CUSTOM_MODE=0`。`cumo=1` 不能当作对象模式开关。
输出前缀保存 `.packets`、`.packet_index.jsonl`、`.cookie`、`.asbd`、`.timing.json`。
`.asbd` 是同机研究格式，不是跨平台文件交换协议。
本版本中辅助性的 `get_acs` 回读可能返回 `!dat`；成功判定使用实际压缩包、原生解码及位置验证。

调试环境开关：

- 不设置 `APAC_PROFILE_CEILING`：只读观察，保留默认 `5/0,31/6`。
- `APAC_PROFILE_CEILING=1` 或 `2`：仅替换自建编码实例的能力表；解码用例必须清除此变量。
- `APAC_TRACE_POSITIONS=0`：保留调用与 Profile 观察，省略大量位置断点。
- `APAC_TRACE_ERRORS=1`：只读记录异常和元数据输出容量检查，不调整解码器。
- `APAC_TRACE_LFE=1`：只读记录 LFE 编码与解码函数，不调整编解码器。
- `APAC_BINARY_METADATA=1`：通过 `mdpf=1` 探测无 PCM 元数据通道时的 BinaryMetadataReader 选择；
  该开关没有提供二进制元数据 payload 的提交接口，不能当作位置输入。

完整运行器会清理继承的上述实验开关。不能把诊断生成的单组件 8／24 对象文件用作默认单组件入口支持它们的证据。
解码入口不设置私有属性，也不提供扩大解码缓冲区或修改解码能力表的开关。
源码、短样本、结构化结果可归档；不要归档系统二进制、IDA 数据库、反编译原文或系统日志。

```sh
python3 scripts/research/apac_objects/archive_results.py \
  --results local/apac-object-experiments/repro \
  --output local/apac-object-experiments/apac_object_experiments_20260922.zip
```

归档同时附带从解码结果离线生成的三个双耳 Float32 WAV，便于独立听取方向对照。
追加 `--extra-results local/apac-object-experiments/boundaries` 可把已核验的续测结果加入新归档。
续测的较大数量样本包含预期失败，是否完整解码必须以对应 `verification.json` 为准。
追加 `--mixed-results local/apac-object-experiments/lfe` 可加入对象＋LFE 的已核验结果及 CAF／MP4 样本。
追加 `--capacity-results local/apac-object-experiments/mixed-capacity` 可加入混合容量矩阵、反例和 CAF／MP4 样本。

## 真实 ADM 文件试编码

7.1.2 BED＋32 对象的全长结果及限制见
[`APAC_REAL_MEDIA_TRIAL.md`](../../../docs/architecture/APAC_REAL_MEDIA_TRIAL.md)。
Core Audio 的标准 `Atmos_7_1_2` 顺序是 `L R C LFE Ls Rs Rls Rrs Ltm Rtm`。
真实文件运行器沿用优化的 `codec_probe`，编解码器能力表均保持默认。

当前采用 `pos-only` 版本：保留对象音频与位置，全部 diffuse=0，尺寸省略。
历史 `pos-diffuse` 版本只完成了逐轨解码等验证，后来发现了扩散回放缺失，已停止交付。
完整尺寸映射仍会触发默认解码错误，不能宣称完整 ADM 语义转换。
`run_adm_trial.py` 的 `--omit-fully-diffuse-extent` 是显式研究退路；省略值逐项记录，不修改原文件。
真实媒体与其全长导出不自动加入合成样本研究归档。

`run_adm_trial.py --bitrate-kbps 12000` 可指定整个 BED＋对象场景的 12 Mbps 目标，
按全频轨道数重新分配各输出组件预算，并同步设置全局值。省略参数时沿用原来 10.512 Mbps 的本素材配置。
12 Mbps 版本实测压缩包平均约 11.754 Mbps，42 路全长默认解码及容器一致性通过；目标码率不保证等于文件实测速率。

**AVAssetReader 离线双声道边界：** 强制请求两声道时，每组件最多 7 对象的样本只保留每组第一路，
32 路仅有 5 路参与。`--objects-per-component 1` 可以绕开这个离线转换问题，输出仍将对象居中混入。
这不是 QuickTime 实时链路：QuickTime＋AirPods Max USB 实测原始分组的 32 条位置测试音均存在。
不要再将 groups1 版本推荐为 QuickTime 修正版；真实歌曲的 diffuse 相关人声缺失需单独处理。
`read_asset INPUT OUTPUT.f32 2` 请求公开双声道转换；再加起始秒数、持续秒数可检查片段。
保留默认声道数可传 `0`。原有无参数或仅输出路径调用仍然有效。

人声 USB 证据：原 diffuse=1 文件只输出约 85 ms，随后归零；相同音频仅改 diffuse=0 后恢复约 3 秒。
这是语义改变的诊断文件，不是完整 ADM 修复。小场景中有 diffuse=1 正例，也有混合场景反例，
不能把某个 8／9 对象对照外推成系统对象数上限。具体配置、命令和保留样本见回放报告。

用户已明确要求制作全长关闭扩散版。研究运行器新增显式 `--discard-diffuse`，
把全部对象的有效 diffuse 置零，源值留在 sidecar，默认仍保持原行为。
与 `--omit-fully-diffuse-extent` 同用时，先按源语义记录尺寸省略，再删除 diffuse；
输出名为 `pos-only`。验证对照源位置和用户指定的有效 diffuse，源 ADM 不改写。
全长结果在 `local/apac-object-experiments/tsuioku-12000kbps-no-diffuse/`。

48 秒片段由 `trim_asset.m` 使用 `AVAssetExportPresetPassthrough` 直通生成，
原 APAC 包和 cookie 不变；不能将用于校验的解码误认为重新编码。
清理后保留压缩样本、原始采集、可听 WAV、JSON 和命令；部分验证脚本需先重新生成暂存或解码 PCM。
历史完整反例文件的删除记录在 `local/apac-object-experiments/cleanup-20260924-objects/`。
