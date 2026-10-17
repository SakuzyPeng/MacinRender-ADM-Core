//! Loudspeaker layouts shared by the input importer and the renderers (PCM consistency 4.3).
//! Moved verbatim from the C++ `render_layouts` table: every coordinate is the same f32 literal.
use std::sync::OnceLock;

#[derive(Clone, Copy, Debug, PartialEq)]
pub struct Speaker {
    pub label: &'static str,
    pub azimuth: f32,
    pub elevation: f32,
    pub is_lfe: bool,
    pub azimuth_range: Option<[f32; 2]>,
    pub elevation_range: Option<[f32; 2]>,
}

#[derive(Clone, Debug, PartialEq)]
pub struct Layout {
    pub id: &'static str,
    pub display_name: &'static str,
    pub speakers: Vec<Speaker>,
}

#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub enum Geometry {
    Standard,
    Apple,
}

const fn sp(label: &'static str, azimuth: f32, elevation: f32) -> Speaker {
    Speaker {
        label,
        azimuth,
        elevation,
        is_lfe: false,
        azimuth_range: None,
        elevation_range: None,
    }
}

const fn lfe(label: &'static str, azimuth: f32, elevation: f32) -> Speaker {
    Speaker {
        is_lfe: true,
        ..sp(label, azimuth, elevation)
    }
}

/// A speaker with its BS.2051 installation ranges.
const fn ranged(speaker: Speaker, azimuth: [f32; 2], elevation: [f32; 2]) -> Speaker {
    Speaker {
        azimuth_range: Some(azimuth),
        elevation_range: Some(elevation),
        ..speaker
    }
}

const LFE1_RANGED: Speaker = ranged(lfe("LFE1", 45.0, -30.0), [-180.0, 180.0], [-90.0, 90.0]);

