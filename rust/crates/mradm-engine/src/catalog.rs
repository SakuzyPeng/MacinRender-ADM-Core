//! The backend capability reports (PCM consistency 5.2 A), moved from each C++ adapter's
//! `*_capabilities()` with the same values and layout order. The Apple AUSpatialMixer report is
//! an island fact and arrives through [`Platform`].
use mradm_scene::layouts::{self, Geometry, Speaker};

#[derive(Clone, Debug, Default, PartialEq)]
pub struct Layout {
    pub id: String,
    pub display_name: String,
    /// Total output channels, including LFE.
    pub channel_count: u16,
    /// Any non-LFE speaker has a non-zero elevation.
    pub is_3d: bool,
    pub lfe_count: u16,
    pub supports_spread: bool,
    /// HRTF convolution is applied.
    pub is_binaural: bool,
}

#[derive(Clone, Debug, Default, PartialEq)]
pub struct Mode {
    pub id: String,
    pub display_name: String,
    pub layouts: Vec<String>,
    pub sample_rates: Vec<u32>,
    pub supports_realtime: bool,
}

#[derive(Clone, Debug, Default, PartialEq)]
pub struct Backend {
    /// The renderer selection string a caller passes (`ear`, `saf`, ...).
    pub renderer: String,
    pub name: String,
    pub version: String,
    pub layouts: Vec<Layout>,
    pub modes: Vec<Mode>,
    /// Optional processing qualities; the first is the default.
    pub qualities: Vec<Mode>,
    pub supports_objects: bool,
    pub supports_direct_speakers: bool,
    pub supports_hoa: bool,
    pub supports_channel_lock: bool,
    pub supports_object_divergence: bool,
    pub supports_screen_ref: bool,
    pub supports_diffuse: bool,
    /// Stable values: "built-in", "user-sofa", "system", ...; empty for loudspeaker backends.
    pub hrtf_sources: Vec<String>,
    /// Internal hint, not in the capabilities JSON: the backend renders a sub-window directly.
    pub supports_render_window: bool,
}

/// A system-spatial monitor sink layout (macOS AVSampleBufferAudioRenderer, Windows
/// ISpatialAudioClient).
#[derive(Clone, Debug, Default, PartialEq)]
pub struct SpatialLayout {
    pub id: String,
    pub display_name: String,
    pub channel_count: u16,
}

/// Build and platform facts, supplied by the caller.
#[derive(Clone, Debug, Default, PartialEq)]
pub struct Platform {
    /// User SOFA HRIRs (`MR_ADM_ENABLE_SOFA`).
    pub sofa: bool,
    /// APAC encoding (macOS AudioToolbox).
    pub apac: bool,
    /// IAMF encoding (`MR_ADM_ENABLE_IAMF` with the AOM bridge).
    pub iamf: bool,
    /// An MP4 packager in PATH; only meaningful when `iamf` is set.
    pub iamf_mp4_packager: bool,
    /// The Apple AUSpatialMixer backend (macOS only).
    pub apple: Option<Backend>,
    pub system_spatial_layouts: Vec<SpatialLayout>,
    /// The macOS AUSpatialMixer LFE routing self-test; absent elsewhere.
    pub apple_system_spatial_lfe_routing_ok: Option<bool>,
}

fn layout(
    id: &str,
    display_name: &str,
    channel_count: u16,
    is_3d: bool,
    lfe_count: u16,
    supports_spread: bool,
    is_binaural: bool,
) -> Layout {
    Layout {
        id: id.to_owned(),
        display_name: display_name.to_owned(),
        channel_count,
        is_3d,
        lfe_count,
        supports_spread,
        is_binaural,
    }
}

fn mode(id: &str, display_name: &str, layouts: &[&str], rates: &[u32], realtime: bool) -> Mode {
    Mode {
        id: id.to_owned(),
        display_name: display_name.to_owned(),
        layouts: layouts.iter().map(|s| (*s).to_owned()).collect(),
        sample_rates: rates.to_vec(),
        supports_realtime: realtime,
    }
}

fn strings(values: &[&str]) -> Vec<String> {
    values.iter().map(|s| (*s).to_owned()).collect()
}

/// A loudspeaker table entry as a capability layout; `spread` decides `supports_spread`.
fn speaker_layout(
    id: &str,
    name: &str,
    speakers: &[Speaker],
    spread: impl Fn(bool) -> bool,
) -> Layout {
    let is_3d = speakers
        .iter()
        .any(|s| !s.is_lfe && s.elevation.abs() > 1.0e-6);
    Layout {
        id: id.to_owned(),
        display_name: name.to_owned(),
        channel_count: speakers.len() as u16,
        is_3d,
        lfe_count: speakers.iter().filter(|s| s.is_lfe).count() as u16,
        supports_spread: spread(is_3d),
        is_binaural: false,
    }
}

pub fn ear() -> Backend {
    Backend {
        renderer: "ear".into(),
        name: "libear".into(),
        version: "rust-ear-0.1.0".into(),
        supports_objects: true,
        supports_direct_speakers: true,
        supports_hoa: true,
        supports_channel_lock: true,
        supports_object_divergence: true,
        supports_diffuse: true,
        supports_render_window: true,
        layouts: layouts::layouts(Geometry::Standard)
            .iter()
            .map(|l| speaker_layout(l.id, l.display_name, &l.speakers, |_| true))
            .collect(),
        ..Backend::default()
    }
}

