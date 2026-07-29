# MacinRender ADM Core

English | [中文](README.md)

MacinRender ADM Core is a cross-platform ADM (Audio Definition Model, ITU-R BS.2076) spatial-audio rendering core
written in C++20. It provides a desktop GUI, the `mradm` command-line tool, and a stable C ABI library.

It reads ADM BWF / BW64 and ordinary channel-based WAVE / RF64 / BW64 input, then renders to loudspeaker layouts, HOA encoding, HRTF binaural output, and delivery formats including WAV, CAF, FLAC, Opus MKA, IAMF, and APAC.

> **Naming:** 麦渲峰 is the official Chinese name of MacinRender. The English brand name and technical identifiers,
> including repository, package, CMake target, namespace, and executable names, remain `MacinRender`.

## Feature Overview

- ADM scene import: reads BW64 ADM metadata through libbw64 / libadm and converts it into the project's own domain model.
- Channel-bed input: maps known WAVE channel masks or constrained custom labels into a DirectSpeakers scene with explicit geometry.
- Desktop workbench: an Avalonia GUI for batch rendering, per-object semantic editing, and realtime spatial monitoring.
- Render backends: libear, SAF VBAP, HOA encoder, HRTF binaural, and Apple AUSpatialMixer (macOS-only).
- Objects / DirectSpeakers: supports timed blocks, gain, interpolation, diffuse, channelLock, objectDivergence, and related ADM semantics.
- Post-processing: loudness normalization, True Peak limiting, bit-depth conversion, and CAF / FLAC / Opus / APAC metadata. HOA output is measured through a 7.1.4 AllRAD reference decode; LFE is excluded from LUFS but included in True Peak.
- Platform boundary: core functionality targets macOS, Linux, and Windows; APAC encoding and the Apple AUSpatialMixer backend are macOS-only.

## Desktop GUI

The MacinRender GUI is an Avalonia desktop workbench backed by the same rendering core as the CLI through the stable
C ABI. Renderer, layout, format, and platform capabilities are queried from the core instead of being duplicated in
the interface. Current releases provide self-contained NativeAOT applications for macOS arm64 and Windows x64.

| Workflow | Current capabilities |
|---|---|
| Batch rendering | Add files or folders, select renderer, layout, codec, and container, inspect structured progress and logs, and cancel jobs |
| Semantic editing | Load one ADM file and edit per-object gain, diffuse, extent, divergence, and head-tracking participation |
| Realtime monitoring | Compare semantic overrides during playback, switch renderer, layout, and output device, and use custom SOFA HRIRs; Apple / SAF binaural monitoring supports hardware-free manual yaw, pitch, roll, and recentering through a mouse, trackpad, or keyboard |
| Spatial visualization | Object positions, trails, and per-channel meters; drag in a standard 64x64 PNG character skin, with automatic classic / slim model selection and persistence |
| Inspection and export | Export an effective ADM while preserving source audio and writing supported semantic changes |

The interface supports Chinese / English and dark / light themes, and exposes system spatial audio when supported by
the platform. macOS additionally provides Apple AUSpatialMixer, APAC, and AirPods head tracking. The GUI is an ADM
rendering and semantic workbench, not a timeline-based DAW or a general-purpose ADM authoring suite.

After extracting a release package, open `MacinRender ADM.app` on macOS. On Windows, run `MacinRender ADM.cmd` or
`app/MacinRender.Gui.exe`. Initial releases do not include macOS Developer ID / notarization or Windows Authenticode
signing. Linux currently ships the CLI AppImage only, with no GUI package.

For implementation details, see the [semantic editor design](docs/architecture/SEMANTIC_EDITOR_GUI.md) and
[realtime monitoring design](docs/architecture/REALTIME_MONITORING.md).

## CLI Quick Start

```bash
cmake -S . -B build/release -DCMAKE_BUILD_TYPE=Release
cmake --build build/release
```

Inspect an ADM scene and query available backends / layouts:

```bash
./build/release/mradm inspect input.wav
./build/release/mradm backends
./build/release/mradm input-layouts
./build/release/mradm layouts --format wav
./build/release/mradm layouts --format flac --renderer saf
```

Render examples:

```bash
./build/release/mradm render -i input.wav -o out_binaural.wav --renderer saf-binaural
./build/release/mradm render -i input.wav -o out_714.flac --renderer ear --output-layout 7.1.4
./build/release/mradm render -i input.wav -o out_222.wav --renderer apple --output-layout 22.2
./build/release/mradm render -i input.wav -o out_trim.wav --start 12.5 --end 45.0
./build/release/mradm render -i bed.wav -o bed_714.wav --input-layout 5.1 --renderer ear --output-layout 7.1.4
./build/release/mradm render -i custom.wav -o custom_binaural.wav --input-channels L,R,C,LFE,M+090,M-090 --renderer saf-binaural --sofa listener.sofa
```

