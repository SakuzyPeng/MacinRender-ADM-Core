# MacinRender ADM Core

English | [中文](README.md)

MacinRender ADM Core is a cross-platform ADM (Audio Definition Model, ITU-R BS.2076) spatial-audio rendering core written in C++20. It provides the `mradm` command-line tool and a stable C ABI library.

It reads ADM BWF / BW64 input and renders to loudspeaker layouts, HOA encoding, HRTF binaural output, and delivery formats including WAV, CAF, FLAC, Opus MKA, IAMF, and APAC.

## Feature Overview

- ADM scene import: reads BW64 ADM metadata through libbw64 / libadm and converts it into the project's own domain model.
- Render backends: libear, SAF VBAP, HOA encoder, HRTF binaural, and Apple AUSpatialMixer (macOS-only).
- Objects / DirectSpeakers: supports timed blocks, gain, interpolation, diffuse, channelLock, objectDivergence, and related ADM semantics.
- Post-processing: loudness normalization, True Peak limiting, bit-depth conversion, and CAF / FLAC / Opus / APAC metadata. HOA output is measured through a 7.1.4 AllRAD reference decode; LFE is excluded from LUFS but included in True Peak.
- Platform boundary: core functionality targets macOS, Linux, and Windows; APAC encoding and the Apple AUSpatialMixer backend are macOS-only.

## Quick Start

```bash
cmake -S . -B build/release -DCMAKE_BUILD_TYPE=Release
cmake --build build/release
```

Inspect an ADM scene and query available backends / layouts:

```bash
./build/release/mradm inspect input.wav
./build/release/mradm backends
./build/release/mradm layouts --format wav
./build/release/mradm layouts --format flac --renderer saf
```

Render examples:

```bash
./build/release/mradm render -i input.wav -o out_binaural.wav --renderer binaural
./build/release/mradm render -i input.wav -o out_714.flac --renderer ear --output-layout 7.1.4
./build/release/mradm render -i input.wav -o out_222.wav --renderer apple --output-layout 22.2
./build/release/mradm render -i input.wav -o out_trim.wav --start 12.5 --end 45.0
```

## Release Packages

The GitHub Actions release workflow produces auditable packages for tags matching `v*` and for manual release runs.

| Platform | Artifact | Baseline | Self-contained boundary |
|---|---|---|---|
| macOS arm64 | `mradm-<version>-macos-arm64.tar.gz` | Built on macOS 26 runner | No Homebrew / `/usr/local` dynamic-library dependency; Apple system libraries and frameworks are allowed |
| Linux x86_64 | `mradm-<version>-linux-x86_64.tar.gz` | Ubuntu 24.04 x86_64 | Includes an `ldd` manifest; rejects missing libraries, build-directory paths, and `/usr/local` dependencies |
| Windows x64 | `mradm-<version>-windows-x64.zip` | Windows Server 2025 + MSVC | Includes `mradm.exe` and required DLLs, plus a `dumpbin /dependents` manifest |

Each package contains:

- `bin/mradm`
- `LICENSE`
- `THIRD_PARTY_NOTICES.md`
- `BUILD_INFO.txt`
- `DEPENDENCIES.txt`

macOS / Linux packages use `.tar.gz`; Windows packages use `.zip`. A `.sha256` file is generated next to each package. macOS universal2, codesign, and notarization are not provided yet.

## Render Backends

| Backend | CLI option | Input types | Output |
|---|---|---|---|
| libear | `--renderer auto` / `ear` | Objects / DirectSpeakers / HOA | Multichannel loudspeakers |
| SAF VBAP | `--renderer saf` | Objects / DirectSpeakers | Multichannel loudspeakers |
| HOA encoder | `--renderer hoa` | Objects / DirectSpeakers | HOA3 16ch (ACN/SN3D) |
| HRTF binaural | `--renderer binaural` | Objects / DirectSpeakers | 2ch binaural |
| Apple AUSpatialMixer | `--renderer apple` | Objects / DirectSpeakers | 2ch binaural / multichannel loudspeakers (macOS-only) |

The `binaural` backend uses SAF's built-in Genelec KEMAR HRTF by default. A user FIR SOFA HRIR file can be loaded with `--sofa <path>`. Current SOFA support is limited to SimpleFreeFieldHRIR / GeneralFIR, 2 receivers, 48 kHz, with no resampling.

The `apple` backend uses AudioToolbox AUSpatialMixer. It supports binaural, 5.1, 7.1, 5.1.2, 5.1.4, 7.1.4, 9.1.6, and 22.2. It is an Apple platform-flavored renderer, not a bit-exact replacement for libear / SAF; HOA and diffuse are not supported, while speaker-output channelLock and extent cloud approximation are supported. `--object-smoothing-frames` currently has no effect on the Apple backend because dynamic parameter smoothing is handled inside SpatialMixer. `--start` / `--end` use on-demand window rendering with one render block of pre-roll to update SpatialMixer state.

