# MacinRender ADM Core

[English](README.en.md) | 中文

MacinRender ADM Core 是一个跨平台 ADM（Audio Definition Model，ITU-R BS.2076）空间音频渲染核心，使用 C++20 实现，提供命令行工具和 C ABI 库。

它面向 ADM BWF / BW64 输入，可渲染到多声道扬声器、HOA 编码、HRTF 双耳，以及 WAV / CAF / FLAC / Opus MKA / IAMF / APAC 等交付格式。

## 功能概览

- ADM scene import：基于 libbw64 / libadm 读取 BW64 ADM 元数据，并转换为项目自有领域模型。
- 多后端渲染：libear、SAF VBAP、HOA encoder、HRTF binaural、Apple AUSpatialMixer（macOS-only）。
- Objects / DirectSpeakers：支持对象和直达扬声器内容，含时间块、增益、插值、扩散、channelLock 和 objectDivergence 等语义。
- 输出后处理：响度归一化、True Peak 限制、位深转换、CAF/FLAC/Opus/APAC metadata。HOA 输出通过 7.1.4 AllRAD 参考解码测量，LFE 不计入 LUFS 但计入 True Peak。
- 平台边界：核心功能面向 macOS / Linux / Windows；APAC 编码与 Apple AUSpatialMixer 后端为 macOS-only。

## 快速开始

```bash
cmake -S . -B build/release -DCMAKE_BUILD_TYPE=Release
cmake --build build/release
```

构建后可先查看 ADM 场景、可用后端和布局：

```bash
./build/release/mradm inspect input.wav
./build/release/mradm backends
./build/release/mradm layouts --format wav
./build/release/mradm layouts --format flac --renderer saf
./build/release/mradm formats
```

常见渲染命令：

```bash
./build/release/mradm render -i input.wav -o out_binaural.wav --renderer saf-binaural
./build/release/mradm render -i input.wav -o out_714.flac --renderer ear --output-layout 7.1.4
./build/release/mradm render -i input.wav -o out_222.wav --renderer apple --output-layout 22.2
./build/release/mradm render -i input.wav -o out_trim.wav --start 12.5 --end 45.0
```

## C ABI 与 GUI 集成

`include/adm/c_api.h` 提供稳定 v1 C ABI。新 GUI 接入应优先使用 `adm_render_file_ex2` 和
`adm_preview_render_window_v2` 的结构化进度回调：事件包含稳定阶段枚举、操作枚举、整体进度、
阶段内进度以及渲染/后处理可用时的帧级 `current_frame / total_frames`。旧 `adm_progress_cb`
仍保留，用于兼容只需要单一 `fraction + stage + message` 的调用方；回调均为同步调用，字符串指针
只在 callback 期间有效。

## 发行包

GitHub Actions 的 release workflow 会在 tag `v*` 或手动触发时生成首版可审计发行包：

| 平台 | artifact | 支持基线 | 自包含边界 |
|---|---|---|---|
| macOS arm64 | `mradm-<version>-macos-arm64.tar.gz` | macOS 26 runner 构建 | 不依赖 Homebrew / `/usr/local` 动态库；允许 Apple 系统库与 framework |
| Linux x86_64 | `mradm-<version>-linux-x86_64.AppImage` | Ubuntu 24.04 x86_64 | AppImage/standalone；非核心运行时库打包进 AppImage；拒绝缺失库、构建目录依赖和 `/usr/local` 依赖 |
| Windows x64 | `mradm-<version>-windows-x64.zip` | Windows Server 2025 + MSVC | 包含 `mradm.exe` 与所需 DLL；附带 `dumpbin /dependents` 清单 |
| macOS GUI arm64 | `MacinRender-Gui-<version>-macos-arm64.tar.gz` | macOS 26 runner 构建 | 自包含 `.app`；只允许 Apple 系统库与 framework 作为外部依赖 |
| Windows GUI x64 | `MacinRender-Gui-<version>-windows-x64.zip` | Windows Server 2025 + MSVC | 包含 GUI NativeAOT 可执行文件、`mradm_capi.dll` 与所需 DLL；附带 `dumpbin /dependents` 清单 |

CLI 发行包内容：

- `bin/mradm` 或 `bin/mradm.exe`
- `LICENSE`
- `THIRD_PARTY_NOTICES.md`
- `BUILD_INFO.txt`
- `DEPENDENCIES.txt`

