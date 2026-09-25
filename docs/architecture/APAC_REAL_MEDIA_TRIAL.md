# APAC 真实 ADM 素材试编码：7.1.2 BED＋32 对象

2026-09-25 整理：本地生成的 CAF、MP4、PCM 和完整实验目录已清理。
以下文件名、路径、字节数与哈希是历史实验记录；挑选的结果 JSON 在[数值证据目录](evidence/apac_objects/README.md)。
当前编码方法见[对象研究交接](APAC_OBJECT_HANDOFF.md)。

实验始于 2026-09-23；交付确认更新于 2026-09-24。macOS 27.0 / 26A428，AudioCodecs.component 7.0。

**当前交付为用户已确认正常的 `pos-only_12000kbps` 无扩散对象版。**
保留 7.1.2 BED、32 个独立对象及其位置，全部 diffuse=0，尺寸省略。
全长文件和无重编码的 10–58 秒片段见 [对象交接文档](APAC_OBJECT_HANDOFF.md)。
以下保留含 diffuse 的首轮试验和后续修正过程；完整 ADM 扩散／尺寸语义尚未实现。
编码能力表和解码器均未修改。私有入口仅用于编码器的 ASC 配置；解码元数据观察是进程内只读断点。

**回放结论更新：** [QuickTime／USB 实测](APAC_PLAYBACK_CHAIN.md) 已证明原始多对象分组能够播放全部
32 条非扩散测试音，并按位置渲染。此前 AVAssetReader 强制双声道的结果不能代表 QuickTime。
但这首歌的人声对象在当前 `pos-diffuse` 映射下确有回放缺失：`diffuse=1` 的隔离人声只在开头短暂输出，
随后归零；只把 diffuse 改为 0 的诊断文件恢复持续输出。原始分组和单对象分组均不能据此宣称完整回放通过。

## 输入与标准 BED 标签

输入为用户提供的 `Tsuioku_SS2RPV4.wav`：48 kHz、PCM24、42 路、7,560,307 帧，
时长 157.506395833 秒。RIFF 含 `axml`、`chna` 和 `dbmd`；AXML、CHNA 与原始 PCM 分别解析，源文件未改写。
源文件 SHA-256：`950f056909e76662ac995b14f2f1634b3bdfbc853d955cb4945cf505fd91af5d`。

Core Audio 提供 `kAudioChannelLayoutTag_Atmos_7_1_2 = (196 << 16) | 10`。
该素材最前面十条 CHNA 轨道恰好对应它的标准顺序：

| WAV 索引，从 0 起 | ADM 标签 | Core Audio 标签后缀 | 数值 |
| --- | --- | --- | ---: |
| 0 | RC_L | Left | 1 |
| 1 | RC_R | Right | 2 |
| 2 | RC_C | Center | 3 |
| 3 | RC_LFE | LFEScreen | 4 |
| 4 | RC_Lss | LeftSurround | 5 |
| 5 | RC_Rss | RightSurround | 6 |
| 6 | RC_Lrs | RearSurroundLeft | 33 |
| 7 | RC_Rrs | RearSurroundRight | 34 |
| 8 | RC_Lts | LeftTopMiddle | 49 |
| 9 | RC_Rts | RightTopMiddle | 51 |

输入配置的 Channel Bed 使用上述完整 `kAudioChannelLabel_…` 名称列表。
整个流还带对象，因此解码布局以 42 条 descriptions 表达，不能把整个流标成只有十路的 7.1.2 tag。
床的声道身份依据标准标签；源 ADM 中表示房间角落的 Cartesian 坐标不另写成自定义扬声器位置。

其余 32 条轨道是静态单声道对象，按 `7+7+7+7+4` 分为五个 Object 组件。
混合 renderer group 0 是 BED，group 1～32 与原始轨道 10～41 一一对应。
APAC 对象标签在不同组件中会重复，不能单靠标签值恢复全局对象身份。

## 首轮含 diffuse 版本的编码及元数据

- 沿用优化后的独立 `codec_probe`，不走 Debug 渲染，也未接入产品 CLI／GUI／C ABI。
- 每个全频声道目标 256 kbps，LFE 16 kbps；BED 显式 2.320 Mbps，总目标 10.512 Mbps。
  实际压缩包平均约 10.3763 Mbps。实际 Profile／Level 为 **5 / 0**。