## Output Formats

### Codecs and Containers

| Codec | Lossy / Lossless | Container | Extension | Status |
|---|---|---|---|---|
| PCM float32 | Uncompressed | WAV / CAF | `.wav` / `.caf` | Cross-platform |
| PCM integer | Uncompressed | WAV | `.wav` | Cross-platform; 24-bit / 16-bit |
| FLAC | Lossless | FLAC | `.flac` | Cross-platform; fixed 24-bit, up to 8 channels |
| Opus | Lossy | Matroska Audio | `.mka` | Cross-platform; Opus VBR |
| Opus | Lossy | IAMF raw OBU | `.iamf` | Requires the official AOM iamf-tools bridge SDK |
| APAC | Lossy | MPEG-4 Audio | `.m4a` / `.mp4` | macOS-only; AudioToolbox |
| APAC | Lossy | CAF | `.caf` | macOS-only; requires `--apac-container caf` |

The status column only describes what this project can currently write. It does not guarantee that a target system or player can preserve the spatial layout semantics during playback. Plain `.caf` output still writes float32 PCM by default; APAC-in-CAF requires `--apac-container caf`.

### Uncompressed / Lossless Output

WAV can be written as float32, 24-bit, or 16-bit PCM. `--output-bit-depth` affects WAV only. CAF currently writes float32 PCM and is useful in the CoreAudio ecosystem because it carries spatial layout tags. FLAC currently writes fixed 24-bit lossless audio, supports up to 8 channels, and is exposed only for `binaural`, `5.1`, and `7.1`-style non-height layouts.

For lossless or uncompressed output with height channels or more than 8 channels, prefer WAV or CAF. Playback compatibility must still be validated against the target player.

### Lossy Delivery Output

Opus MKA is Matroska Audio + Opus VBR and can be written on all supported platforms. Standard 5.1 / 7.1 layouts use Opus/Vorbis channel semantics; higher discrete layouts such as 9.1.6 and 22.2 use transparent multistream encoding with metadata, and players may not automatically infer the full spatial layout.

IAMF output is a raw OBU stream (`.iamf`) with Opus, intended for IAMF testing and delivery chains rather than general player playback. It requires the official AOM iamf-tools bridge at configure time:

```bash
cmake -S . -B build/release \
  -DMR_ADM_ENABLE_IAMF=ON \
  -DMR_ADM_IAMF_AOM_ROOT=/path/to/iamf-sdk
```

APAC output writes MPEG-4 Audio (`.m4a` / `.mp4`) on macOS via AudioToolbox by default and currently requires 48 kHz. APAC-in-CAF is also available by using a `.caf` output path with `--apac-container caf`. Spatial layouts and HOA use stable total-bitrate hints by default, scaled from a 7.1.4 baseline of 2048 kbps. AudioToolbox treats this as an encoder target / hint, so measured bitrate can differ substantially.

### Containers, Layouts, and Playback

Channel order and spatial layout semantics are determined by the combination of codec, container, and layout tag / mapping. Use `mradm layouts --format <fmt>` to query the implemented channel order for a given output format.

HOA output needs special care. CAF PCM, APAC MPEG-4, and APAC CAF are currently the most reliable direct HOA playback paths on macOS. WAV HOA3 writes an AmbiX `ambi` chunk and is better suited for AmbiX-aware tools. Opus MKA writes an ambisonics mapping but is not a general-purpose direct-monitoring format.

## Output Layouts

| Common name / CLI value | Channels | EAR | SAF VBAP | Apple |
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

EAR and SAF VBAP share the same project layout registry. `9.1.4` / `9.1.6` are implemented for the libear backend through project-side custom `ear::Layout` definitions.

Query full channel-order tables with:

```bash
./build/release/mradm layouts --format wav
./build/release/mradm layouts --format caf
./build/release/mradm layouts --format apac
./build/release/mradm layouts --format flac --renderer ear
```

## Common CLI Options

