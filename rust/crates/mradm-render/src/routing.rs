//! DirectSpeakers, speaker-label and LFE routing shared by every loudspeaker backend and the
//! Apple island (PCM consistency 4.5). Moved from the C++ `render_common` with the same alias
//! table, matching order, messages and gains. Labels are protocol bytes: case folding and
//! character classes are ASCII only, and non-UTF-8 input is compared and reported unchanged.
use mradm_dsp::stream_dsp::live_gain;

/// Non-standard RoomCentric and DAW shorthand labels found in ADM BWF exports, by canonical key.
const ALIASES: [(&[u8], &[u8]); 34] = [
    (b"RCL", b"M+030"),
    (b"RCR", b"M-030"),
    (b"RCC", b"M+000"),
    (b"RCLFE", b"LFE1"),
    (b"RCLSS", b"M+090"),
    (b"RCRSS", b"M-090"),
    (b"RCLRS", b"M+135"),
    (b"RCRRS", b"M-135"),
    (b"RCLTS", b"U+090"),
    (b"RCRTS", b"U-090"),
    (b"L", b"M+030"),
    (b"R", b"M-030"),
    (b"C", b"M+000"),
    (b"LFE", b"LFE1"),
    (b"LFEL", b"LFE1"),
    (b"LFER", b"LFE2"),
    (b"LS", b"M+090"),
    (b"RS", b"M-090"),
    (b"LSS", b"M+090"),
    (b"RSS", b"M-090"),
    (b"LRS", b"M+135"),
    (b"RRS", b"M-135"),
    (b"LB", b"M+135"),
    (b"RB", b"M-135"),
    (b"LW", b"M+060"),
    (b"RW", b"M-060"),
    (b"CS", b"M+180"),
    (b"VHL", b"U+045"),
    (b"VHR", b"U-045"),
    (b"VHC", b"U+000"),
    (b"TSL", b"U+090"),
    (b"TSR", b"U-090"),
    (b"LTM", b"U+090"),
    (b"RTM", b"U-090"),
];

fn alias(key: &[u8]) -> Option<&'static [u8]> {
    ALIASES
        .iter()
        .find(|(from, _)| *from == key)
        .map(|(_, to)| *to)
}

/// Uppercased alphanumerics only, for LFE keyword detection (`LFE`, `LFE1`, `SUB` ...).
pub fn label_key(raw: &[u8]) -> Vec<u8> {
    raw.iter()
        .filter(|c| c.is_ascii_alphanumeric())
        .map(u8::to_ascii_uppercase)
        .collect()
}

/// Uppercased alphanumerics, `+` and `-`: the sign survives, so `M+030` and `M-030` differ while
/// `m+030` and `M_+030` match. Used for per-channel live overrides.
fn canonical_bytes(raw: &[u8]) -> impl Iterator<Item = u8> {
    raw.iter()
        .filter(|&&c| c.is_ascii_alphanumeric() || c == b'+' || c == b'-')
        .map(u8::to_ascii_uppercase)
}

pub fn canonical_label(raw: &[u8]) -> Vec<u8> {
    canonical_bytes(raw).collect()
}

/// The canonical label with the known aliases mapped to their BS.2051 label (`L` -> `M+030`).
pub fn direct_label(raw: &[u8]) -> Vec<u8> {
    let key = canonical_label(raw);
    alias(&key).map_or(key, <[u8]>::to_vec)
}

/// One output slot of a layout as the routing sees it.
#[derive(Clone, Copy, Debug)]
pub struct Target<'a> {
    pub label: &'a [u8],
    pub is_lfe: bool,
}

/// The output for a DirectSpeakers block's labels: an exact label match first, then the alias
/// table.
pub fn target_index(targets: &[Target], labels: &[&[u8]]) -> Option<usize> {
    let find = |wanted: &[u8]| {
        targets
            .iter()
            .position(|t| !t.label.is_empty() && t.label == wanted)
    };
    labels.iter().find_map(|l| find(l)).or_else(|| {
        labels
            .iter()
            .find_map(|l| alias(&canonical_label(l)).and_then(find))
    })
}

/// `std::from_chars` on the three characters after the sign: an optional `-`, then digits, all
/// consumed.
fn magnitude(digits: &[u8]) -> Option<i32> {
    let (negative, rest) = match digits.split_first() {
        Some((b'-', rest)) => (true, rest),
        _ => (false, digits),
    };
    if rest.is_empty() || !rest.iter().all(u8::is_ascii_digit) {
        return None;
    }
    let value = rest.iter().fold(0i32, |v, d| v * 10 + i32::from(d - b'0'));
    Some(if negative { -value } else { value })
}