macOS CLI/GUI 使用 `.tar.gz`，Linux CLI 使用 `.AppImage`，Windows CLI/GUI 使用 `.zip`；
每个包旁边都会生成对应 `.sha256`。本轮暂不提供 macOS universal2、notarization 或正式签名。
Linux AppImage 仍以 Ubuntu 24.04 的 glibc baseline 为构建基线，宿主内核、动态加载器、glibc 基线和其他核心平台设施由系统提供。
GUI 发行包包含 macOS `.app` 或 Windows `app/MacinRender.Gui.exe`，同样带有 license、build info、checksum 和依赖清单。

## 渲染后端

| 后端 | CLI 选项 | 输入类型 | 输出 |
|---|---|---|---|
| libear | `--renderer auto` / `ear` | Objects / DirectSpeakers / HOA | 多声道扬声器 |
| SAF VBAP | `--renderer saf` | Objects / DirectSpeakers | 多声道扬声器 |
| HOA 编码 | `--renderer hoa` | Objects / DirectSpeakers | HOA3 16ch（ACN/SN3D） |
| SAF HRTF 双耳 | `--renderer saf-binaural` | Objects / DirectSpeakers | 2ch 双耳 |
| Apple AUSpatialMixer | `--renderer apple` | Objects / DirectSpeakers | 2ch 双耳 / 多声道扬声器（macOS-only） |

`saf-binaural` 默认使用 SAF 内置 Genelec KEMAR HRTF，也可通过 `--sofa <path>` 加载用户 FIR SOFA HRIR 文件。当前 SOFA 限制为 SimpleFreeFieldHRIR / GeneralFIR、2 receivers、48 kHz、不重采样。

