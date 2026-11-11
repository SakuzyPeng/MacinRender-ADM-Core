//! Discovery documents, moved from the C++ engine's `capability_json`,
//! `layout_table`, `format_table`, `input_layout_json` and `support_matrix`. Each document is
//! byte-identical to its former nlohmann `dump(2)`: keys in byte order, floats widened to double.
use crate::catalog::{self, Backend, Layout, Mode, Platform};
use mradm_json::{Map, Value};
use mradm_scene::input::{InputChannel, channel_catalog, input_layouts};

macro_rules! map {
    ($($key:expr => $value:expr),* $(,)?) => {{
        #[allow(unused_mut)]
        let mut map = Map::new();
        $(map.insert($key.to_owned(), Value::from($value));)*
        map
    }};
}

macro_rules! object {
    ($($tt:tt)*) => {
        Value::Object(map! { $($tt)* })
    };
}

fn strings(values: &[String]) -> Value {
    Value::Array(values.iter().map(|s| Value::from(s.as_str())).collect())
}

fn strs(values: &[&str]) -> Value {
    Value::Array(values.iter().map(|s| Value::from(*s)).collect())
}

fn dump(map: Map) -> String {
    mradm_json::dump(&Value::Object(map), Some(2))
}

// ── capabilities ─────────────────────────────────────────────────────────────────────────────

fn layout_json(layout: &Layout) -> Value {
    object! {
        "id" => layout.id.as_str(),
        "display_name" => layout.display_name.as_str(),
        "channel_count" => u32::from(layout.channel_count),
        "is_3d" => layout.is_3d,
        "lfe_count" => u32::from(layout.lfe_count),
        "supports_spread" => layout.supports_spread,
        "is_binaural" => layout.is_binaural,
    }
}

fn mode_json(mode: &Mode) -> Value {
    object! {
        "id" => mode.id.as_str(),
        "display_name" => mode.display_name.as_str(),
        "layouts" => strings(&mode.layouts),
        "sample_rates" => Value::Array(mode.sample_rates.iter().map(|&r| Value::from(r)).collect()),
        "supports_realtime" => mode.supports_realtime,
    }
}

fn backend_json(backend: &Backend) -> Value {
    let mut j = map! {
        "renderer" => backend.renderer.as_str(),
        "backend_name" => backend.name.as_str(),
        "backend_version" => backend.version.as_str(),
        "supports_objects" => backend.supports_objects,
        "supports_direct_speakers" => backend.supports_direct_speakers,
        "supports_hoa" => backend.supports_hoa,
        "supports_channel_lock" => backend.supports_channel_lock,
        "supports_object_divergence" => backend.supports_object_divergence,
        "supports_screen_ref" => backend.supports_screen_ref,
        "supports_diffuse" => backend.supports_diffuse,
        "hrtf_sources" => strings(&backend.hrtf_sources),
    };
    if !backend.modes.is_empty() {
        j.insert(
            "modes".into(),
            Value::Array(backend.modes.iter().map(mode_json).collect()),
        );
    }
    if let Some(first) = backend.qualities.first() {
        j.insert("default_quality".into(), Value::from(first.id.as_str()));
        j.insert(
            "qualities".into(),
            Value::Array(backend.qualities.iter().map(mode_json).collect()),
        );
    }
    // The 0+2+0 renderer path exists only for diagnostics and tests. Public capability
    // discovery exposes two-channel output solely as binaural.
    let layouts = backend
        .layouts
        .iter()
        .filter(|l| l.is_binaural || l.channel_count != 2)
        .map(layout_json)
        .collect();
    j.insert("layouts".into(), Value::Array(layouts));
    Value::Object(j)
}

/// `mradm.capabilities` v1 (`adm_capabilities_json`).
pub fn capabilities_json(platform: &Platform) -> String {
    let mut root = map! {
        "schema" => "mradm.capabilities",
        "schema_version" => 1u32,
        "backends" => Value::Array(catalog::backends(platform).iter().map(backend_json).collect()),
    };
    // The GUI's authoritative list for the system-spatial monitor option.
    let spatial = platform
        .system_spatial_layouts
        .iter()
        .map(|l| {
            object! {
                "id" => l.id.as_str(),
                "display_name" => l.display_name.as_str(),
                "channel_count" => u32::from(l.channel_count),
            }
        })
        .collect();
    root.insert("system_spatial_layouts".into(), Value::Array(spatial));
    if let Some(ok) = platform.apple_system_spatial_lfe_routing_ok {
        root.insert(
            "apple_system_spatial_lfe_routing_ok".into(),
            Value::from(ok),
        );
    }
    dump(root)
}