/// The nominal direction of a BS.2051 label: layers M/U/B at 0/+30/-30 degrees, and T+000.
fn bs2051_position(label: &[u8]) -> Option<[f32; 2]> {
    if label.len() != 5 || (label[1] != b'+' && label[1] != b'-') {
        return None;
    }
    let magnitude = magnitude(&label[2..]).filter(|&m| m <= 180)?;
    let azimuth = (if label[1] == b'+' {
        magnitude
    } else {
        -magnitude
    }) as f32;
    match label[0] {
        b'M' => Some([azimuth, 0.]),
        b'U' => Some([azimuth, 30.]),
        b'B' => Some([azimuth, -30.]),
        b'T' if magnitude == 0 => Some([0., 90.]),
        _ => None,
    }
}

/// A direction from the first label that is, or aliases, a BS.2051 label; used when the label is
/// not in the output layout.
pub fn label_position(labels: &[&[u8]]) -> Option<[f32; 2]> {
    labels.iter().find_map(|label| {
        let key = canonical_label(label);
        bs2051_position(alias(&key).unwrap_or(&key))
    })
}

pub fn is_lfe(raw: &[u8]) -> bool {
    let key = label_key(raw);
    let contains = |needle: &[u8]| key.windows(needle.len()).any(|w| w == needle);
    key == b"LF" || contains(b"LFE") || contains(b"SUB") || contains(b"LOWFREQUENCY")
}

/// The LFE a DirectSpeakers block feeds; `#[repr(u32)]` values match the C++ `LfeTarget`.
#[derive(Clone, Copy, Debug, PartialEq, Eq)]
#[repr(u32)]
pub enum LfeTarget {
    None = 0,
    Lfe1 = 1,
    Lfe2 = 2,
}

/// LFE2 / LFER win over a generic LFE label; any other LFE label, or a lowPass channel frequency,
/// is the first LFE.
pub fn lfe_target(labels: &[&[u8]], low_pass: bool) -> LfeTarget {
    if labels.iter().any(|l| {
        let key = label_key(l);
        key == b"LFE2" || key == b"LFER"
    }) {
        LfeTarget::Lfe2
    } else if labels.iter().any(|l| is_lfe(l)) || low_pass {
        LfeTarget::Lfe1
    } else {
        LfeTarget::None
    }
}

pub const LFE1_22_2: usize = 3;
pub const LFE2_22_2: usize = 9;
pub const LFE_SPLIT_POWER_GAIN: f32 = std::f32::consts::FRAC_1_SQRT_2;

/// The 22.2 (`9+10+3`) LFE routing decision shared by every speaker backend.
#[derive(Clone, Copy, Debug, Default, PartialEq, Eq)]
pub struct LfeRouting {
    pub applies_to_22_2: bool,
    pub split_power: bool,
    pub has_lfe1: bool,
    pub has_lfe2: bool,
}

impl LfeRouting {
    pub fn gain(&self, input: LfeTarget, output: LfeTarget) -> f32 {
        if !self.applies_to_22_2 || input == LfeTarget::None || output == LfeTarget::None {
            0.
        } else if self.split_power {
            LFE_SPLIT_POWER_GAIN
        } else if input == output {
            1.
        } else {
            0.
        }
    }
}

#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub enum Level {
    Info,
    Warning,
}

/// An error with `adm::ErrorCode` and the C++ message and context bytes.
#[derive(Clone, Debug, PartialEq, Eq)]
pub struct Error {
    pub code: i32,
    pub message: Vec<u8>,
    pub context: Vec<u8>,
}

fn invalid(message: Vec<u8>, context: Vec<u8>) -> Error {
    Error {
        code: 1,
        message,
        context,
    }
}

fn concat(parts: &[&[u8]]) -> Vec<u8> {
    parts.concat()
}

