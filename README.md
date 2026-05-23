# MacinRender ADM Core

MacinRender ADM Core 是一个跨平台 ADM（Audio Definition Model，ITU-R BS.2076）空间音频渲染核心，使用 C++20 实现，提供命令行工具和 C ABI 库。

它面向 ADM BWF / BW64 输入，可渲染到多声道扬声器、HOA 编码、HRTF 双耳，以及 WAV / CAF / FLAC / Opus MKA / APAC 等交付格式。

## 功能概览

- ADM scene import：基于 libbw64 / libadm 读取 BW64 ADM 元数据，并转换为项目自有领域模型。
- 多后端渲染：libear、SAF VBAP、HOA encoder、HRTF binaural。
- Objects / DirectSpeakers：支持对象和直达扬声器内容，含时间块、增益、插值、扩散、channelLock 和 objectDivergence 等语义。
- 输出后处理：响度归一化、True Peak 限制、位深转换、CAF/FLAC/Opus/APAC metadata。HOA 输出的响度测量仅作为系数域诊断，不作为播放端响度归一化依据。
- 平台边界：核心功能面向 macOS / Linux / Windows；APAC 编码为 macOS-only。

## 快速开始

```bash
cmake -S . -B build/release -DCMAKE_BUILD_TYPE=Release
cmake --build build/release
```

渲染一个 ADM BWF 文件：

```bash
./build/release/mradm render -i input.wav -o output.wav
```

双耳渲染：

```bash
./build/release/mradm render -i input.wav -o binaural.wav --renderer binaural
```

渲染到 7.1 FLAC：

```bash
./build/release/mradm render -i input.wav -o output.flac --renderer ear --output-layout 7.1
```

查看 ADM 场景：

```bash
./build/release/mradm inspect input.wav
```

查看可用后端和布局：

```bash
./build/release/mradm backends
./build/release/mradm layouts --format wav
./build/release/mradm layouts --format flac --renderer saf
```

## 发行包

GitHub Actions 的 release workflow 会在 tag `v*` 或手动触发时生成首版可审计发行包：

| 平台 | artifact | 支持基线 | 自包含边界 |
|---|---|---|---|
| macOS arm64 | `mradm-<version>-macos-arm64.tar.gz` | macOS 15 runner 构建 | 不依赖 Homebrew / `/usr/local` 动态库；允许 Apple 系统库与 framework |
| Linux x86_64 | `mradm-<version>-linux-x86_64.tar.gz` | Ubuntu 24.04 x86_64 | 附带 `ldd` 清单；拒绝缺失库、构建目录依赖和 `/usr/local` 依赖；不承诺任意 Linux 发行版 portable |

发行包内容：

- `bin/mradm`
- `LICENSE`
- `THIRD_PARTY_NOTICES.md`
- `BUILD_INFO.txt`
- `DEPENDENCIES.txt`

每个 `.tar.gz` 旁边会生成对应 `.sha256`。本轮暂不提供 Windows 产物、macOS universal2、codesign 或 notarization。
Linux 包的目标是 Ubuntu 24.04 及相近 glibc 环境；如果需要更强的跨发行版分发，后续会单独评估 AppImage 或随包携带 `.so` 与 rpath 的方案。

## 渲染后端

| 后端 | CLI 选项 | 输入类型 | 输出 |
|---|---|---|---|
| libear | `--renderer auto` / `ear` | Objects / DirectSpeakers / HOA | 多声道扬声器 |
| SAF VBAP | `--renderer saf` | Objects / DirectSpeakers | 多声道扬声器 |
| HOA 编码 | `--renderer hoa` | Objects | HOA3 16ch（ACN/SN3D） |
| HRTF 双耳 | `--renderer binaural` | Objects / DirectSpeakers | 2ch 双耳 |

`binaural` 默认使用 SAF 内置 Genelec KEMAR HRTF，也可通过 `--sofa <path>` 加载用户 FIR SOFA HRIR 文件。当前 SOFA 限制为 SimpleFreeFieldHRIR / GeneralFIR、2 receivers、48 kHz、不重采样。