// ── output layouts ───────────────────────────────────────────────────────────────────────────

/// One row of the output channel-order reference.
#[derive(Clone, Debug, PartialEq)]
pub struct LayoutRow {
    pub format: &'static str,
    pub layout: &'static str,
    pub channels: u32,
    pub container: &'static str,
    pub order: &'static str,
    pub note: &'static str,
    /// Renderers whose capabilities include the layout, computed from the catalog.
    pub supported_by: Vec<String>,
}

type LayoutInfo = (
    &'static str,
    &'static str,
    u32,
    &'static str,
    &'static str,
    &'static str,
);

const LAYOUT_INFOS: [LayoutInfo; 33] = [
    (
        "wav",
        "binaural",
        2,
        "RF64/BW64 + ADM Binaural AXML/CHNA",
        "leftEar rightEar",
        "No loudspeaker channel mask is written.",
    ),
    (
        "wav",
        "5.1",
        6,
        "WAVEFORMATEXTENSIBLE mask 0x0000003F",
        "L R C LFE Ls Rs",
        "",
    ),
    (
        "wav",
        "5.1.2",
        8,
        "WAVEFORMATEXTENSIBLE mask 0x0000503F",
        "L R C LFE Ls Rs U+030 U-030",
        "",
    ),
    (
        "wav",
        "7.1",
        8,
        "WAVEFORMATEXTENSIBLE mask 0x0000063F",
        "L R C LFE Rls Rrs Ls Rs",
        "WAVE mask-bit order.",
    ),
    (
        "wav",
        "5.1.4",
        10,
        "WAVEFORMATEXTENSIBLE mask 0x0002D03F",
        "L R C LFE Ls Rs U+030 U-030 U+110 U-110",
        "",
    ),
    (
        "wav",
        "7.1.4",
        12,
        "WAVEFORMATEXTENSIBLE mask 0x0002D63F",
        "L R C LFE Rls Rrs Ls Rs U+045 U-045 U+135 U-135",
        "Renderer order is converted to WAVE mask-bit order at write time.",
    ),
    (
        "wav",
        "9.1.4",
        14,
        "PCM BW64 / float RF64 + ADM DirectSpeakers AXML/CHNA",
        "L R C LFE Ls Rs Rls Rrs Lw Rw Vhl Vhr Ltr Rtr",
        "No WAVE mask can express the exact geometry.",
    ),
    (
        "wav",
        "9.1.6",
        16,
        "ADM AXML/CHNA; Triple Balance: WAVE mask 0",
        "L R C LFE Ls Rs Rls Rrs Lw Rw Vhl Vhr Ltm Rtm Ltr Rtr",
        "No WAVE mask can express the exact geometry. Triple Balance writes WAVEFORMATEXTENSIBLE with mask 0, \
         in the same channel order, without ADM; use CAF for explicit 9.1.6 layout metadata.",
    ),
    (
        "wav",
        "22.2",
        24,
        "PCM BW64 / float RF64 + ADM DirectSpeakers AXML/CHNA",
        "M+060 M-060 M+000 LFE1 M+135 M-135 M+030 M-030 M+180 LFE2 M+090 M-090 U+045 U-045 \
         U+000 T+000 U+135 U-135 U+090 U-090 U+180 B+000 B+045 B-045",
        "Exact BS.2051 geometry is carried by ADM.",
    ),
    (
        "wav",
        "hoa3",
        16,
        "PCM BW64 / float RF64 + ADM HOA AXML/CHNA + ambi",
        "ACN 0..15, SN3D",
        "No loudspeaker channel mask is written.",
    ),
    (
        "caf",
        "binaural",
        2,
        "CoreAudio Binaural",
        "BinauralLeft BinauralRight",
        "",
    ),
    (
        "caf",
        "5.1",
        6,
        "CoreAudio MPEG_5_1_A / CICP_6",
        "L R C LFE Ls Rs",
        "",
    ),
    (
        "caf",
        "7.1",
        8,
        "CoreAudio WAVE_7_1",
        "L R C LFE Rls Rrs Ls Rs",
        "Same order as internal wav71.",
    ),
    (
        "caf",
        "hoa3",
        16,
        "CoreAudio HOA_ACN_SN3D",
        "ACN 0..15, SN3D",
        "HOA encode output, not a speaker layout.",
    ),
    (
        "caf",
        "5.1.4",
        10,
        "CoreAudio Atmos_5_1_4",
        "L R C LFE Ls Rs Vhl Vhr Ltr Rtr",
        "",
    ),
    (
        "caf",
        "7.1.4",
        12,
        "CoreAudio Atmos_7_1_4",
        "L R C LFE Ls Rs Rls Rrs Vhl Vhr Ltr Rtr",
        "",
    ),
    (
        "caf",
        "9.1.6",
        16,
        "CoreAudio Atmos_9_1_6",
        "L R C LFE Ls Rs Rls Rrs Lw Rw Vhl Vhr Ltm Rtm Ltr Rtr",
        "",
    ),
    (
        "caf",
        "22.2",
        24,
        "CoreAudio CICP_13",
        "Lw Rw C LFE2 Rls Rrs L R Cs LFE3 Lss Rss Vhl Vhr Vhc Ts Ltr Rtr Ltm Rtm Ctr Cb Lb Rb",
        "CoreAudio names the two LFE slots LFE2/LFE3.",
    ),
    (
        "flac",
        "binaural",
        2,
        "FLAC Vorbis Comment layout=binaural",
        "Binaural L, Binaural R",
        "Some readers may display only generic two-channel metadata.",
    ),
    (
        "flac",
        "5.1",
        6,
        "FLAC + WAVEFORMATEXTENSIBLE mask 0x0000003F",
        "L R C LFE Ls Rs",
        "",
    ),
    (
        "flac",
        "7.1",
        8,
        "FLAC + WAVEFORMATEXTENSIBLE mask 0x0000063F",
        "L R C LFE Rls Rrs Ls Rs",
        "WAVE_7_1 / wav71 order.",
    ),
    (
        "apac",
        "binaural",
        2,
        "APAC requests CoreAudio Binaural",
        "L R",
        "Some inspection tools report a generic two-channel layout; metadata preserves layout=binaural.",
    ),
    (
        "apac",
        "7.1",
        8,
        "CoreAudio AudioUnit_7_1",
        "L R C LFE Ls Rs Rls Rrs",
        "Input wav71 is reordered before APAC encoding.",
    ),
    (
        "apac",
        "hoa3",
        16,
        "CoreAudio HOA_ACN_SN3D",
        "ACN 0..15, SN3D",
        "HOA encode output, not a speaker layout.",
    ),
    (
        "apac",
        "5.1.4",
        10,
        "CoreAudio Atmos_5_1_4",
        "L R C LFE Ls Rs Vhl Vhr Ltr Rtr",
        "",
    ),
    (
        "apac",
        "7.1.4",
        12,
        "CoreAudio Atmos_7_1_4",
        "L R C LFE Ls Rs Rls Rrs Vhl Vhr Ltr Rtr",
        "",
    ),
    (
        "apac",
        "9.1.6",
        16,
        "CoreAudio Atmos_9_1_6",
        "L R C LFE Ls Rs Rls Rrs Lw Rw Vhl Vhr Ltm Rtm Ltr Rtr",
        "",
    ),
    (
        "apac",
        "22.2",
        24,
        "CoreAudio CICP_13",
        "Lw Rw C LFE2 Rls Rrs L R Cs LFE3 Lss Rss Vhl Vhr Vhc Ts Ltr Rtr Ltm Rtm Ctr Cb Lb Rb",
        "CoreAudio names the two LFE slots LFE2/LFE3.",
    ),
    (
        "iamf",
        "5.1",
        6,
        "IAMF kLayout5_1_ch (Opus VBR)",
        "FL FR SiL SiR C LFE (encoding order)",
        "",
    ),
    (
        "iamf",
        "5.1.2",
        8,
        "IAMF kLayout5_1_2_ch (Opus VBR)",
        "FL FR SiL SiR TpFL TpFR C LFE (encoding order)",
        "",
    ),
    (
        "iamf",
        "5.1.4",
        10,
        "IAMF kLayout5_1_4_ch (Opus VBR)",
        "FL FR SiL SiR TpFL TpFR TpBL TpBR C LFE (enc order)",
        "",
    ),
    (
        "iamf",
        "7.1",
        8,
        "IAMF kLayout7_1_ch (Opus VBR)",
        "FL FR Ls Rs Rls Rrs C LFE (wav71 reordered)",
        "",
    ),
    (
        "iamf",
        "7.1.4",
        12,
        "IAMF kLayout7_1_4_ch (Opus VBR)",
        "FL FR Ls Rs Rls Rrs TpFL TpFR TpBL TpBR C LFE",
        "",
    ),
];