/// The 22.2 LFE routing for the in-effect DirectSpeakers blocks' targets. The log lines come back
/// in order, also when the request fails; their text is static.
pub fn lfe_routing(
    split_power: bool,
    output_layout: &[u8],
    blocks: impl IntoIterator<Item = LfeTarget>,
    log: &mut dyn FnMut(Level, &'static str),
) -> Result<LfeRouting, Error> {
    let mut routing = LfeRouting {
        applies_to_22_2: output_layout == b"9+10+3",
        split_power,
        ..LfeRouting::default()
    };
    if !routing.applies_to_22_2 {
        if split_power {
            log(
                Level::Warning,
                "LFE routing mode 'split-power' only applies to 22.2 ('9+10+3'); using existing LFE routing",
            );
        }
        return Ok(routing);
    }
    for target in blocks {
        match target {
            LfeTarget::Lfe1 => routing.has_lfe1 = true,
            LfeTarget::Lfe2 => routing.has_lfe2 = true,
            LfeTarget::None => {}
        }
    }
    log(
        Level::Info,
        if split_power {
            "22.2 LFE routing mode: split-power"
        } else {
            "22.2 LFE routing mode: direct"
        },
    );
    if split_power {
        if routing.has_lfe1 && routing.has_lfe2 {
            return Err(invalid(
                b"22.2 split-power requires a single semantic LFE channel; scene contains both LFE1 and LFE2"
                    .to_vec(),
                Vec::new(),
            ));
        }
        if !routing.has_lfe1 && !routing.has_lfe2 {
            log(
                Level::Warning,
                "22.2 split-power requested but no semantic LFE channel was found; LFE outputs remain silent",
            );
        }
    }
    Ok(routing)
}

/// One row of a DirectSpeakers matrix as authored.
#[derive(Clone, Debug)]
pub struct MatrixRoute<'a> {
    pub source_label: &'a [u8],
    pub mute: bool,
    /// Target labels with their normalised gains.
    pub targets: Vec<(&'a [u8], f32)>,
}

#[derive(Clone, Copy, Debug, PartialEq)]
pub struct ResolvedTarget {
    pub output: usize,
    pub gain: f32,
}

#[derive(Clone, Debug, PartialEq)]
pub struct ResolvedRoute {
    pub source_label: Vec<u8>,
    pub source_key: Vec<u8>,
    pub mute: bool,
    pub targets: Vec<ResolvedTarget>,
}

/// Every matrix target resolved to one output slot of the layout. Unknown, ambiguous or LFE
/// targets, LFE sources and rows or targets that collide after alias resolution fail.
pub fn resolve_matrix(
    routes: &[MatrixRoute],
    targets: &[Target],
    layout_id: &[u8],
) -> Result<Vec<ResolvedRoute>, Error> {
    let layout_context = || concat(&[b"layout=", layout_id]);
    let mut resolved: Vec<ResolvedRoute> = Vec::with_capacity(routes.len());
    for route in routes {
        let source = route.source_label;
        let source_context = || concat(&[b"source_label=", source]);
        let source_key = direct_label(source);
        if source_key.is_empty() {
            return Err(invalid(
                b"DirectSpeakers matrix source_label resolves to an empty label".to_vec(),
                source_context(),
            ));
        }
        if is_lfe(&source_key) {
            return Err(invalid(
                concat(&[
                    b"DirectSpeakers matrix source '",
                    source,
                    b"' is LFE; LFE uses dedicated routing",
                ]),
                layout_context(),
            ));
        }
        if resolved.iter().any(|r| r.source_key == source_key) {
            return Err(invalid(
                concat(&[
                    b"duplicate DirectSpeakers matrix source '",
                    source,
                    b"' after alias resolution",
                ]),
                layout_context(),
            ));
        }
        let mut outputs: Vec<ResolvedTarget> = Vec::with_capacity(route.targets.len());
        for &(label, gain) in &route.targets {
            let target_error = |text: &[u8]| {
                invalid(
                    concat(&[b"DirectSpeakers matrix target '", label, text]),
                    source_context(),
                )
            };
            let key = direct_label(label);
            if key.is_empty() {
                return Err(invalid(
                    b"DirectSpeakers matrix target label resolves to an empty label".to_vec(),
                    source_context(),
                ));
            }
            if is_lfe(&key) {
                return Err(target_error(b"' is LFE; LFE uses dedicated routing"));
            }
            let mut output = None;
            for (channel, target) in targets.iter().enumerate() {
                if target.label.is_empty() || direct_label(target.label) != key {
                    continue;
                }
                if output.is_some() {
                    return Err(target_error(&concat(&[
                        b"' is ambiguous in output layout '",
                        layout_id,
                        b"'",
                    ])));
                }
                output = Some(channel);
            }
            let Some(output) = output else {
                return Err(target_error(&concat(&[
                    b"' is not present in output layout '",
                    layout_id,
                    b"'",
                ])));
            };
            if targets[output].is_lfe {
                return Err(target_error(b"' is LFE; LFE uses dedicated routing"));
            }
            if outputs.iter().any(|t| t.output == output) {
                return Err(invalid(
                    concat(&[
                        b"duplicate DirectSpeakers matrix target '",
                        label,
                        b"' after alias resolution",
                    ]),
                    source_context(),
                ));
            }
            outputs.push(ResolvedTarget { output, gain });
        }
        resolved.push(ResolvedRoute {
            source_label: source.to_vec(),
            source_key,
            mute: route.mute,
            targets: outputs,
        });
    }
    Ok(resolved)
}

fn joined(labels: &[&[u8]]) -> Vec<u8> {
    labels.join(&b","[..])
}

/// The one resolved row a non-LFE DirectSpeakers block's labels select.
pub fn matrix_route(resolved: &[ResolvedRoute], labels: &[&[u8]]) -> Result<usize, Error> {
    let mut found = None;
    for label in labels {
        let key = direct_label(label);
        let Some(index) = resolved.iter().position(|r| r.source_key == key) else {
            continue;
        };
        if found.is_some_and(|f| f != index) {
            return Err(invalid(
                b"DirectSpeakers block matches multiple matrix source rows".to_vec(),
                concat(&[b"labels=", &joined(labels)]),
            ));
        }
        found = Some(index);
    }
    found.ok_or_else(|| {
        let labels = if labels.is_empty() {
            b"<missing>".to_vec()
        } else {
            joined(labels)
        };
        invalid(
            concat(&[
                b"DirectSpeakers block label '",
                &labels,
                b"' is not covered by the routing matrix",
            ]),
            Vec::new(),
        )
    })
}

/// One live override as the routing sees it.
#[derive(Clone, Copy, Debug)]
pub struct Override<'a> {
    pub object_id: &'a [u8],
    pub speaker_label: &'a [u8],
    pub mute: bool,
    pub gain_db: f32,
    pub head_locked: Option<bool>,
}

