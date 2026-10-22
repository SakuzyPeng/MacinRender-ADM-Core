//! Semantic policy for Scene streams (PCM consistency 4.4): element identities, per-generation
//! resolution and per-update application to producer state. Moved from the C++
//! `SceneStreamEngine` (`build_policy_identities`, `apply_policy_to_state`) with the same rules;
//! the block and DirectSpeakers actions are the offline ones in [`crate::policy`].
use crate::error::{Context, Error, Result};
use crate::policy::{
    self, DirectFields, DirectState, Identity, Policy, Resolved, apply_block, apply_direct_fields,
    gain_rule, object_rule,
};
use mradm_adm::records::{AdmPosition, ObjectBlock};
use mradm_dsp::scene_math;
use mradm_dsp::semantic_policy as numeric;
use std::collections::{BTreeSet, HashMap};

/// The semantic identity a producer attaches to one element.
#[derive(Clone, Debug, Default, PartialEq)]
pub struct SourceIdentity {
    pub object_id: String,
    pub object_name: String,
    pub track_uid: String,
    pub importance: Option<i32>,
    pub dialogue_id: Option<i32>,
    /// (id, name) pairs.
    pub contents: Vec<(String, String)>,
    pub programmes: Vec<(String, String)>,
}

#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub enum Role {
    Object,
    DirectSpeakers,
    Lfe,
}

/// One element of a Scene generation as the policy sees it.
#[derive(Clone, Debug, PartialEq)]
pub struct Element {
    pub role: Role,
    pub speaker_label: String,
    /// The descriptor's Cartesian routing position.
    pub position: Option<[f32; 3]>,
    pub identity: Option<SourceIdentity>,
}

/// A generation's elements with their merged identities.
#[derive(Clone, Debug, PartialEq)]
pub struct Generation {
    elements: Vec<Element>,
    identities: Vec<Identity>,
}

fn append_unique(values: &mut Vec<String>, value: &str) {
    if !value.is_empty() && !values.iter().any(|v| v == value) {
        values.push(value.to_owned());
    }
}

fn disagree(field: &str, object_id: &str) -> Error {
    Error::invalid(
        format!("Scene semantic identities disagree on {field} for object_id {object_id}"),
        Context::None,
    )
}

impl Generation {
    /// Elements sharing an `object_id` take one merged identity: the first non-empty name and
    /// the first importance and dialogue id, with every track, content and programme. Conflicting
    /// names, importance or dialogue ids are errors, reported for the object ids in the order
    /// they first appear.
    pub fn new(elements: Vec<Element>) -> Result<Self> {
        let mut identities = vec![Identity::default(); elements.len()];
        let mut groups: Vec<(String, Vec<usize>)> = Vec::new();
        let mut group_of: HashMap<String, usize> = HashMap::new();
        for (index, element) in elements.iter().enumerate() {
            let Some(source) = &element.identity else {
                continue;
            };
            let identity = &mut identities[index];
            identity.object_id.clone_from(&source.object_id);
            identity.object_name.clone_from(&source.object_name);
            append_unique(&mut identity.track_uids, &source.track_uid);
            identity.importance = source.importance;
            identity.dialogue_id = source.dialogue_id;
            for (id, name) in &source.contents {
                append_unique(&mut identity.content_ids, id);
                append_unique(&mut identity.content_names, name);
            }
            for (id, name) in &source.programmes {
                append_unique(&mut identity.programme_ids, id);
                append_unique(&mut identity.programme_names, name);
            }
            if !source.object_id.is_empty() {
                let group = *group_of.entry(source.object_id.clone()).or_insert_with(|| {
                    groups.push((source.object_id.clone(), Vec::new()));
                    groups.len() - 1
                });
                groups[group].1.push(index);
            }
        }
        for (object_id, indices) in &groups {
            let mut aggregate = Identity {
                object_id: object_id.clone(),
                ..Identity::default()
            };
            for &index in indices {
                let source = &identities[index];
                if !aggregate.object_name.is_empty()
                    && !source.object_name.is_empty()
                    && aggregate.object_name != source.object_name
                {
                    return Err(disagree("object_name", object_id));
                }
                if aggregate.importance.is_some()
                    && source.importance.is_some()
                    && aggregate.importance != source.importance
                {
                    return Err(disagree("importance", object_id));
                }
                if aggregate.dialogue_id.is_some()
                    && source.dialogue_id.is_some()
                    && aggregate.dialogue_id != source.dialogue_id
                {
                    return Err(disagree("dialogue_id", object_id));
                }
                if aggregate.object_name.is_empty() {
                    aggregate.object_name.clone_from(&source.object_name);
                }
                aggregate.importance = aggregate.importance.or(source.importance);
                aggregate.dialogue_id = aggregate.dialogue_id.or(source.dialogue_id);
                for (into, from) in [
                    (&mut aggregate.track_uids, &source.track_uids),
                    (&mut aggregate.content_ids, &source.content_ids),
                    (&mut aggregate.content_names, &source.content_names),
                    (&mut aggregate.programme_ids, &source.programme_ids),
                    (&mut aggregate.programme_names, &source.programme_names),
                ] {
                    for value in from {
                        append_unique(into, value);
                    }
                }
            }
            for &index in indices {
                identities[index] = aggregate.clone();
            }
        }
        Ok(Self {
            elements,
            identities,
        })
    }