- 42 路音频逐轨转换 PCM24 → Float32，无音频渲染、合并、归一化或 LFE 加 10 dB。
  第 43 路仅传输 AIA 1.4 元数据，每 1024 个采样更新一次。
- Cartesian 对象位置按 `az = atan2(-X,Y)`、`el = asin(Z/r)`、`r = sqrt(X²+Y²+Z²)`
  转为 APAC 球坐标，范围设为 2。这里是几何坐标转换，未实现特定播放器的房间映射。
- diffuse 通过 renderer parameter 3 保留，原素材 22 路为 1，其余为 0。
- 为让最后不足一帧的元数据完整，输入尾部补 909 个零采样。编码器给出 2048 个前导帧。
  原生共解码 7,563,264 帧；容器记录有效 7,560,307、前导 2048、尾部 909，时长与源 PCM 严格一致。
  原始编码 timing 另存为 `stream.encoder-timing.json`，调整只排除本工具补入的音频零值。

## 已验证结果

本地证据根目录：`local/apac-object-experiments/tsuioku/`。

| 检查 | 结果 |
| --- | --- |
| 全长默认原生解码 | 全部 7386 个包成功，无跳过坏包，42 路均非静音 |
| 逐轨音频对应 | 按前导帧对齐后，检查全曲每一路能量、峰值、误差、跨轨相关性 |
| 41 路非 LFE | 最低同轨相关系数 0.9998209077；最低全曲 SNR 34.4057 dB |
| LFE | 相关系数 0.9977661507；能量增益 -0.01959 dB；SNR 23.5040 dB |
| 默认解码布局 | 前十路准确恢复 `[1,2,3,4,5,6,33,34,49,51]`，其后 32 路为对象 |
| 位置与扩散 | 全长码流头部、末尾独立窗口，32 个 group 全覆盖；diffuse 逐对象吻合 |
| 位置量化 | 方位／仰角／距离精度 9／8／7 bit；最大误差约 0.28648°／0.27623°／0.007144 |
| LFE 分支 | 活跃短片段与全长末尾窗口观察到原生 `APACLFEElement::Deserialize` |
| CAF／MP4 | 重新导入后，压缩字节、magic cookie、有效帧及前后填充全部一致 |
| AVAssetReader | 两种容器均读到 7,560,307 帧、42 路，完成状态正常 |

音频指标来自全长默认原生解码，没有使用旁路音频。位置和 diffuse 来自反序列化后的原生 metadata sink，
不是从原始 AXML 或容器复制坐标。静态位置观察覆盖全长头尾及中段独立短片段，没有宣称逐帧观察全曲元数据。

AVAssetReader 的 RMS 约 0.02862086，而原生逐轨解码约 0.01717784；二者存在明显能量差异。
这与此前多组件对象标签重复、AVAssetReader 可能重混的风险一致，但本轮没有进一步比较其逐轨 PCM。
**AVFoundation 完整读帧不等于系统播放器准确采用对象、LFE 和空间渲染语义。**
这一阶段未进行听音或系统直接空间播放验收；后续实际回放证据及失败边界见上述更新报告。

## 尺寸字段的具体阻塞点

源素材的 22 个非零 `width/height/depth` 都与 `diffuse=1` 共存；另外十个对象尺寸为零。
加入 renderer parameter 1 的 Cartesian spread 后，编码器仍输出压缩包，但默认解码在首包 ProduceOutput 返回 `-50`。
只读系统日志报告 `numBlks > MaxNumBlks`，随后是 RendererMetadata 的反序列化错误。
移除 spread、保留相同音频与位置／diffuse 后，原生解码通过。

已保留不同精度、去掉 depth、Cartesian 位置及 angular spread 等受控变体；这些变体同样失败。
这定位的是**本次尺寸协议映射／元数据序列化路径未打通**，不证明 APAC 格式不支持尺寸，
也不归因为解码器容量不足。未为绕过失败而修改系统解码器。