/// The output channel-order reference (`mradm layouts`, `adm_layouts_json`).
pub fn output_layouts(platform: &Platform) -> Vec<LayoutRow> {
    let backends = catalog::backends(platform);
    LAYOUT_INFOS
        .iter()
        .map(
            |&(format, layout, channels, container, order, note)| LayoutRow {
                format,
                layout,
                channels,
                container,
                order,
                note,
                supported_by: backends
                    .iter()
                    .filter(|b| catalog::find_layout(b, layout).is_some())
                    .map(|b| b.renderer.clone())
                    .collect(),
            },
        )
        .collect()
}

/// `mradm.layouts` v1.
pub fn layouts_json(platform: &Platform) -> String {
    let rows = output_layouts(platform)
        .iter()
        .map(|row| {
            let mut j = map! {
                "format" => row.format,
                "layout" => row.layout,
                "channels" => row.channels,
                "container" => row.container,
                "order" => row.order,
                "supported_by" => strings(&row.supported_by),
            };
            if !row.note.is_empty() {
                j.insert("note".into(), Value::from(row.note));
            }
            Value::Object(j)
        })
        .collect();
    dump(map! {
        "schema" => "mradm.layouts",
        "schema_version" => 1u32,
        "layouts" => Value::Array(rows),
    })
}

