# CLI 用法指南

本文档是 `mradm` 命令行工具的完整参考：子命令、普通多声道输入、渲染后端、输出格式与布局、常用选项和语义策略。
项目概览见 [README](../../README.md)，C ABI 见 `include/adm/c_api.h` 与 [ADR 0007](../adr/0007-c-abi-stability-policy.md)。

## 子命令

| 子命令 | 用途 |
|---|---|
| `render` | 渲染 ADM BWF 或普通多声道 WAVE |
| `inspect` | 打印 ADM 或自动识别的声道床场景元数据；可写出语义策略模板 |
| `backends` | 列出可用渲染后端、能力与支持布局 |
| `layouts` | 查询某输出格式（可按后端过滤）的最终声道顺序 |
| `input-layouts` | 列出普通多声道输入预设与标签几何 |
| `formats` | 列出输出容器格式、可用性与约束（声道数、采样率、位深、码率） |
| `export` | 把应用语义策略后的场景写回新的 ADM BWF（复用源 PCM，不重渲染） |

每个子命令都支持 `--help`。示例中的 `./build/release/mradm` 可替换为发行包内的 `bin/mradm`。

## 快速示例

可先查看 ADM 场景、可用后端和布局：

```bash
./build/release/mradm inspect input.wav
./build/release/mradm backends
./build/release/mradm input-layouts
./build/release/mradm layouts --format wav
./build/release/mradm layouts --format flac --renderer saf
./build/release/mradm formats
```

常见渲染命令：

```bash
./build/release/mradm render -i input.wav -o out_binaural.wav --renderer saf-binaural
./build/release/mradm render -i input.wav -o out_714.flac --renderer ear --output-layout 7.1.4
./build/release/mradm render -i input.wav -o out_222.wav --renderer apple --output-layout 22.2
./build/release/mradm render -i input.wav -o out_room.wav --renderer triple-balance --output-layout 9.1.6
./build/release/mradm render -i input.wav -o out_trim.wav --start 12.5 --end 45.0
./build/release/mradm render -i bed.wav -o bed_714.wav --input-layout 5.1 --renderer ear --output-layout 7.1.4
./build/release/mradm render -i custom.wav -o custom_binaural.wav --input-channels L,R,C,LFE,M+090,M-090 --renderer saf-binaural --sofa listener.sofa
```

## 普通多声道输入

普通输入支持 PCM 16/24/32-bit 与 IEEE float32 WAVE / RF64 / BW64，限制为 1–64 声道。
默认 `--input-layout auto` 按优先级识别：`axml` 使用 ADM 导入，随后识别
WAVEFORMATEXTENSIBLE channel mask；无效 ADM 直接报错。也可显式选择
`5.1`、`5.1.2`、`7.1`、`5.1.4`、`7.1.4`、`9.1.4`、`9.1.6`、`22.2`，或用
`--input-channels` 按文件顺序给出受控标签。

坐标约定为方位角 `+` 向左、`-` 向右、`0°` 正前，仰角 `+` 向上。别名具有固定语义：
`L/FL=M+030`（`+30°`, `0°`）、`R/FR=M-030`（`-30°`, `0°`）、
`C/FC=M+000`（`0°`, `0°`）、`LFE=LFE1`（低频语义声道）。自定义标签数量必须与文件
声道数完全一致，空项、未知项和重复项均报错；`U±110` 必须以 `@30` 或 `@45` 指定仰角。
完整预设顺序、几何、mask 和约束见[普通多声道输入语义](CHANNEL_BED_INPUT.md)，也可运行
`mradm input-layouts` 或 `mradm input-layouts --format json` 查询。

公开两声道输出采用 `binaural` 语义，并作为默认输出。后端与 HRTF 来源分别选择：
`triple-balance` 是独立的房间坐标渲染后端，点源沿 X/Y/Z 三轴逐层进行等功率声像分配，
并提供已验证的 48 kHz 等尺寸对象处理。7.1.4 / 9.1.6 保留现有参考对齐行为；22.2 为项目自有扩展。
当前支持范围和输入限制见 [Triple Balance 渲染器](../architecture/TRIPLE_BALANCE_RENDERER.md)。
CLI、C ABI 和 GUI 批量渲染可选；C API 和 GUI 系统空间音频监听支持实时播放、精确跳转、增益／静音与等尺寸倍率编辑。旧 `--speaker-panner` 参数已删除。

