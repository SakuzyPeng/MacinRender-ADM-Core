# CLI Usage Guide

This is the complete reference for the `mradm` command-line tool: subcommands, channel-bed input, render backends,
output formats and layouts, common options, and semantic policy. See the [README](../../README.en.md) for an overview,
and `include/adm/c_api.h` plus [ADR 0007](../adr/0007-c-abi-stability-policy.md) for the C ABI.

## Subcommands

| Subcommand | Purpose |
|---|---|
| `render` | Render an ADM BWF or an ordinary channel-based WAVE file |
| `inspect` | Print ADM or auto-detected channel-bed scene metadata; can write a semantic-policy template |
| `backends` | List renderer backends, capabilities, and supported layouts |
| `layouts` | Show the final channel order for an output format, optionally filtered by renderer |
| `input-layouts` | List ordinary multichannel input presets and label geometry |
| `formats` | List output container formats with availability and constraints (channels, sample rate, bit depth, bitrate) |
| `export` | Write a new ADM BWF with semantic-policy overrides applied (source PCM reused, no re-render) |

Every subcommand supports `--help`. `./build/release/mradm` in the examples can be replaced with `bin/mradm` from a
release package.

## Quick Examples

Inspect an ADM scene and query available backends / layouts:

```bash
./build/release/mradm inspect input.wav
./build/release/mradm backends
./build/release/mradm input-layouts
./build/release/mradm layouts --format wav
./build/release/mradm layouts --format flac --renderer saf
./build/release/mradm formats
```

Render examples:

```bash
./build/release/mradm render -i input.wav -o out_binaural.wav --renderer saf-binaural
./build/release/mradm render -i input.wav -o out_714.flac --renderer ear --output-layout 7.1.4
./build/release/mradm render -i input.wav -o out_222.wav --renderer apple --output-layout 22.2
./build/release/mradm render -i input.wav -o out_room.wav --renderer triple-balance --output-layout 9.1.6
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
[complete channel-bed input semantics](CHANNEL_BED_INPUT.en.md), or query `mradm input-layouts` and
`mradm input-layouts --format json`.

Public two-channel output uses the `binaural` semantic, which is also the default. Backend and HRTF source are separate
choices: `triple-balance` is an independent Cartesian room renderer. Point sources use successive equal-power
panning along X/Y/Z, with verified 48 kHz isotropic-size processing. The existing 7.1.4 / 9.1.6 reference
behavior is retained; 22.2 is a project-defined extension. See the [Triple Balance renderer](../architecture/TRIPLE_BALANCE_RENDERER.md)
for scope and input limits. CLI, C ABI and GUI batch rendering are available; C API and GUI system-spatial monitoring
also support realtime playback, exact seeks, gain/mute and linked isotropic extent multipliers.
The former `--speaker-panner` option has been removed.

`saf-binaural` offers built-in KEMAR and build-enabled `--sofa` user HRIRs; `apple` uses the Apple system HRTF.
Current entry points are the CLI, the C++ API, and the C ABI.

## Render Backends

| Backend | CLI option | Input types | Output |
|---|---|---|---|
| EAR (compatibility name `libear`) | `--renderer auto` / `ear` | Objects / DirectSpeakers / HOA | Multichannel loudspeakers |
| SAF VBAP | `--renderer saf` | Objects / DirectSpeakers | Multichannel loudspeakers |
| Triple Balance | `--renderer triple-balance` | Cartesian Objects / standard 7.1.2 bed | 7.1.4 / 9.1.6 / 22.2, offline and realtime monitoring |
| HOA encoder | `--renderer hoa` | Objects / DirectSpeakers | HOA3 16ch (ACN/SN3D) |
| SAF HRTF binaural | `--renderer saf-binaural` | Objects / DirectSpeakers | 2ch binaural |
| Apple AUSpatialMixer | `--renderer apple` | Objects / DirectSpeakers | 2ch binaural / multichannel loudspeakers (macOS) |

The `saf-binaural` backend uses SAF's built-in Genelec KEMAR HRTF by default. A user FIR SOFA HRIR file can be loaded with `--sofa <path>`. Current SOFA specifications are SimpleFreeFieldHRIR / GeneralFIR, 2 receivers, and a native 48 kHz sample rate. The `apple` backend uses Apple's system HRTF; `mradm backends` reports each binaural backend's `HRTF sources`.

The recommended general-purpose external HRTF is the D1 KU100 SOFA from the [SADIE II Database](https://www.york.ac.uk/sadie-project/database.html), for example `D1_48K_24bit_256tap_FIR_SOFA.sofa` (also available from the [SOFA database SADIE index](https://sofacoustics.org/data/database/sadie/)). It is a 48 kHz, 256-tap SimpleFreeFieldHRIR dataset with dense direction sampling and low-frequency extension / diffuse-field EQ, making it a balanced default `--sofa` recommendation. The SADIE II data is published by the University of York under the Apache License 2.0; when distributing data or using it academically, follow the dataset page and cite [DOI:10.3390/app8112029](https://doi.org/10.3390/app8112029).

The `apple` backend uses AudioToolbox AUSpatialMixer and Apple platform rendering semantics. It supports binaural, 5.1, 7.1, 5.1.2, 5.1.4, 7.1.4, 9.1.6, and 22.2, plus speaker-output channelLock and extent cloud approximation. Loudspeaker output clears SpatialMixer's InterAuralDelay / DistanceAttenuation flags by default to match ADM / SAF / Logic-style gain semantics; `--apple-speaker-rendering-flags` restores Apple's native flags (the previous behaviour). `--apple-spatial-preset headphone-default|headphone-movie` applies an AUSpatialMixer headphone factory preset to Apple binaural output. SpatialMixer handles dynamic parameter smoothing. `--start` / `--end` use on-demand window rendering with one render block of pre-roll to update SpatialMixer state.

## Output Formats

### Codecs and Containers

| Codec | Lossy / Lossless | Container | Extension | Status |
|---|---|---|---|---|
| PCM float32 | Uncompressed | WAV / CAF | `.wav` / `.caf` | Cross-platform |
| PCM integer | Uncompressed | WAV | `.wav` | Cross-platform; 24-bit / 16-bit |
| FLAC | Lossless | FLAC | `.flac` | Cross-platform; fixed 24-bit, up to 8 channels |
| Opus | Lossy | Matroska Audio | `.mka` | Cross-platform; Opus VBR |
| Opus | Lossy | IAMF raw OBU | `.iamf` | Requires the official AOM iamf-tools bridge prebuilt SDK |
| APAC | Lossy | MPEG-4 Audio | `.m4a` / `.mp4` | macOS; AudioToolbox |
| APAC | Lossy | CAF | `.caf` | macOS; uses `--apac-container caf` |

The status column describes the codec / container combinations this project currently writes; target systems and players determine layout recognition and direct playback. Plain `.caf` output writes float32 PCM by default; `--apac-container caf` selects APAC-in-CAF.

### Uncompressed / Lossless Output

WAV can be written as float32, 24-bit, or 16-bit PCM. The scope of `--output-bit-depth` is WAV. Final WAV output carries machine-readable layout semantics: `5.1`, `5.1.2`, `7.1`, `5.1.4`, and `7.1.4` carry WAVEFORMATEXTENSIBLE masks in ascending mask-bit order, so `7.1.4` is written as `L R C LFE Rls Rrs Ls Rs ...`. `9.1.4`, `9.1.6`, and `22.2` carry ADM DirectSpeakers AXML/CHNA; `binaural` carries ADM Binaural `leftEar/rightEar`; and `hoa3` carries ADM HOA ACN/SN3D AXML/CHNA plus the `ambi` chunk.

WAV output and master input both support files larger than 4 GB. Float32 WAV always uses RF64; for ADM-labelled layouts this is an RF64 extension carrying AXML/CHNA, while integer `i24` / `i16` uses normative PCM BW64. Mask-labelled integer output uses RIFF up to 4 GB and RF64 above 4 GB so WAVEFORMATEXTENSIBLE semantics are retained. The reader accepts RIFF, RF64, BW64, and this project's float32 ADM RF64 output. Choose `--output-bit-depth i24` when a delivery chain explicitly requires PCM BW64. CAF currently writes float32 PCM and carries CoreAudio spatial layout tags. FLAC currently writes fixed 24-bit lossless audio, supports up to 8 channels, and exposes `binaural`, `5.1`, `7.1` and similar base-layer layouts. Height-channel lossless delivery uses WAV or CAF.

For lossless or uncompressed delivery with height channels or more than 8 channels, prefer WAV or CAF. Validate playback compatibility against the target player, container, layout tag, and channel count.

### Lossy Delivery Output

Opus MKA is Matroska Audio + Opus VBR and can be written on all supported platforms. Standard 5.1 / 7.1 layouts use Opus/Vorbis channel semantics; higher discrete layouts such as 9.1.6 and 22.2 use transparent multistream encoding with metadata. Full spatial-layout recognition depends on player capabilities.

IAMF output is a raw OBU stream (`.iamf`) with Opus for IAMF testing and delivery chains. IAMF encoding depends on the official AOM iamf-tools bridge; configure with `-DMR_ADM_ENABLE_IAMF=ON -DMR_ADM_IAMF_AOM_ROOT=/path/to/iamf-sdk`, where the SDK provides `lib/libmr_iamf_aom_bridge.*`. The IAMF layout range currently extends to `7.1.4`. By default IAMF writes a single layer for the final `--output-layout`; scalable channel layers use `--iamf-layers 5.1,5.1.2,5.1.4,7.1.4`, where the last layer matches the output layout and layers increase monotonically. A warning is recorded when a height output includes a planar layer, so the user can decide per delivery target.

APAC output writes MPEG-4 Audio (`.m4a` / `.mp4`) on macOS via AudioToolbox and currently requires 48 kHz. APAC-in-CAF uses a `.caf` output path with `--apac-container caf`. Spatial layouts and HOA use stable total-bitrate hints by default, scaled by channel count from a 7.1.4 baseline of 2048 kbps for 12 channels: for example about 1707 kbps for `5.1.4`, about 2731 kbps for `9.1.6` / `hoa3`, and about 4096 kbps for `22.2`. AudioToolbox treats this value as an encoder target / hint, so the measured bitrate can differ substantially.

### Containers, Layouts, and Playback

Channel order and spatial-layout semantics are determined by the combination of codec, container, and layout tag / mapping. The same codec may express layouts differently in different containers, and one container may carry different codecs. `mradm layouts --format <fmt>` reports the final channel order of the implemented combinations.

Playback compatibility depends on container, layout, and player. macOS has been verified to play PCM CAF, APAC `.mp4/.m4a`, and APAC-in-CAF spatial audio directly. PotPlayer has been observed to play discrete-channel Opus MKA, downmixed to 8ch; some Android devices with a system Opus decoder (such as `c2.android.opus.decoder` / `OMX.google.opus.decoder`) play Opus MKA up to `9.1.6`. How much spatial-layout semantics survive is determined by validating the target player.

Direct HOA playback on macOS uses CAF PCM, APAC MPEG-4, and APAC CAF. WAV HOA3 writes an AmbiX `ambi` chunk for AmbiX-aware tools. Opus MKA writes an ambisonics mapping; VLC 4.0 has been verified to play Opus HOA3 when the audio mix node is changed from `original: ambisonics` to `binaural`, so VLC decodes it to binaural output.

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

EAR and SAF VBAP share the same project layout registry; `9.1.4` / `9.1.6` are implemented for the EAR backend as project-defined custom layouts.

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

Channel order depends on the final output format. Query full tables with:

```bash
./build/release/mradm layouts --format wav
./build/release/mradm layouts --format caf
./build/release/mradm layouts --format apac
./build/release/mradm layouts --format flac --renderer ear
```

Common differences:

| Format | Layout | Final container / mapping | Final channel order |
|---|---|---|---|
| WAV / FLAC | `7.1` | WAVE_7_1 / `wav71` | L R C LFE Rls Rrs Ls Rs |
| WAV | `7.1.4` | WAVEFORMATEXTENSIBLE `0x2D63F` | L R C LFE Rls Rrs Ls Rs U+045 U-045 U+135 U-135 |
| WAV | `9.1.4` / `9.1.6` / `22.2` | ADM DirectSpeakers AXML/CHNA | Exact ADM order reported by `mradm layouts --format wav` |
| WAV | `binaural` | ADM Binaural `leftEar/rightEar` AXML/CHNA | leftEar rightEar |
| APAC / M4A | `7.1` | CoreAudio `AudioUnit_7_1` | L R C LFE Ls Rs Rls Rrs |
| APAC / CAF | `9.1.6` | CoreAudio `Atmos_9_1_6` | L R C LFE Ls Rs Rls Rrs Lw Rw Vhl Vhr Ltm Rtm Ltr Rtr |
| APAC / CAF | `22.2` | CoreAudio `CICP_13` | Lw Rw C LFE2 Rls Rrs L R Cs LFE3 Lss Rss Vhl Vhr Vhc Ts Ltr Rtr Ltm Rtm Ctr Cb Lb Rb |
| WAV | `hoa3` | ADM HOA AXML/CHNA + AmbiX `ambi` chunk | ACN/SN3D 16ch |
| CAF / APAC | `hoa3` | CoreAudio `HOA_ACN_SN3D` | ACN/SN3D 16ch |

## Common CLI Options

| Option | Description | Default |
|---|---|---|
| `--renderer auto\|ear\|saf\|triple-balance\|hoa\|saf-binaural\|apple` | Select the render backend | `auto` |
| `--input-layout auto\|5.1\|5.1.2\|7.1\|5.1.4\|7.1.4\|9.1.4\|9.1.6\|22.2` | Ordinary WAVE input layout; `auto` prefers ADM, then a recognised channel mask | `auto` |
| `--input-channels <csv>` | Custom ordinary-input labels in exact file-channel order; choose this or an explicit `--input-layout` | Off |
| `--output-layout <layout>` | Output semantic/layout: `binaural`, a multichannel layout, or `hoa3` | `binaural` |
| `--speaker-geometry standard\|apple` | EAR / SAF output-speaker coordinates; the Apple backend always uses CoreAudio geometry | `standard` |
| `--direct-speakers-routing auto\|label\|position\|matrix` | Native, label, position, or custom label-matrix DirectSpeakers routing | `auto` |
| `--direct-speakers-matrix <path>` | Strict v1 sparse-matrix JSON required by `matrix` mode | Off |
| `--output-bit-depth f32\|i24\|i16` | WAV output bit depth (CAF is fixed float32; FLAC is fixed 24-bit / up to 8 channels) | `f32` |
| `--loudness-target <LUFS>` | Normalize integrated loudness; HOA uses a 7.1.4 AllRAD reference decode and full-range channels for LUFS | Off |
| `--peak-limit-dbtp <dBTP>` | True Peak limit target | `-1.0` |
| `--peak-normalize-to-limit` | After loudness gain, raise global gain up to `--peak-limit-dbtp` when True Peak is below the ceiling; requires peak limiting | Off |
| `--final-gain-db <dB>` | Add final gain after automatic loudness / peak staging and True Peak limiting; the result may exceed 0 dBFS | `0` |
| `--no-peak-limit` | Set True Peak limiting to off | - |
| `--start <sec>` | Trim output so it starts at this second on the rendered timeline; loudness / True Peak are measured over the kept segment | `0` |
| `--end <sec>` | Trim output to this absolute second on the rendered timeline; the value is greater than `--start`, with timeline end as the default | Off |
| `--interp-ms <ms>` | Gain interpolation ramp when an ADM block omits jumpPosition | `5` |
| `--object-smoothing-frames <frames>` | Smoothing window for dynamic Objects metadata; `0` follows ADM blocks sample-by-sample; raise explicitly for extreme dynamic metadata; Apple delegates smoothing to SpatialMixer | `0` |
| `--apple-spatial-preset off\|headphone-default\|headphone-movie` | AUSpatialMixer factory preset for Apple binaural; the preset is applied first, then ADM-driven spatial parameters are written again | `off` |
| `--apple-speaker-rendering-flags` | Enable InterAuralDelay + DistanceAttenuation flags for Apple loudspeaker output (previous behaviour); off by default to match ADM / SAF gain semantics | Off |
| `--listener-yaw <deg>` / `--listener-pitch <deg>` / `--listener-roll <deg>` | Apple binaural listener head orientation; yaw in `[-180, 180]` with `+` to the left, pitch in `[-90, 90]` with `+` up, roll in `[-180, 180]` | `0` |
| `--opus-bitrate-per-ch <kbps>` | Opus VBR target bitrate per channel | Auto |
| `--apac-bitrate <kbps>` | APAC total bitrate hint; the default scales spatial layouts / HOA from the 7.1.4=2048 kbps baseline | See output-format notes |
| `--apac-container mpeg4\|caf` | APAC container; `caf` requires a `.caf` output path, and plain `.caf` still defaults to PCM | `mpeg4` |
| `--sofa <path>` | Select a user SOFA HRIR for a binaural backend reporting `user-sofa`; currently `saf-binaural` | SAF built-in KEMAR |
| `--semantic-policy <path>` | Apply ADM semantic-control JSON during rendering (gain / mute / position plus diffuse, extent, divergence, channelLock, interpolation, and more for Objects / DirectSpeakers / HOA) | Off |
| `--write-semantic-report <path>` | Write the effective semantic JSON after policy application, to confirm which Objects / DirectSpeakers / HOA rules matched and the original→effective changes | Off |

Post-processing order: `--loudness-target` determines the loudness gain first, `--peak-normalize-to-limit` can optionally add peak makeup to the True Peak ceiling, and `--peak-limit-dbtp` clamps the automatic gain stage; `--final-gain-db` is added after those automatic stages, so it bypasses True Peak limiting.

## Semantic Policy

Semantic policy applies to the current render while the source AXML remains intact. `inspect --write-semantic-policy-template` writes an editable neutral template for the scene; applying the template unchanged is an identity operation. Apply an edited policy with `--semantic-policy`:

```bash
./build/release/mradm inspect in.wav --write-semantic-policy-template policy.json
./build/release/mradm render -i in.wav -o out.flac --renderer saf-binaural --semantic-policy policy.json \
  --write-semantic-report report.json
# Write the policy-applied scene back as a new ADM BWF (source PCM reused, no re-render)
./build/release/mradm export -i in.wav -o out.adm.wav --semantic-policy policy.json
```

`--write-semantic-report` writes the effective semantic snapshot after the policy is applied, so you can confirm which rules matched and the original→effective changes.

`global` applies to all content. `objects[]` contains rule-based overrides. Match dimensions are OR-combined: `id`, `name`, `name_glob`, `track_uid`, `all`, `importance_min/max`, `dialogue_id`, `content`, `programme`; HOA rules also accept `pack_format`. Supported override areas:

- **Objects**: object-level `gain` (`scale`, `gain_db`, `mute`), `position` (absolute `azimuth/elevation/distance` + `offset` + `lock_*`), `diffuse`, `extent`, `divergence`, `channel_lock` (including `max_distance`), and `interpolation`.
- **DirectSpeakers**: `direct_speakers` with per-block filters `speaker_label` / `lfe` (AND), `gain` (`mute` silences the channel), and `position` re-aiming.
- **HOA**: `id` / `pack_format` / `all` matching, applying `gain` (`scale` / `gain_db` / `mute`) to the whole pack.

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

More options:

```bash
./build/release/mradm render --help
```