// ── output formats ───────────────────────────────────────────────────────────────────────────

#[derive(Clone, Debug, Default, PartialEq)]
pub struct Bitrate {
    pub per_channel: bool,
    pub min_kbps: u32,
    pub max_kbps: u32,
}

#[derive(Clone, Debug, Default, PartialEq)]
pub struct Format {
    pub format: &'static str,
    pub extensions: &'static [&'static str],
    pub available: bool,
    /// Set only when the format is unavailable.
    pub available_reason: &'static str,
    pub lossy: bool,
    pub max_channels: u32,
    pub fixed_sample_rate: u32,
    pub supports_height: bool,
    pub bit_depths: &'static [&'static str],
    pub note: &'static str,
    pub bitrate: Option<Bitrate>,
}

const IAMF_REASON: &str = "requires MR_ADM_ENABLE_IAMF=ON and the AOM iamf-tools bridge";

/// The output container formats with this build's availability.
pub fn output_formats(platform: &Platform) -> Vec<Format> {
    let apac = platform.apac;
    let iamf = platform.iamf;
    let iamf_mp4 = iamf && platform.iamf_mp4_packager;
    let reason = |available: bool, text: &'static str| if available { "" } else { text };
    vec![
        Format {
            format: "wav",
            extensions: &[".wav"],
            available: true,
            supports_height: true,
            bit_depths: &["f32", "i24", "i16"],
            note: "Uncompressed PCM; not a preferred delivery format.",
            ..Format::default()
        },
        Format {
            format: "caf",
            extensions: &[".caf"],
            available: true,
            supports_height: true,
            bit_depths: &["f32"],
            note: "Float32 CoreAudio container; richer metadata on macOS.",
            ..Format::default()
        },
        Format {
            format: "flac",
            extensions: &[".flac"],
            available: true,
            max_channels: 8,
            bit_depths: &["i24"],
            note: "1-8 channels; no height layouts; fixed 24-bit.",
            ..Format::default()
        },
        Format {
            format: "opus_mka",
            extensions: &[".mka"],
            available: true,
            lossy: true,
            max_channels: 255,
            fixed_sample_rate: 48000,
            supports_height: true,
            note: "48 kHz input required.",
            bitrate: Some(Bitrate {
                per_channel: true,
                min_kbps: 6,
                max_kbps: 320,
            }),
            ..Format::default()
        },
        Format {
            format: "apac",
            extensions: &[".m4a", ".mp4", ".caf"],
            available: apac,
            available_reason: reason(apac, "macOS only (AudioToolbox)"),
            lossy: true,
            max_channels: 24,
            fixed_sample_rate: 48000,
            supports_height: true,
            note: "macOS only; .caf requires --apac-container caf; spatial/HOA default bitrate scales \
                   from 7.1.4 = 2048 kbps.",
            bitrate: Some(Bitrate {
                per_channel: false,
                min_kbps: 64,
                max_kbps: 32768,
            }),
            ..Format::default()
        },
        Format {
            format: "iamf",
            extensions: &[".iamf"],
            available: iamf,
            available_reason: reason(iamf, IAMF_REASON),
            lossy: true,
            max_channels: 12,
            fixed_sample_rate: 48000,
            supports_height: true,
            note: "Raw OBU stream (Opus); layouts up to 7.1.4 (9.1.6 disabled).",
            ..Format::default()
        },
        Format {
            format: "iamf_mp4",
            extensions: &[".mp4"],
            available: iamf_mp4,
            available_reason: if iamf_mp4 {
                ""
            } else if !iamf {
                IAMF_REASON
            } else {
                "requires mp4box (GPAC) or ffmpeg in PATH"
            },
            lossy: true,
            max_channels: 12,
            fixed_sample_rate: 48000,
            supports_height: true,
            note: "ISOBMFF-packaged IAMF; select via --iamf-container mp4.",
            ..Format::default()
        },
    ]
}

