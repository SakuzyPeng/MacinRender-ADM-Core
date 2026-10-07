# MacinRender ADM Core

English | [中文](README.md)

ITU-R BS.2076 ADM spatial-audio rendering core · C++20 + Rust 2024 · Rust 1.98 · stable C ABI v1 · cross-platform bit-identical gates

## What is this?

ADM (Audio Definition Model, ITU-R BS.2076) uses XML metadata to describe immersive audio such as objects, direct
speakers, and HOA, and is usually delivered as ADM BWF / BW64 files. MacinRender ADM Core (Chinese name: 麦渲峰 ADM Core)
reads an ADM scene or an ordinary multichannel WAVE file, **renders** it to loudspeaker layouts, HOA, or HRTF binaural
signals, and packages the result as WAV / CAF / FLAC / Opus MKA / IAMF / APAC. Besides offline rendering it provides a
realtime monitoring path with hot backend switching and head tracking.

There are three entry points: the `mradm` command-line tool, a stable C ABI library, and an Avalonia desktop GUI built
on that C ABI.

This project is **not** an ADM mastering tool: semantic policy rewrites semantics only at render time, and `export`
reuses the source PCM when it writes a new ADM file. Audio codecs are not implemented here either: FLAC / Opus / IAMF /
APAC encoding goes through libFLAC, libopus, AOM iamf-tools, and AudioToolbox respectively. The project makes no
Dolby / Apple product-certification claims.

> **Naming:** 麦渲峰 is the official Chinese name of MacinRender. The English brand name and technical identifiers,
> including repository, package, CMake target, namespace, and executable names, remain `MacinRender`.

This project is not affiliated with, sponsored by, or endorsed by Dolby Laboratories or Apple Inc. Dolby Atmos, Apple,
AirPods, and similar names are trademarks of their respective owners and are referenced only to describe compatibility.

## Features

- **ADM and channel-bed input**: ADM BWF / BW64 / RF64 scene import; ordinary multichannel WAVE mapped to a DirectSpeakers scene through a channel mask, a preset layout, or constrained custom labels
- **Multiple render backends**: EAR (BS.2127), VBAP, Triple Balance Cartesian room rendering, HOA3 encoding, HRTF binaural, and Apple AUSpatialMixer on macOS
- **ADM semantics**: Objects / DirectSpeakers / HOA with timed blocks, gain, interpolation, diffuse, extent, channelLock, and objectDivergence
- **Semantic policy**: rewrite object semantics at render time with JSON, write the effective-semantics snapshot, or export a policy-applied ADM BWF
- **Post-processing**: loudness normalization, True Peak limiting, bit-depth conversion; HOA is measured through a 7.1.4 AllRAD reference decode
- **Multi-format delivery**: WAV / CAF / FLAC / Opus MKA / IAMF / APAC; WAV output and master input both support files over 4 GB
- **Realtime monitoring**: hot switching of backend / layout / device, head tracking (OSC, AirPods), HpTF headphone compensation, system spatial audio (macOS / Windows)
- **Desktop workbench**: batch rendering, per-object semantic editing, realtime monitoring, and spatial visualization
- **Stable C ABI**: backward binary compatible since 1.0, structured progress callbacks, realtime Scene input
- **Cross-platform bit identity**: all 78 PCM cases in the consistency CI are bit-identical on macOS arm64 / Linux x64 / Windows x64 and gated
- **Rust numerical core**: DSP, EAR, ADM XML, and WAVE I/O run in Rust; project-owned algorithm crates forbid `unsafe`, which appears only in the private C boundary `mradm-ffi`

## Quick Start

### Prerequisites

- A C++20 compiler (Clang / GCC / MSVC) and CMake ≥ 3.24
- Rust 1.98.0 with rustfmt / clippy (see `rust/rust-toolchain.toml`)

```bash
rustup toolchain install 1.98.0 --profile minimal --component rustfmt --component clippy
```

C/C++ dependencies are fetched by CMake `FetchContent` by default, and Rust dependencies use the committed
`Cargo.lock`; production builds need no Boost, vcpkg, SAF, or OpenBLAS. APAC encoding and the Apple backend are
available only on macOS.