## 输出格式

| 格式 | 扩展名 | 位深 / 编码 | 编码支持 |
|---|---|---|---|
| WAV | `.wav` | float32 / 24-bit / 16-bit PCM | 全平台 |
| CAF | `.caf` | float32 PCM | 全平台 |
| FLAC | `.flac` | 24-bit integer FLAC，1-8 声道 | 全平台 |
| Opus MKA | `.mka` | Opus VBR | 全平台 |
| APAC | `.m4a` / `.mp4` | APAC VBR | macOS only |

“编码支持”只表示本项目可在对应平台写出该格式，不代表目标系统或播放器一定能原生识别布局或直接回放。

FLAC 当前最多支持 8 声道，因此不能用于 `5.1.4`、`7.1.4`、`9.1.6`、`22.2` 等超过 8 声道的布局；这些布局请使用 WAV、CAF、APAC 或 Opus MKA。

APAC 空间布局和 HOA 默认使用稳定的总目标码率提示：以 `7.1.4` 的 2048 kbps 为 12 声道基准按声道数缩放，例如 `5.1.4` 约 1707 kbps，`9.1.6` / `hoa3` 约 2731 kbps，`22.2` 约 4096 kbps。APAC 是 VBR 编码，实际统计码率可能明显偏离目标值。

## 输出布局

| 常用名称 / CLI 值 | 声道数 | EAR | SAF VBAP |
|---|---:|---|---|
| `5.1` | 6 | yes | yes |
| `5.1.2` | 8 | yes | - |
| `7.1` | 8 | yes | yes |
| `5.1.4` | 10 | yes | yes |
| `9.1.4` | 14 | yes | - |
| `7.1.4` | 12 | yes | yes |
| `9.1.6` | 16 | - | yes |
| `22.2` | 24 | yes | yes |
| `hoa3` | 16 | - | - |

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
| `--renderer auto\|ear\|saf\|hoa\|binaural` | 选择渲染后端 | `auto` |
| `--output-layout <layout>` | 输出布局，如 `7.1.4` / `9.1.6` / `22.2` | 后端默认 |
| `--output-bit-depth f32\|i24\|i16` | WAV 输出位深（CAF 固定 float32；FLAC 固定 24-bit / 最多 8 声道） | `f32` |
| `--loudness-target <LUFS>` | 响度归一化目标；HOA 输出会跳过，需先解码到播放布局后测量 | 关闭 |
| `--peak-limit-dbtp <dBTP>` | True Peak 限制目标 | `-1.0` |
| `--no-peak-limit` | 关闭 True Peak 限制 | - |
| `--interp-ms <ms>` | ADM 块无 jumpPosition 时的增益插值斜坡 | `5` |
| `--opus-bitrate-per-ch <kbps>` | Opus VBR 目标比特率 / 声道 | 自动 |
| `--apac-bitrate <kbps>` | APAC 总目标比特率提示；未设置时空间布局 / HOA 按 7.1.4=2048 kbps 基准缩放 | 见输出格式说明 |
| `--sofa <path>` | binaural 用户 SOFA HRIR 文件 | 内置 KEMAR |

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
- [C++ ADM 渲染平台化重构规划](docs/architecture/CPP_ADM_PLATFORM_REWRITE.md)
- [架构决策记录](docs/adr/)
- [质量工具配置](docs/guides/QUALITY.md)
- [第三方许可证与发行边界](docs/THIRD_PARTY_LICENSES.md)

## 许可证

本项目源码采用 **MIT License**，以仓库根目录 [LICENSE](LICENSE) 为准。

当前默认构建依赖与 MIT 源码许可证兼容；二进制发行包需要附带第三方依赖的 notice/license 文本。默认构建只启用本项目渲染路径所需的 SAF 组件，额外 SAF 模块和替代 DSP 依赖需在发行前单独确认许可证。