推荐的通用外部 HRTF 是 [SADIE II Database](https://www.york.ac.uk/sadie-project/database.html) 的 D1 KU100 SOFA，例如 `D1_48K_24bit_256tap_FIR_SOFA.sofa`（也可从 [SOFA database SADIE 索引](https://sofacoustics.org/data/database/sadie/) 下载）。它是 48 kHz、256-tap、SimpleFreeFieldHRIR，方向采样密度高，并带 low-frequency extension / diffuse-field EQ，适合作为比内置 KEMAR 更均衡的 `--sofa` 默认推荐。SADIE II 数据集由 University of York 以 Apache License 2.0 发布；若分发素材或学术使用，请按其页面说明引用论文 [DOI:10.3390/app8112029](https://doi.org/10.3390/app8112029)。

`apple` 使用 AudioToolbox AUSpatialMixer，支持 binaural、5.1、7.1、5.1.2、5.1.4、7.1.4、9.1.6 和 22.2。该后端是 Apple 平台风味输出，不是 libear / SAF 的 bit-exact 替代；当前不支持 HOA 和 diffuse，支持扬声器输出的 channelLock 以及 extent 点云近似。扬声器输出默认清除 SpatialMixer 的 InterAuralDelay / DistanceAttenuation flags，以匹配 ADM / SAF / Logic 式增益语义；`--apple-speaker-rendering-flags` 可恢复 Apple 原生 flags（旧行为）。`--apple-spatial-preset headphone-default|headphone-movie` 可对 Apple binaural 应用 AUSpatialMixer headphone factory preset（默认关闭；不适用于扬声器布局）。`--object-smoothing-frames` 对 Apple 后端当前无效，动态参数平滑由 SpatialMixer 内部处理。`--start` / `--end` 会走按需窗口渲染，并使用一个 render block 的 pre-roll 更新 SpatialMixer 内部状态。

## 输出格式

### 编码与容器总览

| 编码 | 有损 / 无损 | 当前容器 | 扩展名 | 状态 |
|---|---|---|---|---|
| PCM float32 | 未压缩 | WAV / CAF | `.wav` / `.caf` | 全平台支持 |
| PCM integer | 未压缩 | WAV | `.wav` | 全平台支持；24-bit / 16-bit |
| FLAC | 无损压缩 | FLAC | `.flac` | 全平台支持；当前固定 24-bit，最多 8 声道 |
| Opus | 有损 | Matroska Audio | `.mka` | 全平台支持；Opus VBR |
| Opus | 有损 | IAMF raw OBU | `.iamf` | 需启用官方 AOM iamf-tools bridge 预构建 SDK |
| APAC | 有损 | MPEG-4 Audio | `.m4a` / `.mp4` | macOS only；AudioToolbox |
| APAC | 有损 | CAF | `.caf` | macOS only；需 `--apac-container caf` |

“状态”只表示本项目当前可写出的编码 / 容器组合，不代表目标系统或播放器一定能原生识别布局或直接回放。容器和编码不是一回事：`.caf` 默认仍写 PCM float32；如需 APAC-in-CAF，必须显式传 `--apac-container caf`。

### 未压缩 / 无损输出

WAV 可写 float32 / 24-bit / 16-bit PCM；`--output-bit-depth` 只影响 WAV。WAV 输出与母版输入都支持超过 4GB：float32 WAV 固定写 RF64 容器（小文件也是 RF64——流式写入无法预知总大小，统一用 RF64 的 64-bit `ds64` 承载真实数据大小），整数（24/16-bit）WAV 在 ≤4GB 时写普通 RIFF、超过 4GB 自动升级为 BW64；读取端支持 RIFF / RF64 / BW64 三种容器，并能跨 2^31 帧随机定位（绕开 libbw64 的 32-bit 帧 seek 限制）。CAF 当前固定写 float32 PCM，适合作为 CoreAudio 生态下携带空间布局标签的未压缩容器。FLAC 当前固定写 24-bit lossless，最多 8 声道，并且只开放 `binaural`、`5.1` 和 `7.1` 等无高度布局；`5.1.2` 虽然是 8 声道，但没有可靠的通用高度声道语义，项目会拒绝写出。

带高度或超过 8 声道的无损 / 未压缩交付优先使用 WAV 或 CAF。若需要更强播放器兼容性，需要结合目标播放器实际验证，而不是只看声道数。

### 有损交付输出

Opus MKA 是 Matroska Audio + Opus VBR，全平台可写。标准 5.1 / 7.1 会使用 Opus/Vorbis 声道语义；9.1.6、22.2 等更高阶离散布局使用透明多流编码并记录 metadata，播放器不保证自动识别完整空间布局。

IAMF 输出为 raw OBU stream（`.iamf`）+ Opus，面向 IAMF 测试和交付链路，不是普通播放器通用容器。IAMF 编码依赖官方 AOM iamf-tools bridge；构建时需设置 `-DMR_ADM_ENABLE_IAMF=ON -DMR_ADM_IAMF_AOM_ROOT=/path/to/iamf-sdk`，其中 SDK 提供 `lib/libmr_iamf_aom_bridge.*`。当前 IAMF 只开放到 `7.1.4`；`9.1.6` 需要 expanded/Base-Enhanced IAMF，因播放器兼容性不足暂时禁用。默认 IAMF 仍按最终 `--output-layout` 写单层；需要 scalable channel layers 时可显式传 `--iamf-layers 5.1,5.1.2,5.1.4,7.1.4`，最后一层必须等于输出布局，层级需单调增加。对高度输出包含平面层的组合仅给 warning，允许用户按交付目标自行取舍。未启用时 `.iamf` 输出会直接报 unsupported。

APAC 默认写入 MPEG-4 Audio 容器（`.m4a` / `.mp4`），macOS-only，依赖 AudioToolbox，当前要求 48 kHz。也可输出 APAC-in-CAF：输出路径使用 `.caf` 并传 `--apac-container caf`。空间布局和 HOA 默认使用稳定的总目标码率提示：以 `7.1.4` 的 2048 kbps 为 12 声道基准按声道数缩放，例如 `5.1.4` 约 1707 kbps，`9.1.6` / `hoa3` 约 2731 kbps，`22.2` 约 4096 kbps。该值传给 AudioToolbox 作为编码码率目标 / 提示，实际统计码率可能明显偏离目标值。

### 容器、布局与回放

声道顺序和布局语义由“编码 + 容器 + layout tag / mapping”共同决定。同一编码在不同容器里的布局表达可能不同，同一容器也可承载不同编码。`mradm layouts --format <fmt>` 查询的是当前已实现组合的最终声道顺序。

回放兼容性取决于容器、布局和播放器。macOS 可直接回放本项目写出的 PCM CAF、APAC `.mp4/.m4a` 以及 APAC-in-CAF 空间音频文件；WAV 多声道直放尚未详细测试。实测 PotPlayer 可正常回放离散声道 Opus MKA，虽然会下混到 8ch，但主观听感仍较完整；部分具备系统 Opus decoder（如 `c2.android.opus.decoder` / `OMX.google.opus.decoder`）的 Android 设备可回放最高 `9.1.6` 的 Opus MKA。这些结果不代表播放器能完整保留空间布局语义。

HOA 输出需要单独看待。CAF PCM、APAC MPEG-4 与 APAC CAF 是目前 macOS 上最可靠的 HOA 直接回放路径；WAV HOA3 会写 AmbiX `ambi` chunk，更适合支持 AmbiX 的工具链，macOS 不能直接按 HOA 回放。Opus MKA 会写 ambisonics mapping，但不作为通用直接监听格式；已验证 VLC 4.0 可回放 Opus HOA3，播放时需在音频选项中将 mix node 从 `original: ambisonics` 改为 `binaural`，由 VLC 解码到双耳输出。

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

EAR 与 SAF VBAP 的扬声器布局能力共享同一份项目 registry；`9.1.4` / `9.1.6` 在 libear 后端通过项目侧自定义 `ear::Layout` 实现，不代表 libear 上游内建这些布局。

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
| APAC / M4A | `7.1` | CoreAudio `AudioUnit_7_1` | L R C LFE Ls Rs Rls Rrs |
| APAC / CAF | `9.1.6` | CoreAudio `Atmos_9_1_6` | L R C LFE Ls Rs Rls Rrs Lw Rw Vhl Vhr Ltm Rtm Ltr Rtr |
| APAC / CAF | `22.2` | CoreAudio `CICP_13` | Lw Rw C LFE2 Rls Rrs L R Cs LFE3 Lss Rss Vhl Vhr Vhc Ts Ltr Rtr Ltm Rtm Ctr Cb Lb Rb |
| WAV | `hoa3` | AmbiX `ambi` chunk | ACN/SN3D 16ch |
| CAF / APAC | `hoa3` | CoreAudio `HOA_ACN_SN3D` | ACN/SN3D 16ch |

## 常用 CLI 选项

| 选项 | 说明 | 默认值 |
|---|---|---|
| `--renderer auto\|ear\|saf\|hoa\|saf-binaural\|apple` | 选择渲染后端 | `auto` |
| `--output-layout <layout>` | 输出布局，如 `7.1.4` / `9.1.6` / `22.2` | 后端默认 |
| `--output-bit-depth f32\|i24\|i16` | WAV 输出位深（CAF 固定 float32；FLAC 固定 24-bit / 最多 8 声道） | `f32` |
| `--loudness-target <LUFS>` | 响度归一化目标；HOA 通过 7.1.4 AllRAD 参考解码测量，LFE 排除于 LUFS | 关闭 |
| `--peak-limit-dbtp <dBTP>` | True Peak 限制目标 | `-1.0` |
| `--peak-normalize-to-limit` | 在响度增益后，如 True Peak 低于 `--peak-limit-dbtp`，自动补全局增益到上限；需要开启 True Peak 限制 | 关闭 |
| `--final-gain-db <dB>` | 在响度 / 峰值自动增益之后追加不受限制的最终增益；绕过 True Peak 限制，可能超过 0 dBFS | `0` |
| `--no-peak-limit` | 关闭 True Peak 限制 | - |
| `--start <sec>` | 从渲染时间线该秒数开始裁剪输出；响度 / True Peak 只按保留片段计量 | `0` |
| `--end <sec>` | 裁剪到渲染时间线该绝对秒数；必须大于 `--start`，未设置则渲染到结尾 | 关闭 |
| `--interp-ms <ms>` | ADM 块无 jumpPosition 时的增益插值斜坡 | `5` |
| `--object-smoothing-frames <frames>` | Objects 动态元数据去拉链平滑窗口；`0` 为逐样本跟随 ADM 块；极端动态素材可显式调大；Apple 后端当前忽略此参数 | `0` |
| `--apple-spatial-preset off\|headphone-default\|headphone-movie` | Apple binaural 专用 AUSpatialMixer factory preset；preset 会先应用，再重新写入 ADM 驱动的空间参数；扬声器布局不支持 | `off` |
| `--apple-speaker-rendering-flags` | Apple 扬声器输出开启 InterAuralDelay + DistanceAttenuation flags，恢复旧行为；默认关闭以匹配 ADM / SAF 增益语义 | 关闭 |
| `--listener-yaw <deg>` / `--listener-pitch <deg>` / `--listener-roll <deg>` | Apple binaural 专用听者头部朝向；yaw 为 `[-180, 180]` 且 `+` 向左，pitch 为 `[-90, 90]` 且 `+` 向上，roll 为 `[-180, 180]` | `0` |
| `--opus-bitrate-per-ch <kbps>` | Opus VBR 目标比特率 / 声道 | 自动 |
| `--apac-bitrate <kbps>` | APAC 总目标比特率提示；未设置时空间布局 / HOA 按 7.1.4=2048 kbps 基准缩放 | 见输出格式说明 |
| `--apac-container mpeg4\|caf` | APAC 容器；`caf` 要求输出路径为 `.caf`，普通 `.caf` 默认仍为 PCM | `mpeg4` |
| `--sofa <path>` | `saf-binaural` 用户 SOFA HRIR 文件 | 内置 KEMAR |
| `--semantic-policy <path>` | 渲染时应用 ADM 语义控制 JSON（覆盖 Objects / DirectSpeakers / HOA 的 gain·mute·position 及 diffuse / extent / divergence / channelLock / 插值等） | 关闭 |
| `--write-semantic-report <path>` | 写出 policy 应用后的 effective semantic JSON，便于确认对象 / DS / HOA 规则命中与 original→effective 变化 | 关闭 |

响度相关后处理顺序为：`--loudness-target` 先决定目标响度增益，`--peak-normalize-to-limit` 可选补峰到 True Peak 上限，`--peak-limit-dbtp` 裁剪自动阶段的全局增益；`--final-gain-db` 在这些自动阶段之后追加，故会绕过 True Peak 限制。

Semantic policy 不修改原始 AXML，仅影响本次渲染。`inspect --write-semantic-policy-template` 会按场景生成一份可编辑的中性模板（原样应用不改变场景），编辑后用 `--semantic-policy` 应用：

```bash
./build/release/mradm inspect in.wav --write-semantic-policy-template policy.json
./build/release/mradm render -i in.wav -o out.flac --renderer saf-binaural --semantic-policy policy.json
```

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

## 构建选项

推荐使用 CMake preset：

```bash
cmake --preset debug
cmake --build --preset debug
ctest --test-dir build/debug --output-on-failure
```

默认会通过 `FetchContent` 拉取依赖。若系统已提供所有依赖，可关闭自动拉取：

```bash
cmake -S . -B build -DMR_ADM_CORE_FETCH_DEPS=OFF
```

FLAC 与 Opus 支持三档 provider：

| 选项 | 说明 |
|---|---|
| `MR_ADM_FLAC_PROVIDER=AUTO` / `MR_ADM_OPUS_PROVIDER=AUTO` | 默认；Release 使用 vendored static，Debug 优先系统库 |
| `VENDORED` | 强制 FetchContent 静态链接，适合发行 |
| `SYSTEM` | 强制使用系统库，适合包管理器和发行版打包 |

SOFA 支持默认开启，可显式控制：

```bash
cmake -S . -B build -DMR_ADM_ENABLE_SOFA=ON
```

## 质量检查

```bash
./scripts/quality/check-changed.sh
./scripts/quality/format.sh --check
./scripts/quality/clang-tidy.sh build/debug
./scripts/quality/cppcheck.sh build/debug
```

## 文档

- [ADM 特性覆盖审计](docs/architecture/ADM_FEATURE_COVERAGE.md)
- [Apple AUSpatialMixer 后端实现说明](docs/architecture/ADM_APPLE_BACKEND.md)
- [C++ ADM 渲染平台化重构规划](docs/architecture/CPP_ADM_PLATFORM_REWRITE.md)
- [架构决策记录](docs/adr/)
- [质量工具配置](docs/guides/QUALITY.md)
- [第三方许可证与发行边界](docs/THIRD_PARTY_LICENSES.md)

## 许可证

本项目源码采用 **MIT License**，以仓库根目录 [LICENSE](LICENSE) 为准。

当前默认构建依赖与 MIT 源码许可证兼容；二进制发行包需要附带第三方依赖的 notice/license 文本。默认构建只启用本项目渲染路径所需的 SAF 组件，额外 SAF 模块和替代 DSP 依赖需在发行前单独确认许可证。