    pub fn len(&self) -> usize {
        self.elements.len()
    }

    pub fn is_empty(&self) -> bool {
        self.elements.is_empty()
    }

    pub fn identity(&self, index: usize) -> &Identity {
        &self.identities[index]
    }
}

/// A policy resolved for every element of one generation.
#[derive(Clone, Debug, PartialEq)]
pub struct Resolution {
    elements: Vec<(Element, Resolved)>,
    unmatched: Vec<usize>,
}

impl Resolution {
    /// Resolves each element; `unmatched` lists, in order, the rules no element matched.
    pub fn new(policy: &Policy, generation: &Generation) -> Self {
        let mut matched = BTreeSet::new();
        let elements = generation
            .elements
            .iter()
            .zip(&generation.identities)
            .map(|(element, identity)| {
                let resolved = policy::resolve(policy, identity);
                matched.extend(resolved.matched.iter().copied());
                (element.clone(), resolved)
            })
            .collect();
        let unmatched = (0..policy.objects.len())
            .filter(|i| !matched.contains(i))
            .collect();
        Self {
            elements,
            unmatched,
        }
    }

    pub fn len(&self) -> usize {
        self.elements.len()
    }

    pub fn is_empty(&self) -> bool {
        self.elements.is_empty()
    }

    pub fn unmatched(&self) -> &[usize] {
        &self.unmatched
    }

    /// Applies element `index`'s policy to its producer state; `None` when out of range.
    pub fn apply(
        &self,
        index: usize,
        sample_rate: u32,
        state: &mut State,
        update: Option<&mut Update>,
    ) -> Option<()> {
        let (element, resolved) = self.elements.get(index)?;
        apply(element, resolved, sample_rate, state, update);
        Some(())
    }
}

/// The producer state fields a policy reads or changes.
#[derive(Clone, Copy, Debug, Default, PartialEq)]
pub struct State {
    /// In: the producer's position is valid. Out (DirectSpeakers): the effective position is.
    pub has_position: bool,
    pub linear_gain: f32,
    pub position: [f32; 3],
    pub width: f32,
    pub height: f32,
    pub depth: f32,
    pub diffuse: f32,
    pub divergence: f32,
    pub channel_lock: bool,
    pub screen_reference: bool,
    pub head_locked: bool,
    pub divergence_azimuth_range: f32,
    pub divergence_position_range: f32,
    pub max_distance: Option<f32>,
}

/// A metadata update's timing: in, the producer's; out, the policy's.
#[derive(Clone, Copy, Debug, Default, PartialEq, Eq)]
pub struct Update {
    pub jump_position: bool,
    /// 0 keeps the default interpolation.
    pub ramp_duration_samples: u32,
}

