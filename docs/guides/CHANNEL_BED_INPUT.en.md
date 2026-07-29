# Ordinary Channel-Bed Input Semantics

English | [中文](CHANNEL_BED_INPUT.md)

`mradm render` accepts channel-based WAVE, RF64, and BW64 files without ADM metadata in addition to ADM BWF / BW64.
Supported sample encodings are PCM 16/24/32-bit and IEEE float32, with 1–64 channels. The importer synthesizes a
DirectSpeakers scene from the channel mapping and passes that scene to the selected renderer, so the same input can
produce multichannel, HOA, or binaural output.

## Detection precedence

With the default `--input-layout auto`:

1. An `axml` chunk selects strict ADM import. Invalid ADM is an error and never silently falls back.
2. Without `axml`, a recognised WAVEFORMATEXTENSIBLE channel mask selects a preset from the table below.
3. A missing mask, a mask whose set-bit count differs from the file channel count, or an unknown mask requires
   explicit `--input-layout` or `--input-channels`.

An explicit mapping forces ordinary channel-bed import. If `axml` is also present, it is ignored with a warning.
`--input-layout` and `--input-channels` are mutually exclusive.

## Presets and file-channel order

The table gives the exact file-channel order for explicit `--input-layout`; channels are never guessed or reordered
by name.

| `--input-layout` | Channels | Auto mask | File-channel order |
|---|---:|---:|---|
| `5.1` | 6 | `0x003F` | `M+030 M-030 M+000 LFE1 M+110 M-110` |
| `5.1.2` | 8 | `0x503F` | `M+030 M-030 M+000 LFE1 M+110 M-110 U+030 U-030` |
| `7.1` | 8 | `0x063F` | `M+030 M-030 M+000 LFE1 M+135 M-135 M+090 M-090` |
| `5.1.4` | 10 | `0x2D03F` | `M+030 M-030 M+000 LFE1 M+110 M-110 U+030 U-030 U+110 U-110` |
| `7.1.4` | 12 | `0x2D63F` | `M+030 M-030 M+000 LFE1 M+135 M-135 M+090 M-090 U+045 U-045 U+135 U-135` |
| `9.1.4` | 14 | — | `M+030 M-030 M+000 LFE1 M+110 M-110 M+150 M-150 M+070 M-070 U+070 U-070 U+150 U-150` |
| `9.1.6` | 16 | — | `M+030 M-030 M+000 LFE1 M+110 M-110 M+150 M-150 M+070 M-070 U+070 U-070 U+110 U-110 U+150 U-150` |
| `22.2` | 24 | — | `M+060 M-060 M+000 LFE1 M+135 M-135 M+030 M-030 M+180 LFE2 M+090 M-090 U+045 U-045 U+000 T+000 U+135 U-135 U+090 U-090 U+180 B+000 B+045 B-045` |

Every mask-bearing explicit preset and automatic mapping follows ascending WAVEFORMATEXTENSIBLE mask-bit order.
For `7.1.4`, back L/R (`0x10/0x20`) therefore precede side L/R (`0x200/0x400`). A legacy file in renderer/Atmos
order, with side L/R before back L/R, must not be declared as standard `--input-layout 7.1.4`; describe its actual
order with `--input-channels`. `mradm input-layouts` prints final file order, and JSON keeps `channels` and
`wave_mask_channel_order` identical for mask-bearing layouts.

Query exact per-channel geometry and the machine-readable schema with:

```bash
mradm input-layouts
mradm input-layouts --layout 7.1.4
mradm input-layouts --format json
```

## Coordinates, labels, and aliases

Azimuth is `0°` front, positive left, and negative right. Elevation is `0°` on the horizontal plane and positive up.
Every non-LFE channel has normalized distance `1.0`. LFE is a low-frequency semantic channel with no geometric
position.

| Input alias | Canonical label | Exact semantics |
|---|---|---|
| `L` / `FL` | `M+030` | azimuth `+30°` (left), elevation `0°` |
| `R` / `FR` | `M-030` | azimuth `-30°` (right), elevation `0°` |
| `C` / `FC` | `M+000` | azimuth `0°` (front), elevation `0°` |
| `LFE` | `LFE1` | LFE, no geometric position |

The `M`, `U`, `T`, and `B` labels and exact geometry come from the catalog printed by `mradm input-layouts`. Matching
is case-insensitive and trims surrounding whitespace. `U+110` and `U-110` each occur at both `30°` and `45°`
elevation, so custom mappings must use `U+110@30`, `U-110@30`, `U+110@45`, or `U-110@45`.