fn features_json(platform: &Platform) -> Value {
    object! {
        "apac" => platform.apac,
        "iamf" => platform.iamf,
        "iamf_mp4_packager" => platform.iamf && platform.iamf_mp4_packager,
        "sofa" => platform.sofa,
        "hptf" => true,
    }
}

fn availability(j: &mut Map, format: &Format) {
    j.insert("available".into(), Value::from(format.available));
    if !format.available && !format.available_reason.is_empty() {
        j.insert(
            "available_reason".into(),
            Value::from(format.available_reason),
        );
    }
    j.insert("lossy".into(), Value::from(format.lossy));
    j.insert("max_channels".into(), Value::from(format.max_channels));
    j.insert(
        "fixed_sample_rate".into(),
        Value::from(format.fixed_sample_rate),
    );
    j.insert(
        "supports_height".into(),
        Value::from(format.supports_height),
    );
}

/// `mradm.output-formats` v1.
pub fn output_formats_json(platform: &Platform) -> String {
    let formats = output_formats(platform)
        .iter()
        .map(|f| {
            let mut j = map! {
                "format" => f.format,
                "extensions" => strs(f.extensions),
            };
            availability(&mut j, f);
            if !f.note.is_empty() {
                j.insert("note".into(), Value::from(f.note));
            }
            if !f.bit_depths.is_empty() {
                j.insert("bit_depths".into(), strs(f.bit_depths));
            }
            if let Some(b) = &f.bitrate {
                let key = if b.per_channel {
                    "bitrate_kbps_per_ch"
                } else {
                    "bitrate_kbps_total"
                };
                j.insert(
                    key.into(),
                    object! { "min" => b.min_kbps, "max" => b.max_kbps, "auto" => 0u32 },
                );
            }
            Value::Object(j)
        })
        .collect();
    dump(map! {
        "schema" => "mradm.output-formats",
        "schema_version" => 1u32,
        "features" => features_json(platform),
        "formats" => Value::Array(formats),
    })
}

// ── input layouts ────────────────────────────────────────────────────────────────────────────

fn channel_json(channel: &InputChannel) -> Value {
    object! {
        "token" => channel.token.as_str(),
        "speaker_label" => channel.speaker_label,
        "azimuth" => channel.azimuth,
        "elevation" => channel.elevation,
        "is_lfe" => channel.is_lfe,
    }
}