fn polar_of([x, y, z]: [f32; 3]) -> [f32; 3] {
    scene_math::cartesian_to_polar([x, y, z])
}

fn apply(
    element: &Element,
    resolved: &Resolved,
    sample_rate: u32,
    state: &mut State,
    update: Option<&mut Update>,
) {
    let object_policy = &resolved.object;
    let object_gain = match &object_policy.gain {
        Some(gain) => {
            let level = numeric::apply_gain(&gain_rule(gain), 1.);
            if gain.mute == Some(true) { 0. } else { level }
        }
        None => 1.,
    };

    if element.role == Role::Object {
        let [x, y, z] = state.position;
        let mut block = ObjectBlock {
            position: AdmPosition {
                cartesian: 1,
                azimuth: 0.,
                elevation: 0.,
                distance: 1.,
                x,
                y,
                z,
            },
            gain: state.linear_gain,
            diffuse: state.diffuse,
            width: state.width,
            height: state.height,
            depth: state.depth,
            channel_lock: u32::from(state.channel_lock),
            max_distance_present: u32::from(state.max_distance.is_some()),
            max_distance: state.max_distance.unwrap_or(0.),
            divergence: state.divergence,
            azimuth_range: state.divergence_azimuth_range,
            position_range: state.divergence_position_range,
            screen_ref: u32::from(state.screen_reference),
            head_locked: u32::from(state.head_locked),
            ..ObjectBlock::default()
        };
        if let Some(update) = &update {
            block.jump = u32::from(update.jump_position);
            if update.ramp_duration_samples != 0 {
                block.interpolation_present = 1;
                block.interpolation = u64::from(update.ramp_duration_samples);
            }
        }
        apply_block(
            &mut block,
            object_policy,
            &object_rule(object_policy),
            sample_rate,
        );
        let p = block.position;
        state.linear_gain = numeric::scene_level(block.gain, object_gain);
        state.diffuse = block.diffuse;
        state.width = block.width;
        state.height = block.height;
        state.depth = block.depth;
        state.channel_lock = block.channel_lock != 0;
        state.max_distance = (block.max_distance_present != 0).then_some(block.max_distance);
        state.divergence = block.divergence;
        state.divergence_azimuth_range = block.azimuth_range;
        state.divergence_position_range = block.position_range;
        state.screen_reference = block.screen_ref != 0;
        state.head_locked = block.head_locked != 0;
        state.position = if p.cartesian == 0 {
            numeric::scene_cartesian([p.azimuth, p.elevation, p.distance])
        } else {
            [p.x, p.y, p.z]
        };
        if let Some(update) = update {
            update.jump_position = block.jump != 0;
            update.ramp_duration_samples = if block.interpolation_present != 0 {
                u32::try_from(block.interpolation).unwrap_or(u32::MAX)
            } else {
                0
            };
        }
        return;
    }

    let lfe = element.role == Role::Lfe;
    let producer_has_position = state.has_position;
    let (polar, has_position) = if producer_has_position {
        (polar_of(state.position), true)
    } else if let Some(position) = element.position {
        (polar_of(position), true)
    } else {
        ([0., 0., 1.], false)
    };
    let label = element.speaker_label.as_str();
    let has_label = |wanted: &str| !label.is_empty() && label.eq_ignore_ascii_case(wanted);
    let run = |has_position: bool| {
        let mut fields = DirectFields {
            head_locked: state.head_locked,
            gain: state.linear_gain,
            polar,
        };
        let mut direct = DirectState {
            has_position,
            ..DirectState::default()
        };
        if let Some(head_locked) = object_policy.head_locked {
            fields.head_locked = head_locked;
        }
        for policy in &resolved.direct_speakers {
            apply_direct_fields(has_label, lfe, &mut fields, &mut direct, policy);
        }
        (fields, direct.has_position)
    };
    // A descriptor position is only a routing fallback: probe the actions separately so only a
    // matching position operation promotes it into the renderer's explicit state.
    let (_, policy_has_position) = run(false);
    let (fields, has_position) = run(has_position);
    let positioned = has_position && (producer_has_position || policy_has_position);
    state.linear_gain = numeric::scene_level(fields.gain, object_gain);
    state.head_locked = fields.head_locked;
    state.has_position = positioned;
    if positioned {
        state.position = numeric::scene_cartesian(fields.polar);
    }
}