`saf-binaural` 提供内置 KEMAR 与构建支持时的 `--sofa` 用户 HRIR；`apple` 使用 Apple 系统 HRTF。
当前入口覆盖 CLI、C++ API 与 C ABI。

## 渲染后端

| 后端 | CLI 选项 | 输入类型 | 输出 |
|---|---|---|---|
| EAR（兼容名 `libear`） | `--renderer auto` / `ear` | Objects / DirectSpeakers / HOA | 多声道扬声器 |
| SAF VBAP | `--renderer saf` | Objects / DirectSpeakers | 多声道扬声器 |
| Triple Balance | `--renderer triple-balance` | Cartesian Objects / 标准 7.1.2 bed | 7.1.4 / 9.1.6 / 22.2，离线与实时监听 |
| HOA 编码 | `--renderer hoa` | Objects / DirectSpeakers | HOA3 16ch（ACN/SN3D） |
| SAF HRTF 双耳 | `--renderer saf-binaural` | Objects / DirectSpeakers | 2ch 双耳 |
| Apple AUSpatialMixer | `--renderer apple` | Objects / DirectSpeakers | 2ch 双耳 / 多声道扬声器（macOS） |

`saf-binaural` 默认使用 SAF 内置 Genelec KEMAR HRTF，也可通过 `--sofa <path>` 加载用户 FIR SOFA HRIR 文件。当前 SOFA 规格为 SimpleFreeFieldHRIR / GeneralFIR、2 receivers、48 kHz 原生采样率。`apple` 使用 Apple 系统 HRTF；可运行 `mradm backends` 查看每个双耳后端的 `HRTF sources`。