/// The `saf` selection: VBAP / MDAP; 2D layouts render without spread.
pub fn vbap() -> Backend {
    Backend {
        renderer: "saf".into(),
        name: "saf-vbap".into(),
        version: "1.3.4".into(),
        supports_objects: true,
        supports_direct_speakers: true,
        supports_channel_lock: true,
        supports_object_divergence: true,
        supports_render_window: true,
        layouts: layouts::layouts(Geometry::Standard)
            .iter()
            .map(|l| speaker_layout(l.id, l.display_name, &l.speakers, |is_3d| is_3d))
            .collect(),
        ..Backend::default()
    }
}

pub fn triple_balance() -> Backend {
    Backend {
        renderer: "triple-balance".into(),
        name: "triple-balance".into(),
        version: "1".into(),
        supports_objects: true,
        supports_direct_speakers: true,
        supports_render_window: true,
        modes: vec![
            mode(
                "standard",
                "Standard",
                &["4+7+0", "9.1.6", "9+10+3"],
                &[],
                true,
            ),
            mode("d", "D mode", &["9.1.6", "9+10+3"], &[48000], false),
        ],
        layouts: vec![
            layout("4+7+0", "7.1.4", 12, true, 1, true, false),
            layout("9.1.6", "9.1.6", 16, true, 1, true, false),
            layout("9+10+3", "22.2 (experimental)", 24, true, 2, true, false),
        ],
        ..Backend::default()
    }
}

pub fn hoa() -> Backend {
    Backend {
        renderer: "hoa".into(),
        name: "hoa-encode".into(),
        version: "1.0".into(),
        supports_objects: true,
        supports_direct_speakers: true,
        supports_object_divergence: true,
        supports_diffuse: true,
        supports_render_window: true,
        layouts: vec![layout(
            "hoa3",
            "HOA 3rd Order (16ch, ACN/SN3D)",
            16,
            true,
            0,
            false,
            false,
        )],
        ..Backend::default()
    }
}

/// The `saf-binaural` selection.
pub fn binaural(platform: &Platform) -> Backend {
    let mut hrtf_sources = strings(&["built-in"]);
    if platform.sofa {
        hrtf_sources.push("user-sofa".into());
    }
    Backend {
        renderer: "saf-binaural".into(),
        name: "saf-binaural-hrtf".into(),
        version: "1.0".into(),
        supports_objects: true,
        supports_direct_speakers: true,
        supports_channel_lock: true,
        supports_object_divergence: true,
        supports_diffuse: true,
        hrtf_sources,
        supports_render_window: true,
        layouts: vec![layout(
            "binaural",
            "SAF HRTF binaural (KEMAR or user SOFA HRIR)",
            2,
            false,
            0,
            true,
            true,
        )],
        ..Backend::default()
    }
}

pub fn room() -> Backend {
    let rates = [48000, 96000];
    Backend {
        renderer: "room-binaural".into(),
        name: "room-binaural".into(),
        version: "1".into(),
        supports_objects: true,
        supports_direct_speakers: true,
        supports_channel_lock: true,
        supports_object_divergence: true,
        supports_diffuse: true,
        supports_screen_ref: true,
        supports_render_window: true,
        hrtf_sources: strings(&["user-sofa", "prepared-hrir", "hrtf-collection"]),
        layouts: vec![layout("binaural", "Room binaural", 2, true, 0, true, true)],
        modes: vec![mode(
            "hybrid-shoebox-v1",
            "Finite hybrid room",
            &["binaural"],
            &rates,
            true,
        )],
        qualities: vec![
            mode("exact", "精确", &["binaural"], &rates, true),
            mode("realtime", "实时", &["binaural"], &rates, true),
        ],
        ..Backend::default()
    }
}

/// Every backend in discovery order: the six Rust backends, then Apple where available.
pub fn backends(platform: &Platform) -> Vec<Backend> {
    let mut out = vec![
        ear(),
        vbap(),
        triple_balance(),
        hoa(),
        binaural(platform),
        room(),
    ];
    if let Some(apple) = &platform.apple {
        out.push(apple.clone());
    }
    out
}

/// The capability layout id for a display layout name (`5.1` → `0+5+0`, ...).
pub fn capability_layout_id(layout: &str) -> &str {
    match layout {
        "5.1" => "0+5+0",
        "5.1.2" => "2+5+0",
        "7.1" => "wav71",
        "5.1.4" => "4+5+0",
        "9.1.4" => "4+5+4",
        "7.1.4" => "4+7+0",
        "22.2" => "9+10+3",
        other => other,
    }
}

/// The backend layout serving a display layout name; `binaural` matches any binaural layout.
pub fn find_layout<'a>(backend: &'a Backend, display_layout: &str) -> Option<&'a Layout> {
    let id = capability_layout_id(display_layout);
    backend
        .layouts
        .iter()
        .find(|l| l.id == id || (display_layout == "binaural" && l.is_binaural))
}