/// Resolve borrowed records without allocating, including when the boundary validates them as
/// it walks them. The last matching channel override wins over the last whole-object override;
/// gain and head lock select independently. Unrequested fields are not evaluated.
pub fn try_live_override<'a, E>(
    overrides: impl IntoIterator<Item = Result<Override<'a>, E>>,
    object_id: &[u8],
    key: &[u8],
    want_gain: bool,
    want_head: bool,
) -> Result<(Option<f32>, Option<bool>), E> {
    let (mut whole_gain, mut channel_gain) = (None, None);
    let (mut whole_head, mut channel_head) = (None, None);
    for ov in overrides {
        let ov = ov?;
        if ov.object_id != object_id || !(want_gain || want_head && ov.head_locked.is_some()) {
            continue;
        }
        let specific = !ov.speaker_label.is_empty();
        if specific
            && (key.is_empty() || !canonical_bytes(ov.speaker_label).eq(key.iter().copied()))
        {
            continue;
        }
        if want_gain {
            *if specific {
                &mut channel_gain
            } else {
                &mut whole_gain
            } = Some(ov);
        }
        if want_head && ov.head_locked.is_some() {
            *if specific {
                &mut channel_head
            } else {
                &mut whole_head
            } = ov.head_locked;
        }
    }
    let gain = channel_gain
        .or(whole_gain)
        .map(|o| if o.mute { 0. } else { live_gain(o.gain_db) });
    Ok((gain, channel_head.or(whole_head)))
}

/// The live gain multiplier of one input channel: exactly 0 when muted, none without an override.
pub fn live_channel_gain(overrides: &[Override], object_id: &[u8], key: &[u8]) -> Option<f32> {
    try_live_override(
        overrides
            .iter()
            .copied()
            .map(Ok::<_, std::convert::Infallible>),
        object_id,
        key,
        true,
        false,
    )
    .expect("infallible records")
    .0
}

/// An explicit live head lock; overrides without one do not take part.
pub fn live_head_locked(overrides: &[Override], object_id: &[u8], key: &[u8]) -> Option<bool> {
    try_live_override(
        overrides
            .iter()
            .copied()
            .map(Ok::<_, std::convert::Infallible>),
        object_id,
        key,
        false,
        true,
    )
    .expect("infallible records")
    .1
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn labels_fold_ascii_only() {
        assert_eq!(canonical_label(b"m_+030 \xc3\xa9"), b"M+030");
        assert_eq!(direct_label(b"Rc_L"), b"M+030");
        assert_eq!(label_key(b"lfe-2"), b"LFE2");
        assert!(is_lfe(b"Sub Woofer") && !is_lfe(b"L"));
    }

    #[test]
    fn positions_follow_from_chars() {
        assert_eq!(label_position(&[b"M+-30"]), Some([-30., 0.]));
        assert_eq!(label_position(&[b"T-000"]), Some([0., 90.]));
        assert_eq!(label_position(&[b"M+181", b"LS"]), Some([90., 0.]));
        assert_eq!(label_position(&[b"T+010"]), None);
    }
}