## Ordinary Channel-Bed Input

Ordinary input accepts PCM 16/24/32-bit and IEEE float32 WAVE / RF64 / BW64 with 1–64 channels.
The default `--input-layout auto` imports ADM whenever an `axml` chunk is present; without `axml`, it uses a
recognised WAVEFORMATEXTENSIBLE channel mask. Invalid ADM never silently falls back. Presets are `5.1`, `5.1.2`,
`7.1`, `5.1.4`, `7.1.4`, `9.1.4`, `9.1.6`, and `22.2`; `--input-channels` provides a constrained custom order.

Azimuth is positive left, negative right, and `0°` front; elevation is positive up. Aliases have exact semantics:
`L/FL=M+030` (`+30°`, `0°`), `R/FR=M-030` (`-30°`, `0°`), `C/FC=M+000` (`0°`, `0°`), and
`LFE=LFE1` (no geometric position). Label count must exactly match the file; empty, unknown, and duplicate labels
are rejected. Custom `U±110` labels require `@30` or `@45` to select elevation. See the
[complete channel-bed input semantics](docs/guides/CHANNEL_BED_INPUT.en.md), or query `mradm input-layouts` and
`mradm input-layouts --format json`.

The only public two-channel output is `binaural`, which is also the default output semantic. Backend and HRTF source
are separate choices: `saf-binaural` offers built-in KEMAR and, when enabled at build time, `--sofa`; `apple` uses
the system HRTF and rejects user SOFA. The first implementation exposes this workflow through the CLI, C++ API, and
C ABI v1.28; the GUI entry point is deferred.

## Release Packages

The GitHub Actions release workflow produces auditable packages for tags matching `v*` and for manual release runs.

| Platform | Artifact | Baseline | Self-contained boundary |
|---|---|---|---|
| macOS arm64 | `mradm-<version>-macos-arm64.tar.gz` | Built on macOS 26 runner | No Homebrew / `/usr/local` dynamic-library dependency; Apple system libraries and frameworks are allowed |
| Linux x86_64 | `mradm-<version>-linux-x86_64.AppImage` | Ubuntu 24.04 x86_64 | Standalone AppImage; bundles non-core runtime libraries and rejects missing, build-tree, and `/usr/local` dependencies |
| Windows x64 | `mradm-<version>-windows-x64.zip` | Windows Server 2025 + MSVC | Includes `mradm.exe` and required DLLs, plus a `dumpbin /dependents` manifest |
| macOS GUI arm64 | `MacinRender-Gui-<version>-macos-arm64.tar.gz` | Built on macOS 26 runner | Self-contained `.app`; only Apple system libraries and frameworks may remain external |
| Windows GUI x64 | `MacinRender-Gui-<version>-windows-x64.zip` | Windows Server 2025 + MSVC | Includes the NativeAOT GUI, `mradm_capi.dll`, required DLLs, and a `dumpbin /dependents` manifest |

CLI packages contain:

- `bin/mradm`
- `LICENSE`
- `THIRD_PARTY_NOTICES.md`
- `BUILD_INFO.txt`
- `DEPENDENCIES.txt`

macOS CLI / GUI packages use `.tar.gz`, the Linux CLI uses `.AppImage`, and Windows CLI / GUI packages use `.zip`.
Each artifact has a matching `.sha256` file. GUI packages contain a macOS `.app` or
`app/MacinRender.Gui.exe`, together with license, build-information, checksum, and dependency files. macOS universal2,
Developer ID signing / notarization, and Windows Authenticode signing are not provided yet.

## Render Backends

| Backend | CLI option | Input types | Output |
|---|---|---|---|
| libear | `--renderer auto` / `ear` | Objects / DirectSpeakers / HOA | Multichannel loudspeakers |
| SAF VBAP | `--renderer saf` | Objects / DirectSpeakers | Multichannel loudspeakers |
| HOA encoder | `--renderer hoa` | Objects / DirectSpeakers | HOA3 16ch (ACN/SN3D) |
| SAF HRTF binaural | `--renderer saf-binaural` | Objects / DirectSpeakers | 2ch binaural |
| Apple AUSpatialMixer | `--renderer apple` | Objects / DirectSpeakers | 2ch binaural / multichannel loudspeakers (macOS-only) |

The `saf-binaural` backend uses SAF's built-in Genelec KEMAR HRTF by default. A user FIR SOFA HRIR file can be loaded with `--sofa <path>`. Current SOFA support is limited to SimpleFreeFieldHRIR / GeneralFIR, 2 receivers, 48 kHz, with no resampling. The `apple` backend uses Apple's system HRTF and rejects `--sofa`; `mradm backends` reports each binaural backend's `HRTF sources`.