/// `mradm.input-layouts` v1 (`adm_input_layouts_json`, `mradm input-layouts --json`).
pub fn input_layouts_json() -> String {
    let layouts = input_layouts()
        .iter()
        .map(|layout| {
            let mut item = map! {
                "id" => layout.id,
                "internal_id" => layout.internal_id,
                "display_name" => layout.display_name,
                "channel_count" => layout.channels.len(),
                "channels" => Value::Array(layout.channels.iter().map(channel_json).collect()),
            };
            if layout.wave_channel_mask != 0 {
                item.insert(
                    "wave_channel_mask".into(),
                    Value::from(layout.wave_channel_mask),
                );
                let order = layout
                    .wave_mask_channels
                    .iter()
                    .map(|c| Value::from(c.token.as_str()))
                    .collect();
                item.insert("wave_mask_channel_order".into(), Value::Array(order));
            }
            Value::Object(item)
        })
        .collect();
    dump(map! {
        "schema" => "mradm.input-layouts",
        "schema_version" => 1u32,
        "max_channels" => 64u32,
        "containers" => strs(&["WAVE", "RF64", "BW64"]),
        "sample_encodings" => strs(&["pcm16", "pcm24", "pcm32", "float32"]),
        "auto_detection" => object! {
            "axml_present" => "strict ADM import; invalid ADM never falls back",
            "axml_absent" => "recognised WAVEFORMATEXTENSIBLE channel mask",
            "unrecognised_or_missing_mask" => "explicit input layout or channel labels required",
        },
        "custom_constraints" => object! {
            "label_count_must_match_file_channels" => true,
            "empty_labels_allowed" => false,
            "duplicate_canonical_labels_allowed" => false,
            "arbitrary_coordinates_allowed" => false,
            "ambiguous_labels" => object! {
                "U+110" => strs(&["U+110@30", "U+110@45"]),
                "U-110" => strs(&["U-110@30", "U-110@45"]),
            },
        },
        "two_channel_output_semantic" => "binaural",
        "coordinate_convention" => object! {
            "azimuth" => "degrees; positive left, negative right, 0 front",
            "elevation" => "degrees; positive up, 0 horizontal",
            "distance" => "normalised; 1.0 for channel-bed speakers",
        },
        "aliases" => object! {
            "L" => "M+030",
            "FL" => "M+030",
            "R" => "M-030",
            "FR" => "M-030",
            "C" => "M+000",
            "FC" => "M+000",
            "LFE" => "LFE1",
        },
        "layouts" => Value::Array(layouts),
        "custom_channel_catalog" => Value::Array(channel_catalog().iter().map(channel_json).collect()),
    })
}

// ── render support matrix ────────────────────────────────────────────────────────────────────

struct LayoutSummary {
    layout: &'static str,
    layout_id: &'static str,
    channels: u32,
    is_3d: bool,
}

struct Target {
    target: &'static str,
    format: &'static str,
    container: &'static str,
    encoding: &'static str,
    extensions: &'static [&'static str],
    layout_format: &'static str,
    required_option: Option<(&'static str, &'static str)>,
}

const TARGETS: [Target; 8] = [
    Target {
        target: "wav",
        format: "wav",
        container: "wav",
        encoding: "pcm",
        extensions: &[".wav"],
        layout_format: "wav",
        required_option: None,
    },
    Target {
        target: "caf",
        format: "caf",
        container: "caf",
        encoding: "pcm",
        extensions: &[".caf"],
        layout_format: "caf",
        required_option: None,
    },
    Target {
        target: "flac",
        format: "flac",
        container: "flac",
        encoding: "flac",
        extensions: &[".flac"],
        layout_format: "flac",
        required_option: None,
    },
    Target {
        target: "opus_mka",
        format: "opus_mka",
        container: "mka",
        encoding: "opus",
        extensions: &[".mka"],
        layout_format: "",
        required_option: None,
    },
    Target {
        target: "apac_mpeg4",
        format: "apac",
        container: "mpeg4",
        encoding: "apac",
        extensions: &[".m4a", ".mp4"],
        layout_format: "apac",
        required_option: None,
    },
    Target {
        target: "apac_caf",
        format: "apac",
        container: "caf",
        encoding: "apac",
        extensions: &[".caf"],
        layout_format: "apac",
        required_option: Some(("apac_container", "caf")),
    },
    Target {
        target: "iamf",
        format: "iamf",
        container: "iamf",
        encoding: "iamf_opus",
        extensions: &[".iamf"],
        layout_format: "iamf",
        required_option: None,
    },
    Target {
        target: "iamf_mp4",
        format: "iamf_mp4",
        container: "mp4",
        encoding: "iamf_opus",
        extensions: &[".mp4"],
        layout_format: "iamf",
        required_option: Some(("iamf_container", "mp4")),
    },
];

fn display_layout_has_height(layout: &str) -> bool {
    matches!(
        layout,
        "5.1.2" | "5.1.4" | "7.1.4" | "9.1.4" | "9.1.6" | "22.2" | "hoa3"
    )
}