| Option | Description | Default |
|---|---|---|
| `--renderer auto\|ear\|saf\|hoa\|binaural\|apple` | Select the render backend | `auto` |
| `--output-layout <layout>` | Output layout, for example `7.1.4` / `9.1.6` / `22.2` | Backend default |
| `--output-bit-depth f32\|i24\|i16` | WAV output bit depth; CAF is fixed float32, FLAC is fixed 24-bit / up to 8 channels | `f32` |
| `--loudness-target <LUFS>` | Normalize integrated loudness; HOA is measured through a 7.1.4 AllRAD reference decode, with LFE excluded from LUFS | Off |
| `--peak-limit-dbtp <dBTP>` | True Peak limit target | `-1.0` |
| `--peak-normalize-to-limit` | After loudness gain, raise global gain up to `--peak-limit-dbtp` when True Peak is below the ceiling; requires peak limiting | Off |
| `--final-gain-db <dB>` | Add unconstrained final gain after automatic loudness / peak staging; bypasses True Peak limiting and may exceed 0 dBFS | `0` |
| `--no-peak-limit` | Disable True Peak limiting | - |
| `--start <sec>` | Trim output so it starts at this second on the rendered timeline; loudness / True Peak are measured over the kept segment | `0` |
| `--end <sec>` | Trim output to this absolute second on the rendered timeline; must be greater than `--start`, unset means render to the end | Off |
| `--interp-ms <ms>` | Gain interpolation ramp when an ADM block has no jumpPosition | `5` |
| `--object-smoothing-frames <frames>` | Smoothing window for dynamic Objects metadata; `0` follows ADM blocks sample-by-sample; raise explicitly for extreme dynamic metadata; currently ignored by the Apple backend | `0` |
| `--opus-bitrate-per-ch <kbps>` | Opus VBR target bitrate per channel | Auto |
| `--apac-bitrate <kbps>` | APAC total bitrate hint; when unset, spatial layouts / HOA scale from the 7.1.4=2048 kbps baseline | See output-format notes |
| `--apac-container mpeg4\|caf` | APAC container; `caf` requires a `.caf` output path, while plain `.caf` remains PCM by default | `mpeg4` |
| `--sofa <path>` | User SOFA HRIR file for binaural rendering | Built-in KEMAR |
| `--semantic-policy <path>` | Apply ADM semantic-control JSON during rendering | Off |
| `--write-semantic-report <path>` | Write the effective semantic JSON after policy application | Off |

Post-processing order: `--loudness-target` determines the loudness gain first, `--peak-normalize-to-limit` can optionally add peak makeup to the True Peak ceiling, and `--peak-limit-dbtp` clamps the automatic gain stage; `--final-gain-db` is added after those automatic stages, so it bypasses True Peak limiting.

## Semantic Policy

Semantic policy does not modify the source AXML. It only affects the current render. `inspect --write-semantic-policy-template` writes an editable neutral template for the scene; applying the template unchanged is an identity operation.

```bash
./build/release/mradm inspect in.wav --write-semantic-policy-template policy.json
./build/release/mradm render -i in.wav -o out.flac --renderer binaural --semantic-policy policy.json
```

`global` applies to all content. `objects[]` contains rule-based overrides. Match dimensions are OR-combined: `id`, `name`, `name_glob`, `track_uid`, `all`, `importance_min/max`, `dialogue_id`, `content`, `programme`, plus HOA-only `pack_format`.

Supported override areas:

- **Objects**: object-level `gain` (`scale`, `gain_db`, `mute`), block-level `position`, `diffuse`, `extent`, `divergence`, `channel_lock`, and `interpolation`.
- **DirectSpeakers**: `direct_speakers` with per-block filters `speaker_label` / `lfe` (AND), gain, and position re-aiming.
- **HOA**: `id` / `pack_format` / `all` matching, with pack-level gain / mute.

More options:

```bash
./build/release/mradm render --help
```

## Build Options

Recommended local workflow:

```bash
cmake --preset debug
cmake --build --preset debug
ctest --test-dir build/debug --output-on-failure
```

Dependencies are fetched through `FetchContent` by default. If all dependencies are provided by the system, automatic fetching can be disabled:

```bash
cmake -S . -B build -DMR_ADM_CORE_FETCH_DEPS=OFF
```

FLAC and Opus providers:

| Option | Description |
|---|---|
| `MR_ADM_FLAC_PROVIDER=AUTO` / `MR_ADM_OPUS_PROVIDER=AUTO` | Default; Release uses vendored static libraries, Debug prefers system libraries |
| `VENDORED` | Force FetchContent static linking, suitable for release packages |
| `SYSTEM` | Force system libraries, suitable for package-manager / distro builds |

SOFA support is enabled by default:

```bash
cmake -S . -B build -DMR_ADM_ENABLE_SOFA=ON
```

## Quality Checks

```bash
./scripts/quality/check-changed.sh
./scripts/quality/format.sh --check
./scripts/quality/clang-tidy.sh build/debug
./scripts/quality/cppcheck.sh build/debug
```

## Documentation

- [ADM feature coverage audit](docs/architecture/ADM_FEATURE_COVERAGE.md)
- [Apple AUSpatialMixer backend implementation notes](docs/architecture/ADM_APPLE_BACKEND.md)
- [C++ ADM platform rewrite plan](docs/architecture/CPP_ADM_PLATFORM_REWRITE.md)
- [Architecture decision records](docs/adr/)
- [Quality tooling](docs/guides/QUALITY.md)
- [Third-party licenses and release boundary](docs/THIRD_PARTY_LICENSES.md)

## License

This project is licensed under the **MIT License**. See [LICENSE](LICENSE).

Binary release packages must include notice / license text for third-party dependencies.