Custom-mapping constraints:

- Supply 1–64 labels, exactly one for each file channel and in file order.
- Empty, unknown, and duplicate canonical speaker labels are errors.
- Only controlled catalog labels are accepted; arbitrary coordinates and unregistered names are not supported.
- `--input-channels L,R,C,LFE,M+090,M-090` is a valid six-channel example. Input `L` / `R` describe source
  geometry only and do not change output semantics.

## Output semantics, backends, and HRTFs

The only public two-channel output is `binaural`, and it is the default output semantic. Renderer and HRTF source are
separate choices:

| Output | Renderer choice | HRTF source |
|---|---|---|
| Binaural | `auto` / `saf-binaural` | SAF built-in KEMAR; `--sofa <file>` when user SOFA is enabled |
| Binaural (macOS) | `apple` | Apple system HRTF; `--sofa` is rejected |
| Multichannel | `ear` / `saf` / `apple` (macOS) | No HRTF |
| HOA3 | `hoa` | No HRTF |

`mradm backends` reports the current platform/build truth under `HRTF sources`. Passing `--sofa` to a backend that
does not report `user-sofa` is an error; the engine does not switch backend or ignore the file.

WAV output does not rely on a text comment to declare its layout:

- `5.1`, `5.1.2`, `7.1`, `5.1.4`, and `7.1.4` carry a WAVEFORMATEXTENSIBLE channel mask and samples are written in
  mask-bit order. A renderer-native order is converted exactly once at the final file boundary.
- `9.1.4`, `9.1.6`, and `22.2` carry ADM DirectSpeakers AXML/CHNA with per-channel labels and geometry. They do not
  claim a WAVE mask that cannot express their exact positions.
- `binaural` carries ADM Binaural `leftEar` / `rightEar` AXML/CHNA and no loudspeaker mask. `hoa3` carries ADM HOA
  ACN/SN3D AXML/CHNA and retains the `ambi` chunk.
- `i24` / `i16` ADM spatial WAV uses PCM BW64. The default `f32` output uses RF64 with the same AXML/CHNA; choose
  `--output-bit-depth i24` when a delivery chain requires normative PCM BW64.

Use `mradm layouts --format wav` for the final container semantics and file-channel order of every output layout.

Standards basis: [Microsoft WAVEFORMATEXTENSIBLE](https://learn.microsoft.com/en-us/windows/win32/api/mmreg/ns-mmreg-waveformatextensible)
requires channels present in a mask to be ordered from the least-significant set bit upward. ADM element semantics
follow [ITU-R BS.2076-3](https://www.itu.int/rec/R-REC-BS.2076/), while PCM BW64, AXML, and CHNA carriage follows
[ITU-R BS.2088-2](https://www.itu.int/dms_pubrec/itu-r/rec/bs/R-REC-BS.2088-2-202511-I%21%21PDF-E.pdf).

```bash
# Auto-detect a 5.1 WAVE mask and use the default binaural output.
mradm render -i bed.wav -o bed_binaural.wav

# Explicit input preset to 7.1.4 output.
mradm render -i bed.wav --input-layout 5.1 \
  --renderer ear --output-layout 7.1.4 -o bed_714.wav

# Custom input order, SAF binaural, and a selected SOFA dataset.
mradm render -i custom.wav --input-channels L,R,C,LFE,M+090,M-090 \
  --renderer saf-binaural --output-layout binaural --sofa listener.sofa \
  -o custom_binaural.wav

# Apple system HRTF; do not pass --sofa.
mradm render -i bed.wav --input-layout 5.1 \
  --renderer apple --output-layout binaural -o apple_binaural.wav
```

## Synthesized scene

Each ordinary input deterministically becomes one `Channel Bed` programme, content, and object. Every file channel
maps to one DirectSpeakers track spanning the entire file. Non-LFE tracks use the exact azimuth/elevation above; LFE
tracks carry low-frequency semantics without a spatial position. `inspect` and scene JSON report
`source_kind=channel_bed`, the resolved input layout, and file-channel order.

C++ callers use `RenderOptions::input_layout` or `input_channel_labels`. C ABI v1.28 provides
`adm_render_options_set_input_layout`, `adm_render_options_set_input_channel_labels`, and `adm_input_layouts_json`;
the JSON schema is `mradm.input-layouts` v1.
