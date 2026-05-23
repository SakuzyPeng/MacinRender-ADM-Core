# MacinRender ADM Core

MacinRender ADM Core 是一个跨平台 ADM（Audio Definition Model，ITU-R BS.2076）空间音频渲染核心，使用 C++20 实现，提供命令行工具和 C ABI 库。

它面向 ADM BWF / BW64 输入，可渲染到多声道扬声器、HOA 编码、HRTF 双耳，以及 WAV / CAF / FLAC / Opus MKA / APAC 等交付格式。

## 功能概览

- ADM scene import：基于 libbw64 / libadm 读取 BW64 ADM 元数据，并转换为项目自有领域模型。
- 多后端渲染：libear、SAF VBAP、HOA encoder、HRTF binaural。
- Objects / DirectSpeakers：支持对象和直达扬声器内容，含时间块、增益、插值、扩散、channelLock 和 objectDivergence 等语义。
- 输出后处理：响度归一化、True Peak 限制、位深转换、CAF/FLAC/Opus/APAC metadata。
- 平台边界：核心功能面向 macOS / Linux / Windows；APAC 编码为 macOS-only。

## 快速开始

```bash
cmake -S . -B build/release -DCMAKE_BUILD_TYPE=Release
cmake --build build/release
```

渲染一个 ADM BWF 文件：

```bash
./build/release/adm render -i input.wav -o output.wav
```

双耳渲染：

```bash
./build/release/adm render -i input.wav -o binaural.wav --renderer binaural
```

渲染到 7.1.4 FLAC：

```bash
./build/release/adm render -i input.wav -o output.flac --renderer ear --output-layout 7.1.4
```

查看 ADM 场景：

```bash
./build/release/adm inspect input.wav
```

查看可用后端和布局：

```bash
./build/release/adm backends
./build/release/adm layouts --format wav
```

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
| FLAC | `.flac` | 24-bit integer FLAC | 全平台 |
| Opus MKA | `.mka` | Opus VBR | 全平台 |
| APAC | `.m4a` | APAC VBR | macOS only |

“编码支持”只表示本项目可在对应平台写出该格式，不代表目标系统或播放器一定能原生识别布局或直接回放。

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
./build/release/adm layouts --format wav
./build/release/adm layouts --format caf
./build/release/adm layouts --format apac
```

常见差异示例：

| 格式 | Layout | 最终容器 / 映射 | 最终声道顺序 |
|---|---|---|---|
| WAV / FLAC | `7.1` | WAVE_7_1 / `wav71` | L R C LFE Rls Rrs Ls Rs |
| APAC / M4A | `7.1` | CoreAudio `AudioUnit_7_1` | L R C LFE Ls Rs Rls Rrs |
| APAC / CAF | `9.1.6` | CoreAudio `Atmos_9_1_6` | L R C LFE Ls Rs Rls Rrs Lw Rw Vhl Vhr Ltm Rtm Ltr Rtr |
| APAC / CAF | `22.2` | CoreAudio `CICP_13` | Lw Rw C LFE2 Rls Rrs L R Cs LFE3 Lss Rss Vhl Vhr Vhc Ts Ltr Rtr Ltm Rtm Ctr Cb Lb Rb |

## 常用 CLI 选项

| 选项 | 说明 | 默认值 |
|---|---|---|
| `--renderer auto\|ear\|saf\|hoa\|binaural` | 选择渲染后端 | `auto` |
| `--output-layout <layout>` | 输出布局，如 `7.1.4` / `9.1.6` / `22.2` | 后端默认 |
| `--output-bit-depth f32\|i24\|i16` | WAV 输出位深（CAF 固定 float32；FLAC 固定 24-bit） | `f32` |
| `--loudness-target <LUFS>` | 响度归一化目标 | 关闭 |
| `--peak-limit-dbtp <dBTP>` | True Peak 限制目标 | `-1.0` |
| `--no-peak-limit` | 关闭 True Peak 限制 | - |
| `--interp-ms <ms>` | ADM 块无 jumpPosition 时的增益插值斜坡 | `5` |
| `--opus-bitrate-per-ch <kbps>` | Opus VBR 目标比特率 / 声道 | 自动 |
| `--apac-bitrate <kbps>` | APAC 总目标比特率提示 | 编码器默认 |
| `--sofa <path>` | binaural 用户 SOFA HRIR 文件 | 内置 KEMAR |

更多选项可查看：

```bash
./build/release/adm render --help
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

当前默认构建依赖与 MIT 源码许可证兼容；二进制发行包需要附带第三方依赖的 notice/license 文本。默认发行构建不得启用 SAF 的 GPLv2 可选模块（如 tracker / HADES）或 FFTW 等需要单独审计的依赖。