交付文件名包含 `pos-diffuse`。运行器必须显式使用 `--omit-fully-diffuse-extent` 才会省略尺寸，
且拒绝对 `diffuse != 1` 的对象采用这一退路。所有原始尺寸值保存在 `full-input/expected.json` 与 `summary.json`。
虽然 ADM 全扩散对象的直接定位部分通常不贡献能量，本轮没有据此声称省略尺寸在系统播放中必然等价。
对象字符串名称／ID、绝对 programme timecode、未解释的 Dolby `dbmd` 没有映射进 APAC；源值或原始文件仍保留。

后续 Core 接入前，应先查清 AIA spread 的配置、初始状态与码流读写协议，并解决 AVFoundation 逐轨输出差异。
当前仅适合作为明确标注范围的研究导出，不宜承诺通用 ADM 无损语义转换。

## 文件、复现与清理

交付目录：`local/apac-object-experiments/tsuioku/deliverables/`：

- `Tsuioku_SS2RPV4_7.1.2BED_32Objects_pos-diffuse.caf`：204,314,369 字节，约 195 MiB。
- `Tsuioku_SS2RPV4_7.1.2BED_32Objects_pos-diffuse.mp4`：204,328,785 字节，约 195 MiB。

`summary.json`、`audio-verification.json`、`metadata-verification.json`、`tail-metadata-verification.json`、
`container-verification.json`、`extent-controls.json`、`commands.sh` 和 `SHA256.json` 保存结构化证据。
失败的含尺寸短片段压缩包与成功的中段／头尾短片段保留，便于继续诊断。
本次未归档或上传用户原始媒体。

在不存在的新结果目录复现交付路径：

```sh
python3 scripts/research/apac_objects/run_adm_trial.py \
  /Users/Sakuzy/Downloads/Tsuioku_SS2RPV4.wav/Tsuioku_SS2RPV4.wav \
  local/apac-object-experiments/tsuioku-new \
  --codec local/apac-object-experiments/lfe/bin/codec_probe \
  --omit-fully-diffuse-extent --cleanup
```

运行器只接受这一类静态单 BED／单轨对象的 PCM24 ADM，不是通用 ADM 解析替代品；遇到不支持的字段报错。
完整节目编解码各设 300 秒上限；短用例 30 秒、只读 LLDB 60 秒。
当前素材编码约 13.9 秒、默认解码约 3.84 秒，这只是一次运行记录，不作为正式性能基准。
`--cleanup` 在核验通过后移除大型 PCM 与重复的容器导入数据，保留删除清单及哈希。

最小失败复现：用 `make_adm_input.py` 截取源文件第 2,880,000 帧起的 8192 帧，不传尺寸省略开关，
以 43 路输入、42 路输出、10,512,000 bit/s 编码，再使用默认 `codec_probe decode` 解码。
对应现有证据为 `probe-input/`、`probe-encode/`、`probe-decode-plain/`。

## 12,000 kbps 追加版本

按用户要求，以相同源 PCM、相同 AIA 元数据和默认编解码能力完成全长提高码率试验。
新结果在 `local/apac-object-experiments/tsuioku-12000kbps/`，原版本保留。

| 指标 | 原版本 | 12,000 kbps 版本 |
| --- | ---: | ---: |
| 全部组件目标之和 | 10,512 kbps | 12,000 kbps |
| 实际压缩包平均码率，按有效节目时长 | 10,376.311 kbps | 11,754.439 kbps |
| MP4 总文件平均码率 | 约 10,378.18 kbps | 11,756.311 kbps |
| MP4 字节数 | 204,328,785 | 231,461,781 |
| 非 LFE 最低同轨相关系数 | 0.9998209077 | 0.9998852164 |
| 非 LFE 最低全曲 SNR | 34.4057 dB | 36.3346 dB |
| LFE 全曲 SNR | 23.5040 dB | 23.5042 dB |

新增 `--bitrate-kbps 12000`，单位是十进制 kbps。配置同时更新全局 `brat` 和每个输出组件的显式预算，
不能只改变全局属性。预算以全频轨道数分配，并在 BED 中保留名义 16 kbps 的 LFE 额度；
它不强制编码器内部逐声道的分配。
BED、四个七对象组件、末尾四对象组件的预算分别为：
`2646634、2046049、2046049、2046049、2046049、1169170` bit/s，合计精确为 12,000,000 bit/s。
整数舍入余量分配到组件；这是约每条全频轨道 292.293 kbps 的组件预算方案。

