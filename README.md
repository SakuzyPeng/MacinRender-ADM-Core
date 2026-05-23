# MacinRender ADM Core

跨平台 ADM（Audio Definition Model，ITU-R BS.2076）空间音频渲染引擎，C++20 实现，提供 CLI 工具和 C ABI 库。

## 功能

### 渲染后端

| 后端 | 选项 | 输入类型 | 输出 |
|---|---|---|---|
| libear | `--renderer auto` / `ear` | Objects · DirectSpeakers · HOA | 多声道扬声器 |
| SAF VBAP | `--renderer saf` | Objects · DirectSpeakers | 多声道扬声器 |
| HOA 编码 | `--renderer hoa` | Objects | HOA3 16ch（ACN/SN3D） |
| HRTF 双耳 | `--renderer binaural` | Objects · DirectSpeakers | 2ch 双耳 |

### 输出布局

| 常用名称 / CLI 值 | 声道数 | 支持 EAR | 支持 VBAP |
|---|---|---|---|
| `5.1` | 6 | ✅ | ✅ |
| `5.1.2` | 8 | ✅ | — |
| `7.1` | 8 | ✅ | ✅ |
| `5.1.4` | 10 | ✅ | ✅ |
| `9.1.4` | 14 | ✅ | — |
| `7.1.4` | 12 | ✅ | ✅ |
| `9.1.6` (Dolby Atmos) | 16 | — | ✅ |
| `22.2` | 24 | ✅ | ✅ |
| `hoa3` | 16 | — | — |

### 最终声道顺序

声道语义必须和最终输出格式一起看；同一个 `--output-layout` 在不同容器里可能有不同的实际顺序。完整表可用 `adm layouts --format <wav|caf|flac|apac>` 查询。

| 格式 | Layout | 最终容器 / 映射 | 最终声道顺序 |
|---|---|---|---|
| WAV / FLAC | `7.1` | WAVE_7_1 / `wav71` | L R C LFE Rls Rrs Ls Rs |
| APAC / M4A | `7.1` | CoreAudio `AudioUnit_7_1` | L R C LFE Ls Rs Rls Rrs |
| CAF | `7.1` | CoreAudio `WAVE_7_1` | L R C LFE Rls Rrs Ls Rs |
| APAC / CAF | `5.1.4` | CoreAudio `Atmos_5_1_4` | L R C LFE Ls Rs Vhl Vhr Ltr Rtr |
| APAC / CAF | `7.1.4` | CoreAudio `Atmos_7_1_4` | L R C LFE Ls Rs Rls Rrs Vhl Vhr Ltr Rtr |
| APAC / CAF | `9.1.6` | CoreAudio `Atmos_9_1_6` | L R C LFE Ls Rs Rls Rrs Lw Rw Vhl Vhr Ltm Rtm Ltr Rtr |
| APAC / CAF | `22.2` | CoreAudio `CICP_13` | Lw Rw C LFE2 Rls Rrs L R Cs LFE3 Lss Rss Vhl Vhr Vhc Ts Ltr Rtr Ltm Rtm Ctr Cb Lb Rb |
| APAC / M4A | `binaural` | 请求 CoreAudio `Binaural`，metadata 写 `layout=binaural` | L R（`afinfo` 目前仍可能显示 Stereo） |

APAC 7.1 在编码前会把内部 `wav71` 顺序重排为 CoreAudio `AudioUnit_7_1` 顺序。`22.2` 的两个 LFE 槽位在 BS.2051/libear 语境中常写作 LFE1/LFE2，而 CoreAudio `CICP_13` 读出为 LFE2/LFE3；这是命名语境差异，不代表声道位置不同。

### 输出格式

| 格式 | 扩展名 | 位深 | 编码支持 |
|---|---|---|---|
| WAV | `.wav` | float32 / 24-bit / 16-bit | 全平台 |
| CAF | `.caf` | float32 | 全平台 |
| FLAC | `.flac` | 24-bit integer | 全平台 |
| Opus MKA | `.mka` | VBR 有损 | 全平台 |
| APAC | `.m4a` | VBR 有损 | macOS only |

“编码支持”只表示本项目可在对应平台写出该格式，不代表目标系统或播放器一定能原生识别布局或直接回放。

## 构建

```bash
cmake -S . -B build/release -DCMAKE_BUILD_TYPE=Release
cmake --build build/release
```

首次配置时 CMake 自动通过 `FetchContent` 拉取所有依赖，无需手动安装。可关闭：

```bash
cmake -S . -B build -DMR_ADM_CORE_FETCH_DEPS=OFF
```

**预设（推荐）：**

```bash
cmake --preset debug    # 含 debug symbols
cmake --build --preset debug
```

需要在构建期启用 clang-tidy / Cppcheck 时使用 `quality` preset。

**测试：**

```bash
ctest --test-dir build/debug --output-on-failure
```

**质量检查：**

```bash
./scripts/quality/check-changed.sh          # 仅检查 git 变更文件
./scripts/quality/format.sh --check         # clang-format
./scripts/quality/clang-tidy.sh build/debug
./scripts/quality/cppcheck.sh build/debug
```

### FLAC 提供方

| CMake 选项 | 说明 |
|---|---|
| `MR_ADM_FLAC_PROVIDER=AUTO`（默认）| Release 用 vendored 静态链接；Debug 优先系统 libFLAC |
| `MR_ADM_FLAC_PROVIDER=VENDORED` | 强制 FetchContent 静态链接，适合分发 |
| `MR_ADM_FLAC_PROVIDER=SYSTEM` | 强制使用系统 libFLAC（Homebrew / vcpkg / Linux 发行版） |