#[cfg(test)]
mod tests {
    use super::*;

    fn element(role: Role, identity: Option<SourceIdentity>) -> Element {
        Element {
            role,
            speaker_label: "M+030".to_owned(),
            position: Some([0.5, 0.5, 0.]),
            identity,
        }
    }

    fn identity(object_id: &str, name: &str, importance: Option<i32>) -> Option<SourceIdentity> {
        Some(SourceIdentity {
            object_id: object_id.to_owned(),
            object_name: name.to_owned(),
            track_uid: format!("ATU_{name}"),
            importance,
            ..SourceIdentity::default()
        })
    }

    #[test]
    fn identities_merge_per_object_and_report_the_first_conflict() {
        let generation = Generation::new(vec![
            element(Role::Object, identity("AO_1", "", Some(3))),
            element(Role::Object, identity("AO_1", "Lead", None)),
            element(Role::Object, None),
        ])
        .unwrap();
        let merged = generation.identity(0);
        assert_eq!(merged, generation.identity(1));
        assert_eq!(
            (merged.object_name.as_str(), merged.importance),
            ("Lead", Some(3))
        );
        assert_eq!(merged.track_uids, ["ATU_", "ATU_Lead"]);
        assert_eq!(generation.identity(2), &Identity::default());

        let conflict = Generation::new(vec![
            element(Role::Object, identity("AO_2", "", Some(1))),
            element(Role::Object, identity("AO_1", "A", None)),
            element(Role::Object, identity("AO_1", "B", None)),
            element(Role::Object, identity("AO_2", "", Some(2))),
        ])
        .unwrap_err();
        assert_eq!(
            conflict.message,
            "Scene semantic identities disagree on importance for object_id AO_2"
        );
    }

    #[test]
    fn a_descriptor_position_is_promoted_only_by_a_position_action() {
        let generation = Generation::new(vec![element(
            Role::DirectSpeakers,
            identity("AO_1", "", None),
        )])
        .unwrap();
        let state = |json: &str| {
            let resolution = Resolution::new(&policy::parse(json.as_bytes()).unwrap(), &generation);
            let mut state = State {
                linear_gain: 1.,
                ..State::default()
            };
            resolution.apply(0, 48000, &mut state, None).unwrap();
            state
        };
        let gain = state(
            r#"{"schema":"mradm.semantic-policy.v1","objects":[{"id":"AO_1","direct_speakers":{"gain":{"scale":0.5}}}]}"#,
        );
        assert!(!gain.has_position && gain.linear_gain == 0.5);
        let aimed = state(
            r#"{"schema":"mradm.semantic-policy.v1","objects":[{"id":"ao_1","direct_speakers":{"speaker_label":"m+030","position":{"azimuth":-90}}}]}"#,
        );
        // Re-aimed to the right at the fallback's distance, sqrt(0.5).
        assert!(aimed.has_position);
        assert!((aimed.position[0] - 0.5f32.sqrt()).abs() < 1e-6 && aimed.position[1].abs() < 1e-6);
    }

    #[test]
    fn unmatched_rules_and_out_of_range_elements() {
        let generation =
            Generation::new(vec![element(Role::Object, identity("AO_1", "", None))]).unwrap();
        let policy = policy::parse(
            br#"{"schema":"mradm.semantic-policy.v1","objects":[{"id":"AO_9"},{"all":true},{"id":"AO_8"}]}"#,
        )
        .unwrap();
        let resolution = Resolution::new(&policy, &generation);
        assert_eq!(resolution.unmatched(), [0, 2]);
        assert!(
            resolution
                .apply(1, 48000, &mut State::default(), None)
                .is_none()
        );
    }
}