fn standard_layouts() -> Vec<Layout> {
    let layout = |id, display_name, speakers: &[Speaker]| Layout {
        id,
        display_name,
        speakers: speakers.to_vec(),
    };
    vec![
        layout(
            "0+2+0",
            "Stereo",
            &[
                ranged(sp("M+030", 30.0, 0.0), [30.0, 30.0], [0.0, 0.0]),
                ranged(sp("M-030", -30.0, 0.0), [-30.0, -30.0], [0.0, 0.0]),
            ],
        ),
        layout(
            "0+5+0",
            "5.1",
            &[
                ranged(sp("M+030", 30.0, 0.0), [30.0, 30.0], [0.0, 0.0]),
                ranged(sp("M-030", -30.0, 0.0), [-30.0, -30.0], [0.0, 0.0]),
                ranged(sp("M+000", 0.0, 0.0), [0.0, 0.0], [0.0, 0.0]),
                LFE1_RANGED,
                ranged(sp("M+110", 110.0, 0.0), [100.0, 120.0], [0.0, 15.0]),
                ranged(sp("M-110", -110.0, 0.0), [-120.0, -100.0], [0.0, 15.0]),
            ],
        ),
        layout(
            "2+5+0",
            "5.1.2",
            &[
                ranged(sp("M+030", 30.0, 0.0), [30.0, 30.0], [0.0, 0.0]),
                ranged(sp("M-030", -30.0, 0.0), [-30.0, -30.0], [0.0, 0.0]),
                ranged(sp("M+000", 0.0, 0.0), [0.0, 0.0], [0.0, 0.0]),
                LFE1_RANGED,
                ranged(sp("M+110", 110.0, 0.0), [100.0, 120.0], [0.0, 15.0]),
                ranged(sp("M-110", -110.0, 0.0), [-120.0, -100.0], [0.0, 15.0]),
                ranged(sp("U+030", 30.0, 30.0), [30.0, 45.0], [30.0, 55.0]),
                ranged(sp("U-030", -30.0, 30.0), [-45.0, -30.0], [30.0, 55.0]),
            ],
        ),
        layout(
            "4+5+0",
            "5.1.4",
            &[
                ranged(sp("M+030", 30.0, 0.0), [30.0, 30.0], [0.0, 0.0]),
                ranged(sp("M-030", -30.0, 0.0), [-30.0, -30.0], [0.0, 0.0]),
                ranged(sp("M+000", 0.0, 0.0), [0.0, 0.0], [0.0, 0.0]),
                LFE1_RANGED,
                ranged(sp("M+110", 110.0, 0.0), [100.0, 120.0], [0.0, 0.0]),
                ranged(sp("M-110", -110.0, 0.0), [-120.0, -100.0], [0.0, 0.0]),
                ranged(sp("U+030", 30.0, 30.0), [30.0, 45.0], [30.0, 55.0]),
                ranged(sp("U-030", -30.0, 30.0), [-45.0, -30.0], [30.0, 55.0]),
                ranged(sp("U+110", 110.0, 30.0), [100.0, 135.0], [30.0, 55.0]),
                ranged(sp("U-110", -110.0, 30.0), [-135.0, -100.0], [30.0, 55.0]),
            ],
        ),
        layout(
            "wav71",
            "7.1",
            &[
                sp("M+030", 30.0, 0.0),
                sp("M-030", -30.0, 0.0),
                sp("M+000", 0.0, 0.0),
                lfe("LFE1", 45.0, -30.0),
                sp("M+135", 135.0, 0.0),
                sp("M-135", -135.0, 0.0),
                sp("M+090", 90.0, 0.0),
                sp("M-090", -90.0, 0.0),
            ],
        ),
        layout(
            "4+7+0",
            "7.1.4",
            &[
                ranged(sp("M+030", 30.0, 0.0), [30.0, 45.0], [0.0, 0.0]),
                ranged(sp("M-030", -30.0, 0.0), [-45.0, -30.0], [0.0, 0.0]),
                ranged(sp("M+000", 0.0, 0.0), [0.0, 0.0], [0.0, 0.0]),
                LFE1_RANGED,
                ranged(sp("M+090", 90.0, 0.0), [85.0, 110.0], [0.0, 0.0]),
                ranged(sp("M-090", -90.0, 0.0), [-110.0, -85.0], [0.0, 0.0]),
                ranged(sp("M+135", 135.0, 0.0), [120.0, 150.0], [0.0, 0.0]),
                ranged(sp("M-135", -135.0, 0.0), [-150.0, -120.0], [0.0, 0.0]),
                ranged(sp("U+045", 45.0, 30.0), [30.0, 45.0], [30.0, 55.0]),
                ranged(sp("U-045", -45.0, 30.0), [-45.0, -30.0], [30.0, 55.0]),
                ranged(sp("U+135", 135.0, 30.0), [100.0, 150.0], [30.0, 55.0]),
                ranged(sp("U-135", -135.0, 30.0), [-150.0, -100.0], [30.0, 55.0]),
            ],
        ),
        layout(
            "4+5+4",
            "9.1.4",
            &[
                sp("M+030", 30.0, 0.0),
                sp("M-030", -30.0, 0.0),
                sp("M+000", 0.0, 0.0),
                lfe("LFE1", 45.0, -30.0),
                sp("M+110", 110.0, 0.0),
                sp("M-110", -110.0, 0.0),
                sp("M+150", 150.0, 0.0),
                sp("M-150", -150.0, 0.0),
                sp("M+070", 70.0, 0.0),
                sp("M-070", -70.0, 0.0),
                sp("U+070", 70.0, 45.0),
                sp("U-070", -70.0, 45.0),
                sp("U+150", 150.0, 45.0),
                sp("U-150", -150.0, 45.0),
            ],
        ),
        layout(
            "9.1.6",
            "9.1.6 (Dolby Atmos)",
            &[
                sp("M+030", 30.0, 0.0),
                sp("M-030", -30.0, 0.0),
                sp("M+000", 0.0, 0.0),
                lfe("LFE1", 45.0, -30.0),
                sp("M+110", 110.0, 0.0),
                sp("M-110", -110.0, 0.0),
                sp("M+150", 150.0, 0.0),
                sp("M-150", -150.0, 0.0),
                sp("M+070", 70.0, 0.0),
                sp("M-070", -70.0, 0.0),
                sp("U+070", 70.0, 45.0),
                sp("U-070", -70.0, 45.0),
                sp("U+110", 110.0, 45.0),
                sp("U-110", -110.0, 45.0),
                sp("U+150", 150.0, 45.0),
                sp("U-150", -150.0, 45.0),
            ],
        ),
        layout(
            "9+10+3",
            "22.2",
            &[
                sp("M+060", 60.0, 0.0),
                sp("M-060", -60.0, 0.0),
                sp("M+000", 0.0, 0.0),
                lfe("LFE1", 45.0, -30.0),
                sp("M+135", 135.0, 0.0),
                sp("M-135", -135.0, 0.0),
                sp("M+030", 30.0, 0.0),
                sp("M-030", -30.0, 0.0),
                sp("M+180", 180.0, 0.0),
                lfe("LFE2", -45.0, -30.0),
                sp("M+090", 90.0, 0.0),
                sp("M-090", -90.0, 0.0),
                sp("U+045", 45.0, 30.0),
                sp("U-045", -45.0, 30.0),
                sp("U+000", 0.0, 30.0),
                sp("T+000", 0.0, 90.0),
                sp("U+135", 135.0, 30.0),
                sp("U-135", -135.0, 30.0),
                sp("U+090", 90.0, 30.0),
                sp("U-090", -90.0, 30.0),
                sp("U+180", 180.0, 30.0),
                sp("B+000", 0.0, -30.0),
                sp("B+045", 45.0, -30.0),
                sp("B-045", -45.0, -30.0),
            ],
        ),
    ]
}