The recommended general-purpose external HRTF is the D1 KU100 SOFA from the [SADIE II Database](https://www.york.ac.uk/sadie-project/database.html), for example `D1_48K_24bit_256tap_FIR_SOFA.sofa` (also available from the [SOFA database SADIE index](https://sofacoustics.org/data/database/sadie/)). It is a 48 kHz, 256-tap SimpleFreeFieldHRIR dataset with dense direction sampling and low-frequency extension / diffuse-field EQ, making it a more balanced `--sofa` recommendation than the built-in KEMAR for many headphone checks. The SADIE II data is published by the University of York under the Apache License 2.0; when distributing data or using it academically, follow the dataset page and cite [DOI:10.3390/app8112029](https://doi.org/10.3390/app8112029).

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

WAV can be written as float32, 24-bit, or 16-bit PCM. `--output-bit-depth` affects WAV only. Final WAV output is layout-labelled rather than a bare channel array: `5.1`, `5.1.2`, `7.1`, `5.1.4`, and `7.1.4` carry WAVEFORMATEXTENSIBLE masks in ascending mask-bit order, so `7.1.4` is written as `L R C LFE Rls Rrs Ls Rs ...`. `9.1.4`, `9.1.6`, and `22.2` carry ADM DirectSpeakers AXML/CHNA; `binaural` carries ADM Binaural `leftEar/rightEar` without a loudspeaker mask; and `hoa3` carries ADM HOA ACN/SN3D AXML/CHNA plus the `ambi` chunk.

Float32 WAV uses RF64. For ADM-labelled layouts this is an RF64 extension carrying AXML/CHNA; integer `i24` / `i16` uses normative PCM BW64. Mask-labelled integer output uses RIFF below 4 GB and RF64 above 4 GB so WAVEFORMATEXTENSIBLE semantics are retained. The reader accepts RIFF, RF64, BW64, and this project's float32 ADM RF64 output. Choose `--output-bit-depth i24` when a delivery chain explicitly requires PCM BW64. CAF currently writes float32 PCM and is useful in the CoreAudio ecosystem because it carries spatial layout tags. FLAC currently writes fixed 24-bit lossless audio, supports up to 8 channels, and is exposed only for `binaural`, `5.1`, and `7.1`-style non-height layouts.

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

| Format | Layout | Final container / mapping | Final channel order |
|---|---|---|---|
| WAV / FLAC | `7.1` | WAVE_7_1 / `wav71` | L R C LFE Rls Rrs Ls Rs |
| WAV | `7.1.4` | WAVEFORMATEXTENSIBLE `0x2D63F` | L R C LFE Rls Rrs Ls Rs U+045 U-045 U+135 U-135 |
| WAV | `9.1.4` / `9.1.6` / `22.2` | ADM DirectSpeakers AXML/CHNA | Exact ADM order reported by `mradm layouts --format wav` |
| WAV | `binaural` | ADM Binaural AXML/CHNA, no loudspeaker mask | leftEar rightEar |
| WAV | `hoa3` | ADM HOA AXML/CHNA + AmbiX `ambi` chunk | ACN/SN3D 16ch |

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
| `--renderer auto\|ear\|saf\|hoa\|saf-binaural\|apple` | Select the render backend | `auto` |
| `--input-layout auto\|5.1\|5.1.2\|7.1\|5.1.4\|7.1.4\|9.1.4\|9.1.6\|22.2` | Ordinary WAVE input layout; `auto` prefers ADM, then a recognised channel mask | `auto` |
| `--input-channels <csv>` | Custom ordinary-input labels in exact file-channel order; mutually exclusive with explicit `--input-layout` | Off |
| `--output-layout <layout>` | Output semantic/layout: `binaural`, a multichannel layout, or `hoa3` | `binaural` |
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
| `--sofa <path>` | Select a user SOFA HRIR for a backend reporting `user-sofa`; currently `saf-binaural` | SAF built-in KEMAR |
| `--semantic-policy <path>` | Apply ADM semantic-control JSON during rendering | Off |
| `--write-semantic-report <path>` | Write the effective semantic JSON after policy application | Off |

Post-processing order: `--loudness-target` determines the loudness gain first, `--peak-normalize-to-limit` can optionally add peak makeup to the True Peak ceiling, and `--peak-limit-dbtp` clamps the automatic gain stage; `--final-gain-db` is added after those automatic stages, so it bypasses True Peak limiting.

## Semantic Policy

Semantic policy does not modify the source AXML. It only affects the current render. `inspect --write-semantic-policy-template` writes an editable neutral template for the scene; applying the template unchanged is an identity operation.

```bash
./build/release/mradm inspect in.wav --write-semantic-policy-template policy.json
./build/release/mradm render -i in.wav -o out.flac --renderer saf-binaural --semantic-policy policy.json
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