fn summaries(rows: &[LayoutRow]) -> Vec<LayoutSummary> {
    let mut out: Vec<LayoutSummary> = Vec::new();
    for row in rows {
        if out.iter().any(|s| s.layout == row.layout) {
            continue;
        }
        out.push(LayoutSummary {
            layout: row.layout,
            layout_id: catalog::capability_layout_id(row.layout),
            channels: row.channels,
            is_3d: display_layout_has_height(row.layout),
        });
    }
    out
}

fn support_reason(
    backend: &Backend,
    layout: &LayoutSummary,
    target: &Target,
    format: &Format,
    rows: &[LayoutRow],
) -> String {
    if catalog::find_layout(backend, layout.layout).is_none() {
        return format!(
            "renderer '{}' does not support layout '{}'",
            backend.renderer, layout.layout
        );
    }
    if !format.available {
        return if format.available_reason.is_empty() {
            format!("format '{}' is unavailable", target.format)
        } else {
            format.available_reason.to_owned()
        };
    }
    if !target.layout_format.is_empty()
        && !rows
            .iter()
            .any(|r| r.format == target.layout_format && r.layout == layout.layout)
    {
        return format!(
            "target '{}' does not define layout '{}'",
            target.target, layout.layout
        );
    }
    if format.max_channels > 0 && layout.channels > format.max_channels {
        return format!(
            "target '{}' supports at most {} channels; layout '{}' has {} channels",
            target.target, format.max_channels, layout.layout, layout.channels
        );
    }
    if !format.supports_height && layout.is_3d {
        return format!(
            "target '{}' does not support height/3D layouts",
            target.target
        );
    }
    String::new()
}

/// `mradm.render-support-matrix` v1.
pub fn support_matrix_json(platform: &Platform) -> String {
    let backends = catalog::backends(platform);
    let rows = output_layouts(platform);
    let layouts = summaries(&rows);
    let formats = output_formats(platform);
    let find_format = |name: &str| formats.iter().find(|f| f.format == name);

    let backend_json = backends
        .iter()
        .map(|b| {
            object! {
                "renderer" => b.renderer.as_str(),
                "backend_name" => b.name.as_str(),
                "backend_version" => b.version.as_str(),
                "hrtf_sources" => strings(&b.hrtf_sources),
            }
        })
        .collect();
    let layout_json = layouts
        .iter()
        .map(|l| {
            object! {
                "layout" => l.layout,
                "layout_id" => l.layout_id,
                "channels" => l.channels,
                "is_3d" => l.is_3d,
            }
        })
        .collect();
    let mut target_json = Vec::new();
    for target in &TARGETS {
        let Some(format) = find_format(target.format) else {
            continue;
        };
        let mut j = map! {
            "target" => target.target,
            "format" => target.format,
            "container" => target.container,
            "encoding" => target.encoding,
            "extensions" => strs(target.extensions),
        };
        availability(&mut j, format);
        if let Some((name, value)) = target.required_option {
            j.insert(
                "required_option".into(),
                object! { "name" => name, "value" => value },
            );
        }
        target_json.push(Value::Object(j));
    }
    let mut entries = Vec::new();
    for backend in &backends {
        for layout in &layouts {
            for target in &TARGETS {
                let Some(format) = find_format(target.format) else {
                    continue;
                };
                let reason = support_reason(backend, layout, target, format, &rows);
                let mut j = map! {
                    "renderer" => backend.renderer.as_str(),
                    "layout" => layout.layout,
                    "layout_id" => layout.layout_id,
                    "channels" => layout.channels,
                    "is_3d" => layout.is_3d,
                    "target" => target.target,
                    "format" => target.format,
                    "container" => target.container,
                    "encoding" => target.encoding,
                    "supported" => reason.is_empty(),
                };
                if !reason.is_empty() {
                    j.insert("reason".into(), Value::from(reason));
                }
                entries.push(Value::Object(j));
            }
        }
    }
    dump(map! {
        "schema" => "mradm.render-support-matrix",
        "schema_version" => 1u32,
        "features" => features_json(platform),
        "backends" => Value::Array(backend_json),
        "layouts" => Value::Array(layout_json),
        "targets" => Value::Array(target_json),
        "entries" => Value::Array(entries),
    })
}