### Opus 提供方

与 FLAC 相同的三档策略，对应选项为 `MR_ADM_OPUS_PROVIDER`。

### SOFA 支持

```bash
cmake -S . -B build -DMR_ADM_ENABLE_SOFA=ON
```

启用后可通过 `--sofa` 加载用户 FIR SOFA HRIR 文件替换内置 KEMAR HRTF。当前限制：SimpleFreeFieldHRIR / GeneralFIR 格式，2 receivers，48 kHz，不支持重采样。

## 使用

```bash
# 自动选择后端，2ch 输出默认走 Binaural
./build/release/adm render -i input.wav -o output.wav

# EAR 后端，7.1.4 扬声器布局，FLAC 输出
./build/release/adm render -i input.wav -o output.flac --renderer ear --output-layout 7.1.4

# SAF VBAP，9.1.6，响度归一化到 -23 LUFS
./build/release/adm render -i input.wav -o output.wav \
    --renderer saf --output-layout 9.1.6 --loudness-target -23

# HRTF 双耳渲染，内置 KEMAR HRTF
./build/release/adm render -i input.wav -o output.wav --renderer binaural

# HRTF 双耳渲染，用户 SOFA 文件
./build/release/adm render -i input.wav -o output.wav --renderer binaural --sofa my.sofa

# Opus MKA，7.1.4，64 kbps/ch
./build/release/adm render -i input.wav -o output.mka \
    --renderer ear --output-layout 7.1.4 --opus-bitrate-per-ch 64

# APAC（macOS only），支持 Binaural / 7.1 / 5.1.4 / 7.1.4 / 9.1.6 / 22.2
./build/release/adm render -i input.wav -o output.m4a \
    --renderer binaural

# 查看 ADM 场景元数据
./build/release/adm inspect input.wav

# 列出所有可用后端和布局
./build/release/adm backends

# 按最终输出格式查看实际声道顺序
./build/release/adm layouts --format apac --layout 7.1
./build/release/adm layouts --format wav
```

### 常用渲染选项

| 选项 | 说明 | 默认值 |
|---|---|---|
| `--output-bit-depth f32\|i24\|i16` | WAV 输出位深（CAF 固定 float32；FLAC 固定 24-bit） | `f32` |
| `--loudness-target <LUFS>` | 响度归一化目标（启用测量） | 关闭 |
| `--peak-limit-dbtp <dBTP>` | True Peak 限制目标 | `-1.0` |
| `--no-peak-limit` | 关闭 True Peak 限制 | — |
| `--interp-ms <ms>` | ADM 块无 jumpPosition 时的增益插值斜坡 | `5` |
| `--opus-bitrate-per-ch <kbps>` | Opus VBR 目标比特率 / 声道 | 自动（64 kbps/ch） |
| `--apac-bitrate <kbps>` | APAC 总目标比特率提示 | 编码器默认 |
| `--apac-drc-music\|--apac-drc-none` | APAC DRC 配置 | `music` |
| `--sofa <path>` | binaural 用户 SOFA HRIR 文件 | 内置 KEMAR |

## 架构

```
include/adm/        公共 C++ API 和 C ABI 头文件
src/adm_core/       核心领域模型、scene 结构和公共基础
src/adm_io/         ADM/BW64 scene importer
src/adm_audio/      音频 I/O（WAV/CAF/FLAC/Opus/APAC）
src/adm_engine/     渲染编排（RenderService）
src/adm_render_ear/         libear 后端
src/adm_render_vbap/        SAF VBAP 后端
src/adm_render_hoa/         HOA 编码后端
src/adm_render_binaural/    HRTF 双耳后端
src/adm_c_api/      C ABI 包装层
src/adm_cli/        CLI 入口（CLI11 + fmt）
tests/unit/         单元与 smoke 测试
cmake/              CMake 辅助模块
docs/adr/           架构决策记录
docs/architecture/  特性覆盖审计和设计规划
```

## binaural 后端

使用 SAF 内置 Genelec KEMAR HRTF（836 方向，256 tap，48 kHz）。渲染流程：Objects 块逐帧解析方位角/仰角 → VBAP 权重插值 HRTF → OLA FFT 卷积 → 2ch 累加输出。DirectSpeakers 通过 BS.2051 扬声器标签查表定位。

输出文件使用 CoreAudio `Binaural` layout tag（PCM CAF），与普通扬声器立体声（`MPEG_2_0`）区分。APAC `.m4a` 编码器不可靠地保留该 tag，binaural 语义通过 `©cmt` metadata 保留。

## 许可证

本项目源码采用 **MIT License**，以仓库根目录 [LICENSE](LICENSE) 为准。

当前默认构建依赖与 MIT 源码许可证兼容；二进制发行包需要附带第三方依赖的 notice/license 文本。默认发行构建不得启用 SAF 的 GPLv2 可选模块（如 tracker / HADES）或 FFTW 等需要单独审计的依赖。

第三方许可证和生产发行边界见 [docs/THIRD_PARTY_LICENSES.md](docs/THIRD_PARTY_LICENSES.md)。

---

*更多信息：[特性覆盖审计](docs/architecture/ADM_FEATURE_COVERAGE.md) · [架构决策记录](docs/adr/) · [质量工具配置](docs/guides/QUALITY.md)*