推荐的通用外部 HRTF 是 [SADIE II Database](https://www.york.ac.uk/sadie-project/database.html) 的 D1 KU100 SOFA，例如 `D1_48K_24bit_256tap_FIR_SOFA.sofa`（也可从 [SOFA database SADIE 索引](https://sofacoustics.org/data/database/sadie/) 下载）。它是 48 kHz、256-tap、SimpleFreeFieldHRIR，方向采样密度高，并带 low-frequency extension / diffuse-field EQ，适合作为均衡的 `--sofa` 默认推荐。SADIE II 数据集由 University of York 以 Apache License 2.0 发布；若分发素材或学术使用，请按其页面说明引用论文 [DOI:10.3390/app8112029](https://doi.org/10.3390/app8112029)。

`apple` 使用 AudioToolbox AUSpatialMixer，提供 Apple 平台渲染语义，支持 binaural、5.1、7.1、5.1.2、5.1.4、7.1.4、9.1.6 和 22.2，以及扬声器输出的 channelLock 与 extent 点云近似。扬声器输出默认清除 SpatialMixer 的 InterAuralDelay / DistanceAttenuation flags，以匹配 ADM / SAF / Logic 式增益语义；`--apple-speaker-rendering-flags` 可恢复 Apple 原生 flags（旧行为）。`--apple-spatial-preset headphone-default|headphone-movie` 为 Apple binaural 应用 AUSpatialMixer headphone factory preset。Apple 动态参数平滑由 SpatialMixer 内部处理。`--start` / `--end` 使用按需窗口渲染和一个 render block 的 pre-roll 更新 SpatialMixer 内部状态。

## 输出格式

### 编码与容器总览

| 编码 | 有损 / 无损 | 当前容器 | 扩展名 | 状态 |
|---|---|---|---|---|
| PCM float32 | 未压缩 | WAV / CAF | `.wav` / `.caf` | 全平台支持 |
| PCM integer | 未压缩 | WAV | `.wav` | 全平台支持；24-bit / 16-bit |
| FLAC | 无损压缩 | FLAC | `.flac` | 全平台支持；当前固定 24-bit，最多 8 声道 |
| Opus | 有损 | Matroska Audio | `.mka` | 全平台支持；Opus VBR |
| Opus | 有损 | IAMF raw OBU | `.iamf` | 需启用官方 AOM iamf-tools bridge 预构建 SDK |
| APAC | 有损 | MPEG-4 Audio | `.m4a` / `.mp4` | macOS；AudioToolbox |
| APAC | 有损 | CAF | `.caf` | macOS；使用 `--apac-container caf` |

“状态”列给出本项目当前可写出的编码 / 容器组合；目标系统和播放器分别决定布局识别与直接回放能力。`.caf` 默认写 PCM float32，`--apac-container caf` 选择 APAC-in-CAF。

### 未压缩 / 无损输出

WAV 可写 float32 / 24-bit / 16-bit PCM；`--output-bit-depth` 的作用域为 WAV。最终 WAV 携带机器可读布局语义：`5.1`、`5.1.2`、`7.1`、`5.1.4`、`7.1.4` 写 WAVEFORMATEXTENSIBLE mask，并严格采用升序 mask 位序；`7.1.4` 因而写成 `L R C LFE Rls Rrs Ls Rs ...`。`9.1.4`、`9.1.6`、`22.2` 写 ADM DirectSpeakers AXML/CHNA；`binaural` 写 ADM Binaural `leftEar/rightEar`；`hoa3` 写 ADM HOA ACN/SN3D AXML/CHNA 与 `ambi` chunk。

WAV 输出与母版输入都支持超过 4GB。float32 固定使用 RF64；带 ADM 语义的 float32 是携带 AXML/CHNA 的 RF64 扩展，整数 `i24` / `i16` 写规范 PCM BW64。使用 WAVE mask 的整数文件在 4GB 内写 RIFF，超过 4GB 时写 RF64，以保留 WAVEFORMATEXTENSIBLE 语义。读取端支持 RIFF / RF64 / BW64，并可读回本项目的 float32 ADM RF64。交付链明确要求 PCM BW64 时，使用 `--output-bit-depth i24`。CAF 当前固定写 float32 PCM，适合作为 CoreAudio 生态下携带空间布局标签的未压缩容器。FLAC 当前固定写 24-bit lossless，最多 8 声道，布局集合为 `binaural`、`5.1` 和 `7.1` 等基础层布局。高度声道的无损交付使用 WAV 或 CAF。

带高度或超过 8 声道的无损 / 未压缩交付优先使用 WAV 或 CAF。播放器兼容性验证覆盖目标播放器、容器、布局标签和声道数。

### 有损交付输出

Opus MKA 是 Matroska Audio + Opus VBR，全平台可写。标准 5.1 / 7.1 使用 Opus/Vorbis 声道语义；9.1.6、22.2 等更高阶离散布局使用透明多流编码并记录 metadata，完整空间布局识别取决于播放器能力。

IAMF 输出为 raw OBU stream（`.iamf`）+ Opus，面向 IAMF 测试和交付链路。IAMF 编码依赖官方 AOM iamf-tools bridge；构建时设置 `-DMR_ADM_ENABLE_IAMF=ON -DMR_ADM_IAMF_AOM_ROOT=/path/to/iamf-sdk`，其中 SDK 提供 `lib/libmr_iamf_aom_bridge.*`。当前 IAMF 布局范围覆盖到 `7.1.4`。默认 IAMF 按最终 `--output-layout` 写单层；scalable channel layers 使用 `--iamf-layers 5.1,5.1.2,5.1.4,7.1.4`，最后一层对应输出布局，层级单调增加。高度输出包含平面层时记录 warning，供用户按交付目标决策。

APAC 在 macOS 上通过 AudioToolbox 写入 MPEG-4 Audio 容器（`.m4a` / `.mp4`），当前采样率为 48 kHz。APAC-in-CAF 使用 `.caf` 输出路径和 `--apac-container caf`。空间布局和 HOA 默认使用稳定的总目标码率提示：以 `7.1.4` 的 2048 kbps 为 12 声道基准按声道数缩放，例如 `5.1.4` 约 1707 kbps，`9.1.6` / `hoa3` 约 2731 kbps，`22.2` 约 4096 kbps。该值传给 AudioToolbox 作为编码码率目标 / 提示，实际统计码率可能明显偏离目标值。

### 容器、布局与回放

声道顺序和布局语义由“编码 + 容器 + layout tag / mapping”共同决定。同一编码在不同容器里的布局表达可能不同，同一容器也可承载不同编码。`mradm layouts --format <fmt>` 查询的是当前已实现组合的最终声道顺序。

回放兼容性取决于容器、布局和播放器。macOS 已验证 PCM CAF、APAC `.mp4/.m4a` 以及 APAC-in-CAF 空间音频直放。实测 PotPlayer 可回放离散声道 Opus MKA，并下混到 8ch；部分具备系统 Opus decoder（如 `c2.android.opus.decoder` / `OMX.google.opus.decoder`）的 Android 设备可回放最高 `9.1.6` 的 Opus MKA。空间布局语义保留程度以目标播放器验证结果为准。

HOA 直接回放在 macOS 上使用 CAF PCM、APAC MPEG-4 与 APAC CAF。WAV HOA3 写 AmbiX `ambi` chunk，服务支持 AmbiX 的工具链。Opus MKA 写 ambisonics mapping；已验证 VLC 4.0 可回放 Opus HOA3，播放时在音频选项中将 mix node 从 `original: ambisonics` 改为 `binaural`，由 VLC 解码到双耳输出。

## 输出布局

| 常用名称 / CLI 值 | 声道数 | EAR | SAF VBAP | Apple |
|---|---:|---|---|---|
| `5.1` | 6 | yes | yes | yes |
| `5.1.2` | 8 | yes | yes | yes |
| `7.1` | 8 | yes | yes | yes |
| `5.1.4` | 10 | yes | yes | yes |
| `9.1.4` | 14 | yes | yes | - |
| `7.1.4` | 12 | yes | yes | yes |
| `9.1.6` | 16 | yes | yes | yes |
| `22.2` | 24 | yes | yes | yes |
| `hoa3` | 16 | - | - | - |

EAR 与 SAF VBAP 的扬声器布局能力共享同一份项目 registry；`9.1.4` / `9.1.6` 在 EAR 后端由项目侧自定义布局实现。

EAR / SAF 的输出扬声器几何可用 `--speaker-geometry standard|apple` 切换。默认 `standard`
保持既有项目 / ADM 标称坐标；`apple` 使用 CoreAudio 固定坐标，覆盖 `5.1`、`7.1`、
`5.1.2`、`5.1.4`、`7.1.4`、`9.1.6` 与 `22.2`（其中 `5.1` 坐标相同）。CoreAudio
没有本项目 `9.1.4` 和内部 speaker-stereo 的对应固定布局，因此这两种组合会明确报 unsupported。
Apple 后端始终使用 CoreAudio 几何，不受该选项影响；LFE 不参与几何切换。

DirectSpeakers 可用 `--direct-speakers-routing auto|label|position|matrix` 切换。默认 `auto` 在 SAF / Apple
扬声器上解析为 `label`：先精确
匹配输出标签，再用共享别名（例如 `L` → `M+030`），命中后 one-hot 直达输出槽位；未命中按坐标
空间化：已知 BS.2051 / 别名标签优先使用标签方向，否则使用 ADM 标称坐标。`position` 忽略非 LFE
标签，以块的标称坐标做零扩散、无插值空间化；SAF 使用当前 `--speaker-geometry`，Apple
使用 AmbienceBed。需要坐标回退但缺少坐标时使用 `(0°,0°)` 并 warning。

`matrix` 配合 `--direct-speakers-matrix <json>`，在 EAR、SAF 与 Apple 扬声器输出上把每个非 LFE
输入标签按稀疏权重送往一个或多个目标标签。每行权重固定归一为 `sqrt(weight/sum)`；也可用
`mute:true` 显式静音。profile 必须绑定当前输出布局，并完整覆盖素材中每个非 LFE DirectSpeakers
块；重复、未知、歧义标签与 LFE source/target 都会报错。LFE 始终先走现有专用路由。
Apple binaural 的 `auto` 仍为 `position`，显式 `label` 返回 unsupported，并拒绝 `matrix`；EAR 只新增
`matrix`，原有显式 `label` / `position` 仍不支持；SAF binaural 与 HOA 保持 `auto` 原生行为并拒绝所有
显式模式。本阶段只提供 CLI、C++ 与 C ABI，GUI 控件后置。
桌面端在 EAR / SAF 扬声器渲染器下提供同一项“扬声器几何”下拉选择，并记住上次选择。

```bash
./build/release/mradm render -i input.wav -o saf_apple_222.wav \
  --renderer saf --output-layout 22.2 --speaker-geometry apple

./build/release/mradm render -i bed.wav -o remapped_714.wav \
  --renderer ear --output-layout 7.1.4 \
  --direct-speakers-routing matrix --direct-speakers-matrix routes.json
```

声道顺序取决于最终输出格式。完整表可用 CLI 查询：

```bash
./build/release/mradm layouts --format wav
./build/release/mradm layouts --format caf
./build/release/mradm layouts --format apac
./build/release/mradm layouts --format flac --renderer ear
```

常见差异示例：

| 格式 | Layout | 最终容器 / 映射 | 最终声道顺序 |
|---|---|---|---|
| WAV / FLAC | `7.1` | WAVE_7_1 / `wav71` | L R C LFE Rls Rrs Ls Rs |
| WAV | `7.1.4` | WAVEFORMATEXTENSIBLE `0x2D63F` | L R C LFE Rls Rrs Ls Rs U+045 U-045 U+135 U-135 |
| WAV | `9.1.4` / `9.1.6` / `22.2` | ADM DirectSpeakers AXML/CHNA | `mradm layouts --format wav` 所列的精确 ADM 顺序 |
| WAV | `binaural` | ADM Binaural `leftEar/rightEar` AXML/CHNA | leftEar rightEar |
| APAC / M4A | `7.1` | CoreAudio `AudioUnit_7_1` | L R C LFE Ls Rs Rls Rrs |
| APAC / CAF | `9.1.6` | CoreAudio `Atmos_9_1_6` | L R C LFE Ls Rs Rls Rrs Lw Rw Vhl Vhr Ltm Rtm Ltr Rtr |
| APAC / CAF | `22.2` | CoreAudio `CICP_13` | Lw Rw C LFE2 Rls Rrs L R Cs LFE3 Lss Rss Vhl Vhr Vhc Ts Ltr Rtr Ltm Rtm Ctr Cb Lb Rb |
| WAV | `hoa3` | ADM HOA AXML/CHNA + AmbiX `ambi` chunk | ACN/SN3D 16ch |
| CAF / APAC | `hoa3` | CoreAudio `HOA_ACN_SN3D` | ACN/SN3D 16ch |

## 常用 CLI 选项

| 选项 | 说明 | 默认值 |
|---|---|---|
| `--renderer auto\|ear\|saf\|triple-balance\|hoa\|saf-binaural\|apple` | 选择渲染后端 | `auto` |
| `--input-layout auto\|5.1\|5.1.2\|7.1\|5.1.4\|7.1.4\|9.1.4\|9.1.6\|22.2` | 普通 WAVE 输入布局；`auto` 优先 ADM，其次识别 channel mask | `auto` |
| `--input-channels <csv>` | 自定义普通输入标签，严格按文件声道顺序；与显式 `--input-layout` 二选一 | 关闭 |
| `--output-layout <layout>` | 输出语义或布局：`binaural`、多声道布局或 `hoa3` | `binaural` |
| `--speaker-geometry standard\|apple` | EAR / SAF 输出扬声器坐标；Apple 后端始终使用 CoreAudio 几何 | `standard` |
| `--direct-speakers-routing auto\|label\|position\|matrix` | DirectSpeakers 原生、标签、位置或自定义标签矩阵路由 | `auto` |
| `--direct-speakers-matrix <path>` | `matrix` 模式所需的严格 v1 稀疏矩阵 JSON | 关闭 |
| `--output-bit-depth f32\|i24\|i16` | WAV 输出位深（CAF 固定 float32；FLAC 固定 24-bit / 最多 8 声道） | `f32` |
| `--loudness-target <LUFS>` | 响度归一化目标；HOA 通过 7.1.4 AllRAD 参考解码测量，LUFS 使用全频声道 | 关闭 |
| `--peak-limit-dbtp <dBTP>` | True Peak 限制目标 | `-1.0` |
| `--peak-normalize-to-limit` | 在响度增益后，如 True Peak 低于 `--peak-limit-dbtp`，自动补全局增益到上限；需要开启 True Peak 限制 | 关闭 |
| `--final-gain-db <dB>` | 在响度 / 峰值自动增益与 True Peak 自动限制之后追加最终增益，可超过 0 dBFS | `0` |
| `--no-peak-limit` | 将 True Peak 限制设为 off | - |
| `--start <sec>` | 从渲染时间线该秒数开始裁剪输出；响度 / True Peak 计量范围为保留片段 | `0` |
| `--end <sec>` | 裁剪到渲染时间线该绝对秒数；取值大于 `--start`，默认渲染到结尾 | 关闭 |
| `--interp-ms <ms>` | ADM 块省略 jumpPosition 时的增益插值斜坡 | `5` |
| `--object-smoothing-frames <frames>` | Objects 动态元数据去拉链平滑窗口；`0` 为逐样本跟随 ADM 块；极端动态素材可显式调大；Apple 后端由 SpatialMixer 负责动态平滑 | `0` |
| `--apple-spatial-preset off\|headphone-default\|headphone-movie` | Apple binaural 专用 AUSpatialMixer factory preset；preset 会先应用，再重新写入 ADM 驱动的空间参数 | `off` |
| `--apple-speaker-rendering-flags` | Apple 扬声器输出开启 InterAuralDelay + DistanceAttenuation flags，恢复旧行为；默认关闭以匹配 ADM / SAF 增益语义 | 关闭 |
| `--listener-yaw <deg>` / `--listener-pitch <deg>` / `--listener-roll <deg>` | Apple binaural 专用听者头部朝向；yaw 为 `[-180, 180]` 且 `+` 向左，pitch 为 `[-90, 90]` 且 `+` 向上，roll 为 `[-180, 180]` | `0` |
| `--opus-bitrate-per-ch <kbps>` | Opus VBR 目标比特率 / 声道 | 自动 |
| `--apac-bitrate <kbps>` | APAC 总目标比特率提示；默认按 7.1.4=2048 kbps 基准缩放空间布局 / HOA | 见输出格式说明 |
| `--apac-container mpeg4\|caf` | APAC 容器；`caf` 要求输出路径为 `.caf`，普通 `.caf` 默认仍为 PCM | `mpeg4` |
| `--sofa <path>` | 为支持 `user-sofa` 的双耳后端选择用户 SOFA HRIR；当前为 `saf-binaural` | SAF 内置 KEMAR |
| `--semantic-policy <path>` | 渲染时应用 ADM 语义控制 JSON（覆盖 Objects / DirectSpeakers / HOA 的 gain·mute·position 及 diffuse / extent / divergence / channelLock / 插值等） | 关闭 |
| `--write-semantic-report <path>` | 写出 policy 应用后的 effective semantic JSON，便于确认对象 / DS / HOA 规则命中与 original→effective 变化 | 关闭 |

响度相关后处理顺序为：`--loudness-target` 先决定目标响度增益，`--peak-normalize-to-limit` 可选补峰到 True Peak 上限，`--peak-limit-dbtp` 裁剪自动阶段的全局增益；`--final-gain-db` 在这些自动阶段之后追加，故会绕过 True Peak 限制。

## 语义策略

语义策略（semantic policy）应用于本次渲染，原始 AXML 保持原样。`inspect --write-semantic-policy-template` 会按场景生成一份可编辑的中性模板；原样应用对应 identity operation，编辑后用 `--semantic-policy` 应用：

```bash
./build/release/mradm inspect in.wav --write-semantic-policy-template policy.json
./build/release/mradm render -i in.wav -o out.flac --renderer saf-binaural --semantic-policy policy.json \
  --write-semantic-report report.json
# 把应用策略后的场景写回新的 ADM BWF（复用源 PCM，不重渲染）
./build/release/mradm export -i in.wav -o out.adm.wav --semantic-policy policy.json
```

`--write-semantic-report` 写出策略应用后的生效语义快照，用于确认规则命中与 original→effective 变化。

`global` 作用于全部内容，`objects[]` 是按规则匹配的覆盖。匹配维度（OR 组合）：`id` / `name` / `name_glob` / `track_uid` / `all` / `importance_min·max` / `dialogue_id` / `content` / `programme`，以及 HOA 专用的 `pack_format`。覆盖项：

- **Objects**：`gain`（`scale` / `gain_db` / `mute`，object 级）、`position`（绝对 `azimuth/elevation/distance` + `offset` + `lock_*`）、`diffuse` / `extent` / `divergence` / `channel_lock`（含 `max_distance`）/ `interpolation`。
- **DirectSpeakers**：`direct_speakers`，含块内过滤器 `speaker_label` / `lfe`（AND）+ `gain`（`mute` 即静音该声道）+ `position` 重瞄。
- **HOA**：匹配 `id` / `pack_format` / `all`，应用 `gain`（`scale` / `gain_db` / `mute`）到整个 pack。

```json
{
  "schema": "mradm.semantic-policy.v1",
  "global": { "gain": { "gain_db": -3 } },
  "objects": [
    { "name_glob": "*kick*", "diffuse": { "enabled": false }, "extent": { "enabled": false } },
    { "dialogue_id": 1, "gain": { "gain_db": 2 } },
    { "id": "AO_1003", "position": { "azimuth": 30, "lock_elevation": 0 } },
    { "all": true, "direct_speakers": { "lfe": true, "gain": { "gain_db": -6 } } },
    { "pack_format": "AP_00031001", "gain": { "scale": 0.5 } }
  ]
}
```

更多选项可查看：

```bash
./build/release/mradm render --help
```
