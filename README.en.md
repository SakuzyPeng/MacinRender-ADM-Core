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
- Render backends: libear, SAF VBAP, HOA encoder, HRTF binaural, and Apple AUSpatialMixer on macOS.
- Objects / DirectSpeakers: supports timed blocks, gain, interpolation, diffuse, channelLock, objectDivergence, and related ADM semantics.
- Post-processing: loudness normalization, True Peak limiting, bit-depth conversion, and CAF / FLAC / Opus / APAC metadata. HOA output is measured through a 7.1.4 AllRAD reference decode; LUFS uses full-range channels and True Peak covers every channel.
- Platform scope: core functionality targets macOS, Linux, and Windows; macOS also provides APAC encoding and the Apple AUSpatialMixer backend.

## Desktop GUI

The MacinRender GUI is an Avalonia desktop workbench backed by the same rendering core as the CLI through the stable
C ABI. The interface consumes renderer, layout, format, and platform capabilities queried directly from the core.
Current releases provide self-contained NativeAOT applications for macOS arm64 and Windows x64.

| Workflow | Current capabilities |
|---|---|
| Batch rendering | Add files or folders, select renderer, layout, codec, and container, inspect structured progress and logs, and cancel jobs |
| Semantic editing | Load one ADM file and edit per-object gain, diffuse, extent, divergence, and head-tracking participation |
| Realtime monitoring | Compare semantic overrides during playback, switch renderer, layout, and output device, and use custom SOFA HRIRs; Apple / SAF binaural monitoring supports hardware-free manual yaw, pitch, roll, and recentering through a mouse, trackpad, or keyboard |
| Spatial visualization | Object positions, trails, and per-channel meters; drag in a standard 64x64 PNG character skin, with automatic classic / slim model selection and persistence |
| Inspection and export | Export an effective ADM while preserving source audio and writing supported semantic changes |

The interface supports Chinese / English and dark / light themes, and exposes system spatial audio when supported by
the platform. macOS additionally provides Apple AUSpatialMixer, APAC, and AirPods head tracking. The GUI focuses on
ADM batch rendering, semantic editing, realtime monitoring, spatial visualization, inspection, and export.

After extracting a release package, open `MacinRender ADM.app` on macOS. On Windows, run `MacinRender ADM.cmd` or
`app/MacinRender.Gui.exe`. Initial packages use local developer distribution: macOS follows the Gatekeeper confirmation
flow and Windows follows the SmartScreen confirmation flow. Initial packages target macOS arm64 and Windows x64.

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
The default `--input-layout auto` follows an ordered detection path: an `axml` chunk selects ADM import, followed by a
recognised WAVEFORMATEXTENSIBLE channel mask. Invalid ADM produces an error. Presets are `5.1`, `5.1.2`,
`7.1`, `5.1.4`, `7.1.4`, `9.1.4`, `9.1.6`, and `22.2`; `--input-channels` provides a constrained custom order.

Azimuth is positive left, negative right, and `0°` front; elevation is positive up. Aliases have exact semantics:
`L/FL=M+030` (`+30°`, `0°`), `R/FR=M-030` (`-30°`, `0°`), `C/FC=M+000` (`0°`, `0°`), and
`LFE=LFE1` (the low-frequency semantic channel). Label count must exactly match the file; empty, unknown, and duplicate
labels produce errors. Custom `U±110` labels require `@30` or `@45` to select elevation. See the
[complete channel-bed input semantics](docs/guides/CHANNEL_BED_INPUT.en.md), or query `mradm input-layouts` and
`mradm input-layouts --format json`.

Public two-channel output uses the `binaural` semantic, which is also the default. Backend and HRTF source are separate
choices: `saf-binaural` offers built-in KEMAR and build-enabled `--sofa` user HRIRs; `apple` uses the Apple system HRTF.
Current entry points are the CLI, C++ API, and C ABI v1.34.

## Release Packages

The GitHub Actions release workflow produces auditable packages for tags matching `v*` and for manual release runs.

| Platform | Artifact | Baseline | Self-contained boundary |
|---|---|---|---|
| macOS arm64 | `mradm-<version>-macos-arm64.tar.gz` | Built on macOS 26 runner | Third-party libraries ship statically; external dependencies are Apple system libraries and frameworks |
| Windows x64 | `mradm-<version>-windows-x64.zip` | Windows Server 2025 + MSVC | Includes `mradm.exe` and required DLLs, plus a `dumpbin /dependents` manifest |
| macOS GUI arm64 | `MacinRender-Gui-<version>-macos-arm64.tar.gz` | Built on macOS 26 runner | Self-contained `.app`; external dependencies are Apple system libraries and frameworks |
| Windows GUI x64 | `MacinRender-Gui-<version>-windows-x64.zip` | Windows Server 2025 + MSVC | Includes the NativeAOT GUI, `mradm_capi.dll`, required DLLs, and a `dumpbin /dependents` manifest |

