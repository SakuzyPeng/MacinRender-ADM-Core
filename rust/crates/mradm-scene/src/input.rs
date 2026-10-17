//! Channel-bed input presets, the custom-label catalog and label resolution (PCM consistency 4.3),
//! moved from the C++ channel-bed importer. Label and layout matching is ASCII-only.
use crate::error::{Context, Error, Result};
use crate::layouts::{Geometry, find, layouts};
use std::collections::BTreeSet;
use std::sync::OnceLock;

pub const MAX_INPUT_CHANNELS: usize = 64;

#[derive(Clone, Debug, PartialEq)]
pub struct InputChannel {
    pub token: String,
    pub speaker_label: &'static str,
    pub azimuth: f32,
    pub elevation: f32,
    pub is_lfe: bool,
}

#[derive(Clone, Debug, PartialEq)]
pub struct InputLayout {
    pub id: &'static str,
    pub internal_id: &'static str,
    pub display_name: &'static str,
    /// 0 = no automatic WAVE mask mapping.
    pub wave_channel_mask: u32,
    /// Exact file order for an explicit preset.
    pub channels: Vec<InputChannel>,
    /// Ascending mask-bit order, for presets with a WAVE mask.
    pub wave_mask_channels: Vec<InputChannel>,
}

const PRESETS: [(&str, &str, u32); 8] = [
    ("5.1", "0+5+0", 0x003F),
    ("5.1.2", "2+5+0", 0x503F),
    ("7.1", "wav71", 0x063F),
    ("5.1.4", "4+5+0", 0x2D03F),
    ("7.1.4", "4+7+0", 0x2D63F),
    ("9.1.4", "4+5+4", 0),
    ("9.1.6", "9.1.6", 0),
    ("22.2", "9+10+3", 0),
];

pub(crate) fn lower(value: &str) -> String {
    value.to_ascii_lowercase()
}

fn trim_upper(value: &str) -> String {
    value
        .trim_matches([' ', '\t', '\r', '\n'])
        .to_ascii_uppercase()
}

pub fn input_layouts() -> &'static [InputLayout] {
    static LAYOUTS: OnceLock<Vec<InputLayout>> = OnceLock::new();
    LAYOUTS.get_or_init(|| {
        PRESETS
            .iter()
            .filter_map(|&(id, internal_id, wave_channel_mask)| {
                let source = find(internal_id, Geometry::Standard)?;
                let mut channels: Vec<InputChannel> = source
                    .speakers
                    .iter()
                    .map(|s| InputChannel {
                        token: s.label.to_owned(),
                        speaker_label: s.label,
                        azimuth: s.azimuth,
                        elevation: s.elevation,
                        is_lfe: s.is_lfe,
                    })
                    .collect();
                if id == "7.1.4" {
                    // Explicit presets describe file order. WAVEFORMATEXTENSIBLE orders channels by
                    // ascending mask bit, so back L/R (0x10/0x20) precede side L/R (0x200/0x400).
                    channels.swap(4, 6);
                    channels.swap(5, 7);
                }
                let wave_mask_channels = if wave_channel_mask != 0 {
                    channels.clone()
                } else {
                    Vec::new()
                };
                Some(InputLayout {
                    id,
                    internal_id,
                    display_name: source.display_name,
                    wave_channel_mask,
                    channels,
                    wave_mask_channels,
                })
            })
            .collect()
    })
}

fn same_position(lhs: &InputChannel, rhs: &InputChannel) -> bool {
    if lhs.speaker_label != rhs.speaker_label || lhs.is_lfe != rhs.is_lfe {
        return false;
    }
    lhs.is_lfe
        || ((lhs.azimuth - rhs.azimuth).abs() < 1.0e-4
            && (lhs.elevation - rhs.elevation).abs() < 1.0e-4)
}

/// Every distinct speaker of the standard layouts except stereo. A label that appears at more
/// than one nominal elevation gets an unambiguous `label@elevation` token.
pub fn channel_catalog() -> &'static [InputChannel] {
    static CATALOG: OnceLock<Vec<InputChannel>> = OnceLock::new();
    CATALOG.get_or_init(|| {
        let mut catalog: Vec<InputChannel> = Vec::new();
        for layout in layouts(Geometry::Standard) {
            if layout.id == "0+2+0" {
                continue;
            }
            for speaker in &layout.speakers {
                let candidate = InputChannel {
                    token: speaker.label.to_owned(),
                    speaker_label: speaker.label,
                    azimuth: speaker.azimuth,
                    elevation: speaker.elevation,
                    is_lfe: speaker.is_lfe,
                };
                if !catalog.iter().any(|item| same_position(item, &candidate)) {
                    catalog.push(candidate);
                }
            }
        }
        let tokens: Vec<Option<String>> = catalog
            .iter()
            .map(|item| {
                let count = catalog
                    .iter()
                    .filter(|other| other.speaker_label == item.speaker_label)
                    .count();
                (count > 1 && !item.is_lfe)
                    .then(|| format!("{}@{:.0}", item.speaker_label, item.elevation))
            })
            .collect();
        for (item, token) in catalog.iter_mut().zip(tokens) {
            if let Some(token) = token {
                item.token = token;
            }
        }
        catalog
    })
}

pub fn find_input_layout(raw: &str) -> Option<&'static InputLayout> {
    let key = lower(raw);
    input_layouts()
        .iter()
        .find(|layout| key == lower(layout.id) || key == lower(layout.internal_id))
}

/// Resolves custom labels in file-channel order to catalog indices.
pub fn resolve_input_channel_labels(labels: &[String]) -> Result<Vec<usize>> {
    if labels.is_empty() || labels.len() > MAX_INPUT_CHANNELS {
        return Err(Error::invalid(
            format!("custom input mapping requires 1-{MAX_INPUT_CHANNELS} labels"),
            Context::None,
        ));
    }
    let catalog = channel_catalog();
    let mut used = BTreeSet::new();
    let mut out = Vec::with_capacity(labels.len());
    for raw in labels {
        let mut token = trim_upper(raw);
        if token.is_empty() {
            return Err(Error::invalid(
                "custom input mapping contains an empty label",
                Context::None,
            ));
        }
        let alias = match token.as_str() {
            "L" | "FL" => Some("M+030"),
            "R" | "FR" => Some("M-030"),
            "C" | "FC" => Some("M+000"),
            "LFE" => Some("LFE1"),
            _ => None,
        };
        if let Some(alias) = alias {
            token = alias.to_owned();
        }
        let candidates: Vec<usize> = (0..catalog.len())
            .filter(|&i| catalog[i].token == token || catalog[i].speaker_label == token)
            .collect();
        let index = match candidates.as_slice() {
            [] => {
                return Err(Error::invalid(
                    format!(
                        "unknown input channel label '{raw}'; use mradm input-layouts for the catalog"
                    ),
                    Context::None,
                ));
            }
            [index] => *index,
            _ => {
                return Err(Error::invalid(
                    format!(
                        "input channel label '{token}' has multiple nominal elevations; use '{token}@30' or \
                         '{token}@45'"
                    ),
                    Context::None,
                ));
            }
        };
        if !used.insert(catalog[index].speaker_label) {
            return Err(Error::invalid(
                format!(
                    "duplicate input speaker label '{}'",
                    catalog[index].speaker_label
                ),
                Context::None,
            ));
        }
        out.push(index);
    }
    Ok(out)
}