Prebuilt CLI and GUI packages for macOS arm64 and Windows x64 are available from
[GitHub Releases](https://github.com/SakuzyPeng/MacinRender-ADM-Core/releases); see
[release packages](docs/guides/BINARY_RELEASE.en.md) for artifacts and SHA-256 verification.

### Build and Test

```bash
cmake --preset debug
cmake --build --preset debug
ctest --preset debug --output-on-failure
```

Use a Release build for real renders:

```bash
cmake --preset release
cmake --build --preset release
```

### Inspect Scenes and Capabilities

```bash
./build/release/mradm inspect input.wav      # ADM / channel-bed scene metadata
./build/release/mradm backends               # render backends, capabilities, and supported layouts
./build/release/mradm formats                # output formats, availability, and constraints
./build/release/mradm layouts --format wav   # final channel order for an output format
```

### Common Build Options

| Option | Description | Default |
|---|---|---|
| `MR_ADM_CORE_FETCH_DEPS` | Turn off to use system dependencies; also requires Corrosion 0.6.1 and a prepared Cargo cache | `ON` |
| `MR_ADM_FLAC_PROVIDER` / `MR_ADM_OPUS_PROVIDER` | `AUTO` (vendored static for Release, system libraries preferred for Debug) / `VENDORED` / `SYSTEM` | `AUTO` |
| `MR_ADM_ENABLE_SOFA` | User SOFA HRIR support for the binaural backend | `ON` |
| `MR_ADM_ENABLE_IAMF` | IAMF encoding; needs `MR_ADM_IAMF_AOM_ROOT` pointing at the official AOM iamf-tools bridge SDK | `OFF` |
| `MR_ADM_BUILD_CAPI_BUNDLE` | Build the self-contained `libmradm_capi` shared library loaded by the GUI | `OFF` |
| `MR_ADM_BUILD_*_REFERENCE_TESTS` | Build legacy-library reference comparisons (SAF, libear, libadm, ...) without changing production code | `OFF` |

## Basic Usage

The most common commands are below. See the [CLI usage guide](docs/guides/CLI_USAGE.en.md) for every subcommand,
backend, output format and layout, option, and the semantic policy.

**Binaural → FLAC** — the default output semantic is `binaural`; `--sofa` selects a user HRIR:

```bash
./build/release/mradm render -i input.wav -o out.binaural.flac --renderer saf-binaural
```

**Loudspeaker layouts** — FLAC for ≤ 8-channel layouts without height, Opus MKA for height layouts (48 kHz required;
APAC `.m4a` on macOS):

```bash
./build/release/mradm render -i input.wav -o out.5_1.flac --renderer ear --output-layout 5.1
./build/release/mradm render -i input.wav -o out.7_1_4.mka --renderer ear --output-layout 7.1.4
```

**Third-order HOA**:

```bash
./build/release/mradm render -i input.wav -o out.hoa3.mka --renderer hoa --output-layout hoa3
```

**Ordinary multichannel input** — interpret WAVE channels with a preset or custom labels:

```bash
./build/release/mradm render -i bed.wav -o bed_714.mka --input-layout 5.1 --renderer ear --output-layout 7.1.4
```

**Semantic policy** — generate a neutral template, apply the edited policy, and write the effective-semantics snapshot:

```bash
./build/release/mradm inspect input.wav --write-semantic-policy-template policy.json
./build/release/mradm render -i input.wav -o out.flac --semantic-policy policy.json \
  --write-semantic-report report.json
```

**C ABI** — `include/adm/c_api.h` is the stable v1 interface: file rendering should use `adm_render_file_ex2` with
structured progress, realtime monitoring uses the `adm_monitor_*` family, and external decoders can submit per-object
PCM and spatial metadata through the realtime Scene interface. See [ADR 0007](docs/adr/0007-c-abi-stability-policy.md)
for the compatibility policy.

## Desktop GUI

The MacinRender GUI is an Avalonia / .NET NativeAOT desktop workbench that drives the same rendering core as the CLI
through the stable C ABI; renderer, layout, format, and platform capabilities come directly from core queries. Release
packages cover macOS arm64 and Windows x64.

| Workflow | Current capabilities |
|---|---|
| Batch rendering | Add files or folders, select renderer, layout, codec, and container, inspect structured progress and logs, and cancel jobs |
| Semantic editing | Load one ADM file and edit per-object gain, diffuse, extent, divergence, and head-tracking participation |
| Realtime monitoring | Compare semantic overrides during playback, switch renderer, layout, and output device, and use custom SOFA HRIRs; binaural monitoring supports mouse, trackpad, and keyboard yaw / pitch / roll control |
| Spatial visualization | Object positions, trails, and per-channel meters; the spatial character accepts a custom 64x64 PNG skin |
| Inspection and export | Export an effective ADM while preserving source audio and writing supported semantic changes |

The interface supports Chinese / English and dark / light themes; macOS additionally provides Apple AUSpatialMixer,
APAC, and AirPods head tracking. See the [semantic editor design](docs/architecture/SEMANTIC_EDITOR_GUI.md) and the
[realtime monitoring design](docs/architecture/REALTIME_MONITORING.md).

## Project Structure

```text
mradm (CLI) ──→ ADMEngine ──→ RenderService / monitor_session
GUI (C#) ──→ ADMCAPI ──→ ADMEngine
ADMEngine ──→ ADMRendererFactory ──→ ADMRender{Ear,VBAP,TripleBalance,HOA,Binaural,Apple}
          ──→ ADMRealtime / ADMIo / ADMMetadata / ADMAudio / ADMPeak / ADMLoudness
C++ modules ──→ ADMDsp ──→ mradm-ffi (Rust staticlib) ──→ mradm-dsp / mradm-ear / mradm-adm / mradm-wav
All modules ──→ ADMCore (domain model, errors, options, capabilities)
```

| Component | Responsibility |
|---|---|
| `ADMCore` | Project-owned ADM domain model (`AdmScene`), errors, logging, options, capabilities, and semantic policy |
| `ADMMetadata` / `ADMIo` | ADM AXML parsing and write-back, channel-bed scene synthesis, producing `AdmScene` |
| `ADMRender*` | EAR, VBAP, Triple Balance, HOA, HRTF binaural, and Apple AUSpatialMixer backends |
| `ADMRendererFactory` | Backend selection shared by offline rendering and realtime monitoring |
| `ADMAudio` / `ADMPeak` / `ADMLoudness` | Container encoding and metadata, loudness and True Peak |
| `ADMRealtime` | Realtime monitoring: MonitorEngine, SceneStream, device output, and head tracking |
| `ADMEngine` / `ADMCAPI` | `RenderService` orchestration and the stable C ABI |
| `rust/crates/mradm-dsp` | Numerical DSP: FFT, VBAP, HRTF/SOFA, convolution, OM spreader, resampling, metering, HOA, Monitor, and more |
| `rust/crates/mradm-ear` | Layouts, gains, and FIR design ported from libear |
| `rust/crates/mradm-adm` / `mradm-wav` | ADM XML metadata; WAVE / RF64 / BW64 I/O and container editing |
| `rust/crates/mradm-math` | Portable, cross-platform bit-identical sin/cos |
| `rust/crates/mradm-ffi` | The only private C boundary containing `unsafe` |
| `gui/MacinRender.Gui` | Avalonia NativeAOT desktop GUI calling the C ABI through P/Invoke |

See [ADR 0003](docs/adr/0003-owned-domain-model-and-backend-boundaries.md) for module boundaries and third-party type
isolation.

## Data Flow

```text
ADM BWF / BW64 / RF64, or ordinary multichannel WAVE
    → container and chunk reading (mradm-wav)
    → ADM AXML parsing (mradm-adm) or channel-bed synthesis
    → AdmScene  ← optional semantic-policy rewrite
    → RenderPlan → render backend (EAR / VBAP / Triple Balance / HOA / binaural / Apple)
    → post-processing (loudness normalization, True Peak limiting, bit-depth conversion)
    → container encoding (WAV / CAF / FLAC / Opus MKA / IAMF / APAC)
```

Realtime monitoring shares the same backend selection: worker threads render into a ring buffer and the audio callback
only does lightweight output.

## Current Status

| Area | Status | Summary |
|---|---|---|
| Render backends | ✅ | EAR, VBAP, Triple Balance, HOA3, HRTF binaural, Apple AUSpatialMixer (macOS) |
| Output formats | ✅ | WAV / CAF / FLAC / Opus MKA on all platforms; APAC on macOS only |
| IAMF | 🚧 | Needs the official AOM bridge SDK; layouts up to 7.1.4, 9.1.6 deferred for player compatibility |
| Realtime monitoring | ✅ | Hot switching, head tracking, HpTF, system spatial audio (Windows is static spatialization without OS head tracking) |
| C ABI | ✅ | Stable v1 (currently 1.43), compatibility maintained through `struct_size` extension and deprecation |
| Desktop GUI | ✅ | macOS arm64 / Windows x64 release packages |
| Rust phase 1 migration | ✅ | SAF, libear, libadm, libbw64, dr_wav, libebur128, and libsamplerate are out of the production path and kept only as reference comparisons |
| Rust phase 2 consistency | ✅ Closed | The original 78-case matrix is [closed](docs/architecture/RUST_PHASE2_CLOSEOUT.md), with initial performance recovery complete. [Follow-up coverage](docs/architecture/RUST_COVERAGE_EXTENSION.md) reaches 118/118 bit-identical PCM cases across three platforms |

See the [Rust adoption and SAF replacement roadmap](docs/architecture/RUST_SAF_REPLACEMENT_ROADMAP.md) for migration
scope and acceptance, and the [ADM feature coverage audit](docs/architecture/ADM_FEATURE_COVERAGE.md) for ADM coverage.

## Design Principles

1. A project-owned ADM domain model; third-party types never cross module boundaries, and render backends do not re-parse ADM.
2. Public APIs return `Result` for recoverable errors; every exported C ABI function is `noexcept` and translates errors to codes.
3. The C ABI is backward binary compatible since 1.0; struct extensions always use `struct_size`.
4. Numerical code lives in Rust and is always called from C++; algorithm crates forbid `unsafe`, and processing after preparation does not allocate.
5. Replacing a legacy library keeps a frozen reference implementation for comparison; the old implementation is never a silent runtime fallback.
6. Cross-platform bit identity is continuously verified by gates; a failing gate means fixing the implementation, not loosening the gate.
7. Test fixtures are generated at run time; the repository does not commit private or non-redistributable media.
8. GUI backend, format, and layout availability always comes from core capability queries, never a hard-coded support table.

## Documentation

| Document | Description |
|---|---|
| [CLI usage guide](docs/guides/CLI_USAGE.en.md) | Subcommands, backends, output formats and layouts, options, semantic policy |
| [Channel-bed input](docs/guides/CHANNEL_BED_INPUT.en.md) | Preset order, label geometry, channel masks, and constraints |
| [Release packages](docs/guides/BINARY_RELEASE.en.md) | Release artifacts, package contents, SHA-256, and launching the GUI |
| [Platform rewrite plan](docs/architecture/CPP_ADM_PLATFORM_REWRITE.md) | Module boundaries and long-term direction |
| [ADM feature coverage audit](docs/architecture/ADM_FEATURE_COVERAGE.md) | Supported ADM semantics |
| [Realtime monitoring](docs/architecture/REALTIME_MONITORING.md) | Monitoring path, backend switching, and device output |
| [Rust roadmap](docs/architecture/RUST_SAF_REPLACEMENT_ROADMAP.md) / [Phase 2 closeout](docs/architecture/RUST_PHASE2_CLOSEOUT.md) | Migration, accepted matrix, evidence, and follow-up boundaries |
| [Quality tooling](docs/guides/QUALITY.md) / [CI guide](docs/guides/CI.md) | Local checks, CI workflows, and gates |
| [Third-party licenses](docs/THIRD_PARTY_LICENSES.md) | Dependency licenses and release boundary |
| [Documentation index](docs/README.md) / [ADRs](docs/adr/) | All architecture documents and the 17 architecture decision records |

Architecture documents are mostly written in Chinese.

## License

This project's source code is licensed under the **MIT License**; see [LICENSE](LICENSE).

The default build dependencies are compatible with the MIT source license; binary release packages include notice /
license text for third-party dependencies. Ported algorithms, the built-in KEMAR set, and filter data keep their
original licenses and provenance; see [third-party licenses](docs/THIRD_PARTY_LICENSES.md).