CLI packages contain:

- `bin/mradm`
- `LICENSE`
- `THIRD_PARTY_NOTICES.md`
- `BUILD_INFO.txt`
- `DEPENDENCIES.txt`

macOS CLI / GUI packages use `.tar.gz`, and Windows CLI / GUI packages use `.zip`. Each artifact has a matching
`.sha256` file. GUI packages contain a macOS `.app` or `app/MacinRender.Gui.exe`, together with license,
build-information, checksum, and dependency files. The initial platform set is macOS arm64 and Windows x64,
delivered through local developer distribution.

## Render Backends

| Backend | CLI option | Input types | Output |
|---|---|---|---|
| libear | `--renderer auto` / `ear` | Objects / DirectSpeakers / HOA | Multichannel loudspeakers |
| SAF VBAP | `--renderer saf` | Objects / DirectSpeakers | Multichannel loudspeakers |
| HOA encoder | `--renderer hoa` | Objects / DirectSpeakers | HOA3 16ch (ACN/SN3D) |
| SAF HRTF binaural | `--renderer saf-binaural` | Objects / DirectSpeakers | 2ch binaural |
| Apple AUSpatialMixer | `--renderer apple` | Objects / DirectSpeakers | 2ch binaural / multichannel loudspeakers (macOS) |

The `saf-binaural` backend uses SAF's built-in Genelec KEMAR HRTF by default. A user FIR SOFA HRIR file can be loaded with `--sofa <path>`. Current SOFA specifications are SimpleFreeFieldHRIR / GeneralFIR, 2 receivers, and a native 48 kHz sample rate. The `apple` backend uses Apple's system HRTF; `mradm backends` reports each binaural backend's `HRTF sources`.

