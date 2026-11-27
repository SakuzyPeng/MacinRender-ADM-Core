//! Identify a complete fixed speaker array at topology preparation. PCM planes
//! may be reordered and the same scene may also carry independently moving Objects.
use crate::routing;
use mradm_dsp::room::{
    calibration::{Feed, Speaker, SpeakerLayout, same_position},
    control::RoomDescription,
    loading,
};
use mradm_scene::{
    input::find_input_layout,
    layouts::{self, Geometry},
};

#[derive(Default)]
pub(crate) struct Candidate {
    pub label: Vec<u8>,
    pub position: [f32; 3],
    pub inferred: bool,
    pub reassigned: bool,
}

pub(crate) fn assign(descriptions: &mut [RoomDescription], candidates: &[Candidate]) {
    let count = descriptions.iter().filter(|d| d.role != 0).count();
    let Some(layout) = SpeakerLayout::ALL
        .into_iter()
        .find(|l| l.channels() == count)
    else {
        return;
    };
    let preset = find_input_layout(layout.id()).expect("built-in speaker layout");
    let apple = layouts::find(preset.internal_id, Geometry::Apple).expect("built-in Apple layout");
    let mut standard_positions = true;
    let mut apple_positions = true;
    let mut seen = 0u32;
    let mut bindings = Vec::with_capacity(count);
    for (i, (d, candidate)) in descriptions.iter().zip(candidates).enumerate() {
        if d.role == 0 {
            continue;
        }
        if candidate.reassigned {
            return;
        }
        let label = routing::direct_label(&candidate.label);
        let found = preset.channels.iter().enumerate().find(|(_, c)| {
            c.is_lfe == (d.role == 2)
                && (c.speaker_label.as_bytes() == label
                    || (c.is_lfe && label.is_empty() && layout != SpeakerLayout::TwentyTwoTwo))
        });
        let Some((index, c)) = found else {
            return;
        };
        let bit = 1 << index;
        if seen & bit != 0 {
            return;
        }
        seen |= bit;
        let nominal = loading::polar(c.azimuth, c.elevation, 1.).expect("finite speaker direction");
        if !c.is_lfe && !candidate.inferred {
            let source = apple
                .speakers
                .iter()
                .find(|s| s.label == c.speaker_label)
                .expect("same speaker labels");
            let alternative = loading::polar(source.azimuth, source.elevation, 1.)
                .expect("finite speaker direction");
            standard_positions &= same_position(nominal, candidate.position);
            apple_positions &= same_position(alternative, candidate.position);
        }
        bindings.push((
            i,
            Speaker {
                feed: Feed {
                    layout,
                    channel: index as u8,
                },
                nominal,
                source_nominal: (!same_position(nominal, candidate.position))
                    .then_some(candidate.position),
            },
        ));
    }
    if !standard_positions && !apple_positions {
        return;
    }
    for (index, speaker) in bindings {
        descriptions[index].speaker = Some(speaker);
    }
}

#[cfg(test)]
mod tests {
    use super::*;
    #[test]
    fn matches_fixed_feeds_in_mixed_scenes_and_rejects_ambiguous_arrays() {
        let layout = find_input_layout("9.1.6").unwrap();
        let mut candidates: Vec<_> = layout
            .channels
            .iter()
            .map(|c| Candidate {
                label: c.speaker_label.as_bytes().to_vec(),
                position: loading::polar(c.azimuth, c.elevation, 1.).unwrap(),
                ..Default::default()
            })
            .collect();
        let mut sources: Vec<_> = layout
            .channels
            .iter()
            .map(|c| RoomDescription {
                role: if c.is_lfe { 2 } else { 1 },
                ..Default::default()
            })
            .collect();
        candidates.reverse();
        sources.reverse();
        sources.push(RoomDescription::default());
        candidates.push(Candidate::default());
        assign(&mut sources, &candidates);
        assert!(
            sources[..16]
                .iter()
                .enumerate()
                .all(|(i, s)| usize::from(s.speaker.unwrap().feed.channel) == 15 - i)
        );
        assert!(sources[16].speaker.is_none());
        for s in &mut sources {
            s.speaker = None;
        }
        candidates[0].position = [0.2, 0.6, 0.1];
        assign(&mut sources, &candidates);
        assert!(sources.iter().all(|s| s.speaker.is_none()));
        candidates[0].inferred = true;
        candidates[0].label = candidates[1].label.clone();
        assign(&mut sources, &candidates);
        assert!(sources.iter().all(|s| s.speaker.is_none()));
    }
}