默认码率控制模式下，目标值不等于最终文件固定平均速率，故本次约 11.754 Mbps 是实际测量值。
不能把文件名中的 `12000kbps` 当成实测值，也没有通过填充文件凑出 12 Mbps。

新版本 42 路、7,560,307 个有效帧全部通过默认原生解码和逐轨对照；头部 32 个对象的 192 条元数据记录通过。
实际 Profile／Level 仍为 5／0，位置／diffuse 与原版本一致。
CAF、MP4 再导入后的压缩包、cookie、timing 均保持一致，文件哈希已核对。
音频误差指标有所改善，但这一阶段没有听音评分。22 个全扩散对象尺寸未写入的限制依旧适用；
后续 USB 回放又发现了 diffuse 相关的人声缺失，不能只凭逐轨音频对照将此版作为完整空间母版交付。

```sh
python3 scripts/research/apac_objects/run_adm_trial.py \
  /Users/Sakuzy/Downloads/Tsuioku_SS2RPV4.wav/Tsuioku_SS2RPV4.wav \
  local/apac-object-experiments/tsuioku-12000-new \
  --codec local/apac-object-experiments/lfe/bin/codec_probe \
  --omit-fully-diffuse-extent --bitrate-kbps 12000 --cleanup
```

新交付文件名为 `Tsuioku_SS2RPV4_7.1.2BED_32Objects_pos-diffuse_12000kbps.caf`／`.mp4`，各约 221 MiB。
`--cleanup` 核验通过后删除大型 PCM 和重复压缩数据，保存哈希及删除清单；原始媒体和两种交付容器保留。

## AVAssetReader 离线双声道：分组兼容性实验与解释更正

用户反馈 QuickTime 播放前述 MP4 时听起来只有 BED。进一步检查发现：
**此前的逐轨原生解码验收成立，但 `7+7+7+7+4` 对象分组不能完整通过 AVAssetReader 请求两声道的离线转换路径。**
曾据此推断 QuickTime 也丢失组内后续对象；该推断已被原生实时链路及 QuickTime USB 抓包推翻。
以下保留离线转换实验本身的有效结果，不能用于判断实时对象回放。

扩展 `read_asset.m`，通过公开 AVAssetReader 请求两声道 PCM，而非只读取 42 路。
以标准 7.1.2 BED＋32 条独立频率对象进行受控对照：

| 对象分组 | 系统双声道输出实际包含的对象 | 结论 |
| --- | --- | --- |
| `7+7+7+7+4` | 索引 `0,7,14,21,28`，仅 5 路 | 每个组件只有第一路对象进入输出，其余 27 路被漏掉 |
| `1×32` | 索引 `0..31`，全部 32 路 | 单对象组件避开本次丢声边界 |

这里的观测边界是公开 AVAssetReader 双声道输出；当时没有直接捕获 QuickTime 的音频设备输出。
原生输出布局中，旧分组返回 `Object, Object+1, …`，每个组件重新计数。
实测只有 `Object` 基值对应的对象进入这个混音，其余组内标签没有进入输出。
这是通道标签与默认折叠行为的关联证据，尚未追踪到系统混音器内部具体代码。
七对象控制中的 Apple／Atmos／EBU／VBAP renderer 标记变体没有改变这一输出结果。

真实歌曲的 60～63 秒窗口在该离线转换器中也得到相同结论：对原生解码音轨和离线双声道输出做线性拟合，
仅上述五条对象有约 `0.5 / 0.5` 的左右系数。主钢琴、贝斯等位于后续对象槽位的音轨没有进入输出。
这不是提高码率能解决的问题，也不是压缩包中不存在对象音频。

已用 `--objects-per-component 1 --bitrate-kbps 12000` 重编码全长版本，目录：
`local/apac-object-experiments/tsuioku-12000kbps-groups1/`。
仍保留一个十路 BED 和 32 个真正的 Object 组件，没有预先把对象烘焙进 BED 或新增立体声音轨。
源 PCM 和位置／diffuse 元数据与上一版一致；编码能力表与解码器继续保持默认。