The recommended general-purpose external HRTF is the D1 KU100 SOFA from the [SADIE II Database](https://www.york.ac.uk/sadie-project/database.html), for example `D1_48K_24bit_256tap_FIR_SOFA.sofa` (also available from the [SOFA database SADIE index](https://sofacoustics.org/data/database/sadie/)). It is a 48 kHz, 256-tap SimpleFreeFieldHRIR dataset with dense direction sampling and low-frequency extension / diffuse-field EQ, making it a balanced default `--sofa` recommendation for headphone checks. The SADIE II data is published by the University of York under the Apache License 2.0; when distributing data or using it academically, follow the dataset page and cite [DOI:10.3390/app8112029](https://doi.org/10.3390/app8112029).

The `apple` backend uses AudioToolbox AUSpatialMixer and Apple platform rendering semantics. It supports binaural, 5.1, 7.1, 5.1.2, 5.1.4, 7.1.4, 9.1.6, and 22.2, plus speaker-output channelLock and extent cloud approximation. SpatialMixer handles dynamic parameter smoothing. `--start` / `--end` use on-demand window rendering with one render block of pre-roll to update SpatialMixer state.

## Output Formats

### Codecs and Containers

| Codec | Lossy / Lossless | Container | Extension | Status |
|---|---|---|---|---|
| PCM float32 | Uncompressed | WAV / CAF | `.wav` / `.caf` | Cross-platform |
| PCM integer | Uncompressed | WAV | `.wav` | Cross-platform; 24-bit / 16-bit |
| FLAC | Lossless | FLAC | `.flac` | Cross-platform; fixed 24-bit, up to 8 channels |
| Opus | Lossy | Matroska Audio | `.mka` | Cross-platform; Opus VBR |
| Opus | Lossy | IAMF raw OBU | `.iamf` | Requires the official AOM iamf-tools bridge SDK |
| APAC | Lossy | MPEG-4 Audio | `.m4a` / `.mp4` | macOS; AudioToolbox |
| APAC | Lossy | CAF | `.caf` | macOS; uses `--apac-container caf` |

The status column describes the combinations this project currently writes. Target systems and players determine spatial-layout preservation during playback. Plain `.caf` output writes float32 PCM by default; `--apac-container caf` selects APAC-in-CAF.

### Uncompressed / Lossless Output

WAV can be written as float32, 24-bit, or 16-bit PCM. The scope of `--output-bit-depth` is WAV. Final WAV output carries machine-readable layout semantics: `5.1`, `5.1.2`, `7.1`, `5.1.4`, and `7.1.4` carry WAVEFORMATEXTENSIBLE masks in ascending mask-bit order, so `7.1.4` is written as `L R C LFE Rls Rrs Ls Rs ...`. `9.1.4`, `9.1.6`, and `22.2` carry ADM DirectSpeakers AXML/CHNA; `binaural` carries ADM Binaural `leftEar/rightEar`; and `hoa3` carries ADM HOA ACN/SN3D AXML/CHNA plus the `ambi` chunk.

Float32 WAV uses RF64. For ADM-labelled layouts this is an RF64 extension carrying AXML/CHNA; integer `i24` / `i16` uses normative PCM BW64. Mask-labelled integer output uses RIFF through 4 GB and RF64 above 4 GB so WAVEFORMATEXTENSIBLE semantics are retained. The reader accepts RIFF, RF64, BW64, and this project's float32 ADM RF64 output. Choose `--output-bit-depth i24` when a delivery chain explicitly requires PCM BW64. CAF currently writes float32 PCM and carries CoreAudio spatial layout tags. FLAC currently writes fixed 24-bit lossless audio, supports up to 8 channels, and exposes `binaural`, `5.1`, and `7.1`-style base-layer layouts. Height-channel lossless delivery uses WAV or CAF.

For lossless or uncompressed output with height channels or more than 8 channels, prefer WAV or CAF. Playback compatibility must still be validated against the target player.

### Lossy Delivery Output

Opus MKA is Matroska Audio + Opus VBR and can be written on all supported platforms. Standard 5.1 / 7.1 layouts use Opus/Vorbis channel semantics; higher discrete layouts such as 9.1.6 and 22.2 use transparent multistream encoding with metadata. Full spatial-layout recognition depends on player capabilities.

IAMF output is a raw OBU stream (`.iamf`) with Opus for IAMF testing and delivery chains. It uses the official AOM iamf-tools bridge at configure time:

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
| WAV | `binaural` | ADM Binaural `leftEar/rightEar` AXML/CHNA | leftEar rightEar |
| WAV | `hoa3` | ADM HOA AXML/CHNA + AmbiX `ambi` chunk | ACN/SN3D 16ch |

Direct HOA playback on macOS uses CAF PCM, APAC MPEG-4, and APAC CAF. WAV HOA3 writes an AmbiX `ambi` chunk for AmbiX-aware tools. Opus MKA writes an ambisonics mapping for compatible players.

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

EAR / SAF output-speaker geometry is selectable with `--speaker-geometry standard|apple`. The default `standard`
preserves the existing project / ADM nominal coordinates. `apple` uses CoreAudio's fixed coordinates for `5.1`, `7.1`,
`5.1.2`, `5.1.4`, `7.1.4`, `9.1.6`, and `22.2` (`5.1` is coordinate-identical). CoreAudio has no matching fixed
profile for project `9.1.4` or internal speaker stereo, so those combinations return unsupported instead of silently
falling back. The Apple renderer always uses CoreAudio geometry and ignores this option. LFE does not participate in
the geometry switch.

DirectSpeakers routing is selectable with `--direct-speakers-routing auto|label|position|matrix`. The default `auto`
resolves to `label` for SAF and Apple loudspeaker output:
an exact output-label match is attempted first, followed by the shared alias table (for example `L` → `M+030`), and
a match is routed one-hot to that output slot. A miss is spatialized from the known BS.2051/alias label direction when
available, otherwise from the ADM nominal coordinates. `position` ignores non-LFE labels and performs zero-spread,
non-interpolated spatialization; SAF uses the selected speaker geometry and Apple uses its AmbienceBed path. When a
coordinate fallback is needed but coordinates are missing, front centre `(0°,0°)` is used with a warning.

`matrix` uses `--direct-speakers-matrix <json>` to route every non-LFE input label to one or more target labels on
EAR, SAF, and Apple loudspeaker outputs. Each row is normalized as `sqrt(weight/sum)`, and `mute:true` is an explicit
silent route. The profile must bind the effective output layout and cover every non-LFE DirectSpeakers block;
duplicate, unknown, or ambiguous labels and LFE source/target rows are errors. LFE always takes the existing dedicated
route first. Apple binaural keeps `auto=position`, rejects explicit `label`, and rejects `matrix`; EAR adds only
`matrix`, while its explicit `label` / `position` boundary is unchanged. SAF binaural and HOA retain native `auto`
behaviour and reject every explicit mode. This phase exposes CLI, C++, and C ABI entry points; GUI controls are
deferred.
The desktop app exposes the same Speaker Geometry selector for EAR / SAF loudspeaker rendering and remembers the last
selection.

```bash
./build/release/mradm render -i input.wav -o saf_apple_222.wav \
  --renderer saf --output-layout 22.2 --speaker-geometry apple

./build/release/mradm render -i bed.wav -o remapped_714.wav \
  --renderer ear --output-layout 7.1.4 \
  --direct-speakers-routing matrix --direct-speakers-matrix routes.json
```

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
| `--input-channels <csv>` | Custom ordinary-input labels in exact file-channel order; choose this or an explicit `--input-layout` | Off |
| `--output-layout <layout>` | Output semantic/layout: `binaural`, a multichannel layout, or `hoa3` | `binaural` |
| `--speaker-geometry standard\|apple` | EAR / SAF output-speaker coordinates; the Apple backend always uses CoreAudio geometry | `standard` |
| `--direct-speakers-routing auto\|label\|position\|matrix` | Native, label, position, or custom label-matrix DirectSpeakers routing | `auto` |
| `--direct-speakers-matrix <path>` | Strict v1 sparse-matrix JSON required by `matrix` mode | Off |
| `--output-bit-depth f32\|i24\|i16` | WAV output bit depth; CAF is fixed float32, FLAC is fixed 24-bit / up to 8 channels | `f32` |
| `--loudness-target <LUFS>` | Normalize integrated loudness; HOA uses a 7.1.4 AllRAD reference decode and full-range channels for LUFS | Off |
| `--peak-limit-dbtp <dBTP>` | True Peak limit target | `-1.0` |
| `--peak-normalize-to-limit` | After loudness gain, raise global gain up to `--peak-limit-dbtp` when True Peak is below the ceiling; requires peak limiting | Off |
| `--final-gain-db <dB>` | Add final gain after automatic loudness / peak staging and True Peak limiting; the result may exceed 0 dBFS | `0` |
| `--no-peak-limit` | Set True Peak limiting to off | - |
| `--start <sec>` | Trim output so it starts at this second on the rendered timeline; loudness / True Peak are measured over the kept segment | `0` |
| `--end <sec>` | Trim output to this absolute second on the rendered timeline; the value is greater than `--start`, with timeline end as the default | Off |
| `--interp-ms <ms>` | Gain interpolation ramp when an ADM block omits jumpPosition | `5` |
| `--object-smoothing-frames <frames>` | Smoothing window for dynamic Objects metadata; `0` follows ADM blocks sample-by-sample; raise explicitly for extreme dynamic metadata; Apple delegates smoothing to SpatialMixer | `0` |
| `--opus-bitrate-per-ch <kbps>` | Opus VBR target bitrate per channel | Auto |
| `--apac-bitrate <kbps>` | APAC total bitrate hint; the default scales spatial layouts / HOA from the 7.1.4=2048 kbps baseline | See output-format notes |
| `--apac-container mpeg4\|caf` | APAC container; `caf` uses a `.caf` output path, and the default plain `.caf` mode is PCM | `mpeg4` |
| `--sofa <path>` | Select a user SOFA HRIR for a backend reporting `user-sofa`; currently `saf-binaural` | SAF built-in KEMAR |
| `--semantic-policy <path>` | Apply ADM semantic-control JSON during rendering | Off |
| `--write-semantic-report <path>` | Write the effective semantic JSON after policy application | Off |

Post-processing order: `--loudness-target` determines the loudness gain first, `--peak-normalize-to-limit` can optionally add peak makeup to the True Peak ceiling, and `--peak-limit-dbtp` clamps the automatic gain stage; `--final-gain-db` is added after those automatic stages, so it bypasses True Peak limiting.

## Semantic Policy

Semantic policy applies to the current render while the source AXML remains intact. `inspect --write-semantic-policy-template` writes an editable neutral template for the scene; applying the template unchanged is an identity operation.

```bash
./build/release/mradm inspect in.wav --write-semantic-policy-template policy.json
./build/release/mradm render -i in.wav -o out.flac --renderer saf-binaural --semantic-policy policy.json
```

`global` applies to all content. `objects[]` contains rule-based overrides. Match dimensions are OR-combined: `id`, `name`, `name_glob`, `track_uid`, `all`, `importance_min/max`, `dialogue_id`, `content`, `programme`; HOA rules also accept `pack_format`.

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

Dependencies are fetched through `FetchContent` by default. A system-dependency build uses:

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