// Captured on macOS 27.0 (26A5406e) with AudioFormatGetProperty using
// kAudioFormatProperty_ChannelLayoutForTag. CoreAudio expands these standard layout tags to fixed
// spherical coordinates: MPEG_5_1_A, WAVE_7_1, Atmos_5_1_2, Atmos_5_1_4, Atmos_7_1_4, Atmos_9_1_6
// and CICP_13. Coordinates use the project's ADM convention (+azimuth = left), the inverse of
// CoreAudio's +right convention. LFE coordinates are deliberately left as project placeholders
// because LFE never participates in object panning.
const APPLE_LAYOUT_IDS: [&str; 7] = [
    "0+5+0", "wav71", "2+5+0", "4+5+0", "4+7+0", "9.1.6", "9+10+3",
];

#[rustfmt::skip]
const APPLE_POSITION_OVERRIDES: [(&str, &str, f32, f32); 38] = [
    ("wav71",  "M+135",  150.0,   0.0), ("wav71",  "M-135", -150.0,   0.0),
    ("wav71",  "M+090",  110.0,   0.0), ("wav71",  "M-090", -110.0,   0.0),

    ("2+5+0",  "U+030",   90.0,  45.0), ("2+5+0",  "U-030",  -90.0,  45.0),

    ("4+5+0",  "U+030",   45.0,  45.0), ("4+5+0",  "U-030",  -45.0,  45.0),
    ("4+5+0",  "U+110",  135.0,  45.0), ("4+5+0",  "U-110", -135.0,  45.0),

    ("4+7+0",  "M+090",  110.0,   0.0), ("4+7+0",  "M-090", -110.0,   0.0),
    ("4+7+0",  "M+135",  150.0,   0.0), ("4+7+0",  "M-135", -150.0,   0.0),
    ("4+7+0",  "U+045",   45.0,  45.0), ("4+7+0",  "U-045",  -45.0,  45.0),
    ("4+7+0",  "U+135",  135.0,  45.0), ("4+7+0",  "U-135", -135.0,  45.0),

    ("9.1.6",  "M+070",   60.0,   0.0), ("9.1.6",  "M-070",  -60.0,   0.0),
    ("9.1.6",  "U+070",   45.0,  45.0), ("9.1.6",  "U-070",  -45.0,  45.0),
    ("9.1.6",  "U+110",   90.0,  45.0), ("9.1.6",  "U-110",  -90.0,  45.0),
    ("9.1.6",  "U+150",  135.0,  45.0), ("9.1.6",  "U-150", -135.0,  45.0),

    ("9+10+3", "M+135",  150.0,   0.0), ("9+10+3", "M-135", -150.0,   0.0),
    ("9+10+3", "U+045",   45.0,  45.0), ("9+10+3", "U-045",  -45.0,  45.0),
    ("9+10+3", "U+135",  135.0,  45.0), ("9+10+3", "U-135", -135.0,  45.0),
    ("9+10+3", "U+090",   90.0,  45.0), ("9+10+3", "U-090",  -90.0,  45.0),
    ("9+10+3", "U+180",  180.0,  45.0),
    ("9+10+3", "B+000",    0.0, -15.0), ("9+10+3", "B+045",   45.0, -15.0),
    ("9+10+3", "B-045",  -45.0, -15.0),
];

fn apple_layouts(standard: &[Layout]) -> Vec<Layout> {
    let mut result: Vec<Layout> = APPLE_LAYOUT_IDS
        .iter()
        .map(|id| {
            standard
                .iter()
                .find(|layout| layout.id == *id)
                .expect("Apple layout has a standard counterpart")
                .clone()
        })
        .collect();
    for (layout_id, label, azimuth, elevation) in APPLE_POSITION_OVERRIDES {
        let speaker = result
            .iter_mut()
            .find(|layout| layout.id == layout_id)
            .and_then(|layout| layout.speakers.iter_mut().find(|s| s.label == label))
            .expect("Apple override names an existing speaker");
        speaker.azimuth = azimuth;
        speaker.elevation = elevation;
    }
    // This profile describes CoreAudio's fixed effective positions, not the installation ranges
    // carried by the ADM nominal layout.
    for speaker in result
        .iter_mut()
        .flat_map(|layout| layout.speakers.iter_mut())
    {
        if !speaker.is_lfe {
            speaker.azimuth_range = Some([speaker.azimuth, speaker.azimuth]);
            speaker.elevation_range = Some([speaker.elevation, speaker.elevation]);
        }
    }
    result
}

struct Tables {
    standard: Vec<Layout>,
    apple: Vec<Layout>,
}

fn tables() -> &'static Tables {
    static TABLES: OnceLock<Tables> = OnceLock::new();
    TABLES.get_or_init(|| {
        let standard = standard_layouts();
        let apple = apple_layouts(&standard);
        Tables { standard, apple }
    })
}

pub fn layouts(geometry: Geometry) -> &'static [Layout] {
    match geometry {
        Geometry::Standard => &tables().standard,
        Geometry::Apple => &tables().apple,
    }
}

pub fn find(id: &str, geometry: Geometry) -> Option<&'static Layout> {
    layouts(geometry).iter().find(|layout| layout.id == id)
}