- 实际压缩包平均 11,746.304 kbps；Profile／Level 为 5／0。
- 42 路原生全长解码、逐轨音频、32 个对象位置／diffuse、CAF／MP4 一致性均通过。
- 系统双声道完整输出 7,560,307 帧。
- 以源音轨构造同样的系统折叠矩阵，左右相关系数约 0.99999741／0.99999735，SNR 约 52.80／52.71 dB。
- 双声道绝对峰值约 0.74514／0.75147；没有超过 1.0 的采样。

**单对象组件改善的是 AVAssetReader 离线双声道输出，不能称作 QuickTime 人声修复。**
本次离线路径将各对象以 `0.5 / 0.5` 居中混入，未采用它们的方位、高度和 diffuse；
BED 按普通标签折叠，LFE 在此双声道矩阵中的系数为零。码流中仍保存 LFE 音频和对象元数据，
原生逐轨解码能够恢复它们。22 个对象尺寸未写入的原有限制也仍然存在。

分组控制证据：`local/apac-object-experiments/quicktime-object-playback/controls712/verification.json`。
真实歌曲双声道证据：同级 `music-group1-stereo-verification.json`；旧版拟合结果见 `asset-routing-regression.json`。
该历史对照由 `run_playback_controls.py` 复现；全长生成命令曾增加 `--objects-per-component 1`。
groups1 不是当前交付，旧全长媒体已在 2026-09-24 清理，短反例、配置、结果及哈希保留。

## 按用户要求关闭 diffuse 的全长版本

在确认人声扩散回放故障后，用户明确要求“抛弃扩散，做一版本”。
新增 `tsuioku-12000kbps-no-diffuse/`，文件名为
`Tsuioku_SS2RPV4_7.1.2BED_32Objects_pos-only_12000kbps.mp4`／`.caf`。
32 个对象全部以 diffuse=0 编码，实际移除 22 个非零源值；源值和有效参数分别保存在 expected.json。
位置、音频、原始增益、BED／LFE 及 7+7+7+7+4 分组保持原方案，尺寸继续沿用已声明的省略方式。

全长 7,560,307 个有效帧、42 路音频、32 对象位置和有效 diffuse=0 通过默认原生解码核验，
CAF／MP4 再导入的压缩包、cookie 和 timing 完全一致。Profile／Level 仍为 5／0。
实际压缩包平均 **11,741.898 kbps**；非 LFE 最低同轨相关系数 **0.9998852164**，
最低全曲 SNR **36.3346 dB**。系统编解码器、回放设置和原始 WAV 未修改。

这是用户选定的位置对象转换，不再保留 ADM 的扩散及尺寸效果。
2026-09-24 用户明确确认本对象版本正常；播放记录与用户确认分别保留。
这一确认不扩大为完整 ADM 语义一致性。

```sh
python3 scripts/research/apac_objects/run_adm_trial.py \
  /Users/Sakuzy/Downloads/Tsuioku_SS2RPV4.wav/Tsuioku_SS2RPV4.wav \
  local/apac-object-experiments/tsuioku-no-diffuse-new \
  --codec local/apac-object-experiments/lfe/bin/codec_probe \
  --omit-fully-diffuse-extent --discard-diffuse --bitrate-kbps 12000 --cleanup
```

## 10–58 秒片段与文件保留

从当前全长 MP4 直通裁切 [10.000, 58.000) 秒，得到精确 48 秒、2,304,000 个有效帧。
2,252 个 APAC 压缩包与源文件第 469 包起的数据逐字节一致，cookie 一致；
通过容器的 1,792 帧前导与 256 帧尾部余量表达边界，没有进行第二次有损编码。
片段保存在 `tsuioku-12000kbps-no-diffuse-10s-58s/`。
现有短名 `APAC_OBJ_Tsuioku.m4a`、`APAC_OBJ_TEST.m4a` 分别与对应全长／片段 MP4 内容相同，均保留。

本次整理清理三份旧含 diffuse 全长版本的 CAF／MP4、可再生成的大块 PCM 和完成后的分析缓存。
当前无扩散全长及片段、短反例、原始 USB 采集、源码、运行命令与验证 JSON 保留。
本报告前面的历史文件名、字节数和哈希仍是当时实验记录；现存文件以交接文档和清理清单为准。
普通 24 声道／CICP 13 的本地派生媒体也已在后续统一清理中移除；本报告只讨论对象实验。
