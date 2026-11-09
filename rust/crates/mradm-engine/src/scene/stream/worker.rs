//! The Scene stream worker thread (formerly `SceneStreamEngine::Impl`'s worker side): it takes the
//! queued work and controls, slices each frame for the renderer, applies the semantic policy,
//! crossfades backend switches, resamples, gates the output ring by the media timeline and pads the
//! end of stream.
use super::trace;
use super::{
    Control, Descriptor, Generation, Kind, MetaUpdate, OwnedFrame, OwnedPlane, PendingBackend,
    Shared, State, WorkItem, code, diagnostic, error, frame_flag, level,
};
use crate::backend::Host;
use crate::request::Selection;
use crate::scene::renderer::Renderer;
use mradm_dsp::resampler::Resampler;
use mradm_dsp::scene_transition::Transitions;
use mradm_render::live::{self, field};
use mradm_render::routing::Error;
use mradm_scene::{live_policy, policy};
use std::collections::HashMap;
use std::panic::{AssertUnwindSafe, catch_unwind};
use std::sync::Arc;
use std::sync::atomic::Ordering;
use std::time::Duration;

const RESAMPLER_OUTPUT_FRAMES: usize = 4096;
const POLICY_TRANSITION_MS: u64 = 20;
const WORKER_CHUNK_FRAMES: u32 = 1024;
const TRACKING_CHUNK_FRAMES: u32 = 512;
const RING_WAIT: Duration = Duration::from_millis(1);

/// A renderer call; a panic is the error the FFI boundary of the C++ adapter returned.
fn guarded<T>(f: impl FnOnce() -> Result<T, Error>) -> (Result<T, Error>, bool) {
    match catch_unwind(AssertUnwindSafe(f)) {
        Ok(result) => (result, false),
        Err(_) => (
            Err(Error {
                code: code::INTERNAL_ERROR,
                message: b"Rust render panic".to_vec(),
                context: Vec::new(),
            }),
            true,
        ),
    }
}

fn configure(
    shared: &Shared,
    renderer: &mut Renderer,
    generation: u64,
    elements: &[Descriptor],
) -> Result<(), Error> {
    let records: Vec<live::Element> = elements
        .iter()
        .map(|e| live::Element {
            id: e.element_id,
            role: match e.role {
                0 => Some(live::Role::Object),
                1 => Some(live::Role::DirectSpeaker),
                2 => Some(live::Role::Lfe),
                _ => None,
            },
            speaker_label: &e.speaker_label,
            object_id: e.identity.as_ref().map_or(&[][..], |i| &i.object_id),
            has_position: e.has_position,
            position: e.position,
        })
        .collect();
    let (result, panicked) = guarded(|| renderer.configure(generation, &records));
    if !panicked {
        shared
            .diagnostics
            .renderer(renderer.configured(), 0, generation);
    }
    result
}

/// One slice as the renderers see it.
struct Slice {
    epoch: u64,
    generation: u64,
    start: i64,
    duration: u32,
    flags: u32,
    pcm: Vec<OwnedPlane>,
    initial: Vec<live::Entry>,
    updates: Vec<MetaUpdate>,
}

fn render(
    shared: &Shared,
    renderer: &mut Renderer,
    slice: &Slice,
    initial: &[live::Entry],
    role: &str,
    output: &mut [f32],
) -> Result<(), Error> {
    let planes: Vec<live::Plane> = slice
        .pcm
        .iter()
        .map(|p| live::Plane {
            element_id: p.element_id,
            has_signal: p.has_signal,
            samples: &p.samples,
        })
        .collect();
    let updates: Vec<live::Update> = slice.updates.iter().map(|u| u.update).collect();
    let trace_prefix = if trace::ENABLED {
        format!(
            "{}-{role}{}",
            trace::slice_key(slice.epoch, slice.generation, slice.start),
            renderer.trace_suffix()
        )
    } else {
        String::new()
    };
    let frame = live::Frame {
        generation: slice.generation,
        duration: slice.duration,
        planes: &planes[..],
        initial,
        updates: &updates,
        trace: trace_prefix.as_bytes(),
    };
    let (result, panicked) = guarded(|| renderer.render(&frame, output));
    if !panicked {
        shared
            .diagnostics
            .renderer(renderer.diagnostics(), slice.epoch, slice.generation);
    }
    result
}

fn end_input(renderer: &mut Renderer) -> Result<u64, Error> {
    guarded(|| renderer.end_input()).0
}

pub(super) struct Worker {
    shared: Arc<Shared>,
    host: Arc<dyn Host + Send + Sync>,
    channels: usize,
    renderer: Renderer,
    selection: Selection,
    room_worker_active: bool,
    resampler: Option<Resampler>,
    current_generation: u64,
    has_current_generation: bool,
    model: Option<Arc<Generation>>,
    current_policy: Option<Arc<policy::Policy>>,
    resolved: Option<live_policy::Resolution>,
    policy_retarget_pending: bool,
    orientation: [f32; 3],
    base_states: HashMap<u64, live::State>,
    effective_states: HashMap<u64, live::State>,
    element_index: HashMap<u64, usize>,
    incoming: Option<(Renderer, Selection)>,
    deferred: Option<PendingBackend>,
    incoming_needs_initial_state: bool,
    render_output: Vec<f32>,
    render_output_b: Vec<f32>,
    resample_output: Vec<f32>,
    zero_output: Vec<f32>,
    pending_output: Vec<f32>,
    pending_frame_offset: usize,
    target_sample: i64,
    preroll_output_boundary: u64,
    preroll_rational_remainder: u64,
    preroll_output_frames_generated: u64,
    output_frames_to_skip: u64,
    target_reached: bool,
    rational_remainder: u64,
    allowed_output_frames: u64,
    output_frames_pushed: u64,
    transitions: Transitions,
    trace_output_frames: u64,
}

/// The controls taken with one work item.
#[derive(Default)]
struct Controls {
    backend: Option<PendingBackend>,
    orientation: Option<[f32; 3]>,
    policy: Option<(Option<Arc<policy::Policy>>, u64)>,
}

impl Controls {
    fn is_empty(&self) -> bool {
        self.backend.is_none() && self.orientation.is_none() && self.policy.is_none()
    }

    fn take(c: &mut Control) -> Self {
        let mut controls = Self {
            backend: c.pending_backend.take(),
            ..Self::default()
        };
        if c.orientation_pending {
            controls.orientation = Some(c.pending_orientation);
            c.orientation_pending = false;
        }
        if c.policy_pending {
            controls.policy = Some((c.pending_policy.take(), c.pending_policy_revision));
            c.policy_pending = false;
        }
        controls
    }
}

impl Worker {
    pub(super) fn new(
        shared: Arc<Shared>,
        host: Arc<dyn Host + Send + Sync>,
        renderer: Renderer,
        selection: Selection,
        resampler: Option<Resampler>,
        transitions: Transitions,
    ) -> Self {
        let channels = shared.channels;
        Self {
            shared,
            host,
            channels,
            renderer,
            selection,
            room_worker_active: false,
            resampler,
            current_generation: 0,
            has_current_generation: false,
            model: None,
            current_policy: None,
            resolved: None,
            policy_retarget_pending: false,
            orientation: [0.; 3],
            base_states: HashMap::new(),
            effective_states: HashMap::new(),
            element_index: HashMap::new(),
            incoming: None,
            deferred: None,
            incoming_needs_initial_state: false,
            render_output: Vec::new(),
            render_output_b: Vec::new(),
            resample_output: vec![0.; RESAMPLER_OUTPUT_FRAMES * channels],
            zero_output: vec![0.; RESAMPLER_OUTPUT_FRAMES * channels],
            pending_output: Vec::new(),
            pending_frame_offset: 0,
            target_sample: 0,
            preroll_output_boundary: 0,
            preroll_rational_remainder: 0,
            preroll_output_frames_generated: 0,
            output_frames_to_skip: 0,
            target_reached: false,
            rational_remainder: 0,
            allowed_output_frames: 0,
            output_frames_pushed: 0,
            transitions,
            trace_output_frames: 0,
        }
    }

    fn rate(&self) -> u32 {
        self.shared.input_rate
    }

    fn report_backend_switch_failure(&self, detail: &[u8]) {
        let mut message = b"live Scene backend switch was rejected: ".to_vec();
        message.extend_from_slice(detail);
        self.shared.add(
            level::ERROR,
            diagnostic::BACKEND_FAILURE,
            self.shared.epoch_status.load(Ordering::Relaxed),
            self.current_generation,
            message,
        );
    }

    fn adopt(&mut self, renderer: Renderer, selection: Selection) {
        self.renderer = renderer;
        self.shared.set_room_control(&self.renderer);
        self.selection = selection;
    }

    fn begin_backend_switch(&mut self, backend: Option<PendingBackend>) {
        let Some(mut backend) = backend else {
            return;
        };
        backend.renderer.pose(self.orientation);
        if self.has_current_generation
            && let Some(model) = self.model.clone()
            && let Err(e) = configure(
                &self.shared,
                &mut backend.renderer,
                self.current_generation,
                &model.elements,
            )
        {
            self.report_backend_switch_failure(&e.message);
            return;
        }
        if !self.has_current_generation || self.model.is_none() {
            self.adopt(backend.renderer, backend.selection);
            return;
        }
        self.incoming = Some((backend.renderer, backend.selection));
        self.transitions.reset_backend();
        self.incoming_needs_initial_state = true;
    }

    fn start_deferred_backend_switch(&mut self) {
        if self.incoming.is_some() || self.deferred.is_none() {
            return;
        }
        let next = self.deferred.take();
        self.begin_backend_switch(next);
    }

    fn abandon_incoming_backend(&mut self) {
        self.incoming = None;
        self.transitions.reset_backend();
        self.incoming_needs_initial_state = false;
        self.start_deferred_backend_switch();
    }

    fn finalize_backend_switch(&mut self) {
        let Some((renderer, selection)) = self.incoming.take() else {
            return;
        };
        self.adopt(renderer, selection);
        self.transitions.reset_backend();
        self.incoming_needs_initial_state = false;
        self.start_deferred_backend_switch();
    }

    fn resolve(&self, policy: &Option<Arc<policy::Policy>>) -> Option<live_policy::Resolution> {
        let (Some(model), Some(policy)) = (&self.model, policy) else {
            return None;
        };
        let resolved = live_policy::Resolution::new(policy, &model.policy);
        for index in resolved.unmatched() {
            self.shared.add(
                level::WARNING,
                diagnostic::SEMANTIC_DEGRADED,
                self.shared.epoch_status.load(Ordering::Relaxed),
                self.current_generation,
                format!("semantic policy objects[{index}] did not match this Scene generation")
                    .into_bytes(),
            );
        }
        Some(resolved)
    }

    /// The resolved policy applied to one element's producer state (and an update's timing).
    fn apply(
        &self,
        index: usize,
        base: &live::State,
        update: Option<&mut live::Update>,
    ) -> live::State {
        let (Some(_), Some(resolved), Some(_)) =
            (&self.current_policy, &self.resolved, &self.model)
        else {
            return *base;
        };
        let mut s = live_policy::State {
            has_position: base.valid_fields & field::POSITION != 0,
            linear_gain: base.linear_gain,
            position: base.position,
            width: base.width,
            height: base.height,
            depth: base.depth,
            diffuse: base.diffuse,
            divergence: base.divergence,
            channel_lock: base.channel_lock,
            screen_reference: base.screen_reference,
            head_locked: base.head_locked,
            divergence_azimuth_range: base.divergence_azimuth_range,
            divergence_position_range: base.divergence_position_range,
            max_distance: base.channel_lock_max_distance,
        };
        let mut timing = update.as_ref().map(|u| live_policy::Update {
            jump_position: u.jump,
            ramp_duration_samples: u.ramp,
        });
        if resolved
            .apply(index, self.rate(), &mut s, timing.as_mut())
            .is_none()
        {
            return *base;
        }
        let mut out = *base;
        out.linear_gain = s.linear_gain;
        out.position = s.position;
        out.width = s.width;
        out.height = s.height;
        out.depth = s.depth;
        out.diffuse = s.diffuse;
        out.divergence = s.divergence;
        out.channel_lock = s.channel_lock;
        out.screen_reference = s.screen_reference;
        out.head_locked = s.head_locked;
        out.divergence_azimuth_range = s.divergence_azimuth_range;
        out.divergence_position_range = s.divergence_position_range;
        out.channel_lock_max_distance = s.max_distance;
        if s.has_position {
            out.valid_fields |= field::POSITION;
        } else {
            out.valid_fields &= !field::POSITION;
        }
        if let (Some(update), Some(timing)) = (update, timing) {
            update.jump = timing.jump_position;
            update.ramp = timing.ramp_duration_samples;
        }
        out
    }

    fn effective_state_snapshot(&self) -> Vec<live::Entry> {
        let Some(model) = &self.model else {
            return Vec::new();
        };
        model
            .elements
            .iter()
            .map(|d| live::Entry {
                element_id: d.element_id,
                state: self
                    .effective_states
                    .get(&d.element_id)
                    .copied()
                    .unwrap_or_else(|| d.default_state()),
            })
            .collect()
    }

    fn take_pending_controls(&self) -> Controls {
        let mut c = self.shared.lock();
        if c.reset_request != 0 {
            return Controls::default();
        }
        Controls::take(&mut c)
    }

    fn apply_pending_controls(&mut self, controls: Controls) {
        if let Some((policy, revision)) = controls.policy {
            let resolved = self.resolve(&policy);
            self.current_policy = policy;
            self.resolved = resolved;
            self.policy_retarget_pending = self.has_current_generation;
            self.shared
                .applied_policy_revision
                .store(revision, Ordering::Release);
        }
        if let Some(orientation) = controls.orientation {
            self.orientation = orientation;
            self.renderer.pose(orientation);
            if let Some((incoming, _)) = &mut self.incoming {
                incoming.pose(orientation);
            }
            if let Some(deferred) = &mut self.deferred {
                deferred.renderer.pose(orientation);
            }
        }
        let Some(backend) = controls.backend else {
            return;
        };
        if self.incoming.is_some() && self.transitions.status().backend_position > 0 {
            // Replacing an audible incoming renderer would jump back to the outgoing backend:
            // finish this curve, then start the latest one.
            self.deferred = Some(backend);
            return;
        }
        self.incoming = None;
        self.transitions.reset_backend();
        self.incoming_needs_initial_state = false;
        self.deferred = None;
        self.begin_backend_switch(Some(backend));
    }

    fn apply_available_controls(&mut self) {
        let controls = self.take_pending_controls();
        if !controls.is_empty() {
            self.apply_pending_controls(controls);
        }
    }

    fn perform_reset(&mut self, serial: u64, target: i64) {
        let latest = self.shared.lock().pending_backend.take();
        let latest = latest.or_else(|| self.deferred.take());
        if let Some(latest) = latest {
            self.adopt(latest.renderer, latest.selection);
            self.incoming = None;
        } else if let Some((renderer, selection)) = self.incoming.take() {
            self.adopt(renderer, selection);
        }
        self.deferred = None;
        self.transitions.reset_backend();
        self.incoming_needs_initial_state = false;
        self.renderer.reset();
        self.renderer.pose(self.orientation);
        if let Some(resampler) = &mut self.resampler {
            resampler.reset();
        }
        self.shared.ring.clear();
        self.pending_output.clear();
        self.pending_frame_offset = 0;
        self.current_generation = 0;
        self.has_current_generation = false;
        self.model = None;
        self.element_index.clear();
        self.resolved = None;
        self.base_states.clear();
        self.effective_states.clear();
        self.policy_retarget_pending = false;
        self.target_sample = target;
        self.preroll_output_boundary = 0;
        self.preroll_rational_remainder = 0;
        self.preroll_output_frames_generated = 0;
        self.output_frames_to_skip = 0;
        self.target_reached = false;
        self.rational_remainder = 0;
        self.allowed_output_frames = 0;
        self.output_frames_pushed = 0;
        self.transitions.reset();
        self.trace_output_frames = 0;
        let s = &self.shared;
        s.media_frames_pulled.store(0, Ordering::Relaxed);
        s.underruns.store(0, Ordering::Relaxed);
        s.output_ready.store(false, Ordering::Relaxed);
        s.production_done.store(false, Ordering::Relaxed);
        s.failed.store(false, Ordering::Relaxed);
        s.generation_status.store(0, Ordering::Relaxed);
        s.set_state(State::Buffering, Ordering::Release);
        s.lock().reset_ack = serial;
        s.cv.notify_all();
    }

    fn acknowledge_failed_reset(&self, serial: u64) {
        {
            let mut c = self.shared.lock();
            c.reset_ack = c.reset_ack.max(serial);
        }
        self.shared.cv.notify_all();
    }

    pub(super) fn run(mut self) {
        let shared = self.shared.clone();
        while !shared.quit.load(Ordering::Acquire) {
            let mut item = None;
            let mut controls = Controls::default();
            let mut pending_reset = 0;
            let mut pending_target = 0;
            {
                let c = shared.lock();
                let mut c = shared
                    .cv
                    .wait_while(c, |c| {
                        !(shared.quit.load(Ordering::Acquire)
                            || c.reset_request != 0
                            || !c.queue.is_empty()
                            || c.pending_backend.is_some()
                            || c.orientation_pending
                            || c.policy_pending)
                    })
                    .unwrap_or_else(|e| e.into_inner());
                if shared.quit.load(Ordering::Acquire) {
                    return;
                }
                if c.reset_request != 0 {
                    pending_reset = c.reset_request;
                    pending_target = c.reset_target;
                    c.reset_request = 0;
                } else {
                    controls = Controls::take(&mut c);
                    item = c.queue.pop_front();
                }
            }
            let item_epoch = item.as_ref().map_or(0, |i: &WorkItem| i.frame.epoch);
            let epoch = if item_epoch != 0 {
                item_epoch
            } else {
                shared.epoch_status.load(Ordering::SeqCst)
            };
            let generation = item.as_ref().map_or(0, |i| i.frame.generation);
            let serial = item.as_ref().map(|i| i.serial);
            let step = catch_unwind(AssertUnwindSafe(|| {
                if pending_reset != 0 {
                    self.perform_reset(pending_reset, pending_target);
                    return;
                }
                if !controls.is_empty() {
                    self.apply_pending_controls(std::mem::take(&mut controls));
                }
                let Some(item) = &item else {
                    return;
                };
                if item.serial != shared.reset_serial.load(Ordering::Acquire) {
                    return;
                }
                let processed = match item.kind {
                    Kind::Frame => self.process_frame(item),
                    Kind::End => self.process_end(item),
                    Kind::Fence => {
                        {
                            let mut c = shared.lock();
                            c.completed_fence = c.completed_fence.max(item.fence_id);
                        }
                        shared.cv.notify_all();
                        Ok(())
                    }
                };
                shared.release_budget(item);
                if let Err(e) = processed
                    && !shared.reset_or_quit(item.serial)
                {
                    shared.fail(&e, epoch, item.frame.generation);
                }
            }));
            if step.is_err() {
                if pending_reset != 0 {
                    self.acknowledge_failed_reset(pending_reset);
                }
                if serial.is_none_or(|serial| !shared.reset_or_quit(serial)) {
                    shared.fail_worker_exception(b"unknown exception", epoch, generation);
                }
            }
        }
    }

    fn switch_generation(&mut self, item: &WorkItem) -> Result<bool, Error> {
        let Some(generation) = item.generation.clone() else {
            return Err(error(
                code::INTERNAL_ERROR,
                "Scene frame lost its generation descriptor",
            ));
        };
        if self.has_current_generation && self.current_generation == generation.id {
            return Ok(false);
        }
        configure(
            &self.shared,
            &mut self.renderer,
            generation.id,
            &generation.elements,
        )?;
        if let Some((incoming, _)) = &mut self.incoming
            && let Err(e) = configure(&self.shared, incoming, generation.id, &generation.elements)
        {
            let mut message =
                b"incoming live Scene backend rejected a generation; keeping the current backend: "
                    .to_vec();
            message.extend_from_slice(&e.message);
            self.shared.add(
                level::ERROR,
                diagnostic::BACKEND_FAILURE,
                item.frame.epoch,
                generation.id,
                message,
            );
            self.incoming = None;
            self.transitions.reset_backend();
            self.incoming_needs_initial_state = false;
        }
        self.current_generation = generation.id;
        self.has_current_generation = true;
        self.model = Some(generation.clone());
        self.element_index.clear();
        self.base_states.clear();
        self.effective_states.clear();
        for (index, d) in generation.elements.iter().enumerate() {
            self.element_index.insert(d.element_id, index);
            self.base_states.insert(d.element_id, d.default_state());
        }
        self.resolved = self.resolve(&self.current_policy.clone());
        for (index, d) in generation.elements.iter().enumerate() {
            let base = self.base_states[&d.element_id];
            let effective = self.apply(index, &base, None);
            self.effective_states.insert(d.element_id, effective);
        }
        self.policy_retarget_pending = false;
        self.start_deferred_backend_switch();
        self.incoming_needs_initial_state = self.incoming.is_some();
        self.shared
            .generation_status
            .store(self.current_generation, Ordering::Release);
        self.transitions.begin_generation();
        Ok(true)
    }

    fn apply_initial_states(
        &mut self,
        source: &[live::Entry],
        generation_switched: bool,
        converted: &mut Vec<live::Entry>,
    ) {
        let model = self.model.clone();
        for initial in source {
            let Some(&index) = self.element_index.get(&initial.element_id) else {
                continue;
            };
            let descriptor = &model.as_ref().expect("a current generation").elements[index];
            let mut base = descriptor.default_state();
            live::copy_state_fields(&mut base, &initial.state, initial.state.valid_fields);
            self.base_states.insert(initial.element_id, base);
            let effective = self.apply(index, &base, None);
            self.effective_states.insert(initial.element_id, effective);
        }
        if generation_switched {
            *converted = self.effective_state_snapshot();
            return;
        }
        converted.reserve(source.len());
        for initial in source {
            if let Some(state) = self.effective_states.get(&initial.element_id) {
                converted.push(live::Entry {
                    element_id: initial.element_id,
                    state: *state,
                });
            }
        }
    }

    fn append_policy_retargets(&mut self, updates: &mut Vec<MetaUpdate>, order: &mut u64) {
        let Some(model) = self.model.clone() else {
            return;
        };
        if !self.policy_retarget_pending {
            return;
        }
        let transition = ((u64::from(self.rate()) * POLICY_TRANSITION_MS) / 1000).max(1) as u32;
        for (index, d) in model.elements.iter().enumerate() {
            let producer = self
                .base_states
                .get(&d.element_id)
                .copied()
                .unwrap_or_else(|| d.default_state());
            let next = self.apply(index, &producer, None);
            let previous = self.effective_states.get(&d.element_id).copied();
            if previous != Some(next) {
                updates.push(MetaUpdate {
                    update: live::Update {
                        element_id: d.element_id,
                        offset: 0,
                        ramp: transition,
                        jump: false,
                        changed: next.valid_fields,
                        cleared: previous.map_or(0, |p| p.valid_fields & !next.valid_fields),
                        state: next,
                    },
                    order: *order,
                });
                *order += 1;
            }
            self.effective_states.insert(d.element_id, next);
        }
        self.policy_retarget_pending = false;
    }

    fn append_producer_update(
        &mut self,
        source: &MetaUpdate,
        slice_start: u32,
        updates: &mut Vec<MetaUpdate>,
        order: &mut u64,
    ) {
        let Some(&index) = self.element_index.get(&source.update.element_id) else {
            return;
        };
        let model = self.model.clone().expect("a current generation");
        let descriptor = &model.elements[index];
        let id = source.update.element_id;
        let mut base = self
            .base_states
            .get(&id)
            .copied()
            .unwrap_or_else(|| descriptor.default_state());
        let previous = self.apply(index, &base, None);
        live::copy_state_fields(&mut base, &source.update.state, source.update.changed);
        self.base_states.insert(id, base);

        let mut converted = source.update;
        converted.offset -= slice_start;
        let state = self.apply(index, &base, Some(&mut converted));
        converted.state = state;
        // Retarget the explicit producer fields plus any fields the policy couples; the others
        // keep their own ramp and deadline.
        converted.changed = source.update.changed & converted.state.valid_fields;
        let mut bit = 1u64;
        while bit <= field::CHANNEL_LOCK_MAX_DISTANCE {
            let mut probe = previous;
            live::copy_state_fields(&mut probe, &converted.state, bit);
            if probe != previous {
                converted.changed |= bit;
            }
            bit <<= 1;
        }
        updates.push(MetaUpdate {
            update: converted,
            order: *order,
        });
        *order += 1;
        self.effective_states.insert(id, converted.state);
    }

    fn room_active(&self) -> bool {
        self.selection == Selection::RoomBinaural
            || self
                .incoming
                .as_ref()
                .is_some_and(|(_, s)| *s == Selection::RoomBinaural)
    }

    fn render_slice(
        &mut self,
        slice: &Slice,
        incoming_snapshot: &[live::Entry],
    ) -> Result<(), Error> {
        let room_active = self.room_active();
        if room_active || self.room_worker_active {
            self.host.room_worker(room_active);
            self.room_worker_active = room_active;
        }
        let key = if trace::ENABLED {
            trace::slice_key(slice.epoch, slice.generation, slice.start)
        } else {
            String::new()
        };
        if trace::ENABLED {
            for plane in &slice.pcm {
                if !plane.samples.is_empty() {
                    trace::f32(
                        &format!("{key}-element{}.10-input.f32", plane.element_id),
                        &plane.samples,
                    );
                }
            }
            let mut states = Vec::new();
            for entry in &slice.initial {
                let v = &entry.state;
                states.extend([
                    v.position[0],
                    v.position[1],
                    v.position[2],
                    v.linear_gain,
                    v.width,
                    v.height,
                    v.depth,
                    v.diffuse,
                    v.divergence,
                    f32::from(u8::from(v.head_locked)),
                    f32::from(u8::from(v.channel_lock)),
                ]);
            }
            for update in &slice.updates {
                let u = &update.update;
                let v = &u.state;
                states.extend([
                    u.offset as f32,
                    u.ramp as f32,
                    v.position[0],
                    v.position[1],
                    v.position[2],
                    v.linear_gain,
                    f32::from(u8::from(v.head_locked)),
                ]);
            }
            if !states.is_empty() {
                trace::f32(&format!("{key}.20-effective.f32"), &states);
            }
            trace::states(
                &format!("{key}.21-effective"),
                &slice.initial,
                &slice.updates,
            );
        }

        let samples = slice.duration as usize * self.channels;
        self.render_output.resize(samples, 0.);
        let role = if self.incoming.is_some() {
            "outgoing"
        } else {
            "current"
        };
        render(
            &self.shared,
            &mut self.renderer,
            slice,
            &slice.initial,
            role,
            &mut self.render_output,
        )?;
        let Some((incoming, _)) = &mut self.incoming else {
            return Ok(());
        };
        let initial = if self.incoming_needs_initial_state {
            incoming_snapshot
        } else {
            &slice.initial[..]
        };
        self.render_output_b.resize(samples, 0.);
        if let Err(e) = render(
            &self.shared,
            incoming,
            slice,
            initial,
            "incoming",
            &mut self.render_output_b,
        ) {
            let mut message =
                b"incoming live Scene backend failed while crossfading; keeping the current backend: "
                    .to_vec();
            message.extend_from_slice(&e.message);
            self.shared.add(
                level::ERROR,
                diagnostic::BACKEND_FAILURE,
                slice.epoch,
                slice.generation,
                message,
            );
            self.abandon_incoming_backend();
            return Ok(());
        }
        self.incoming_needs_initial_state = false;
        if trace::ENABLED {
            trace::f32(
                &format!("{key}.35-outgoing.f32"),
                &self.render_output[..samples],
            );
            trace::f32(
                &format!("{key}.36-incoming.f32"),
                &self.render_output_b[..samples],
            );
        }
        let done = self
            .transitions
            .mix(
                &mut self.render_output,
                &self.render_output_b,
                slice.duration as usize,
            )
            .map_err(|e| error(code::INTERNAL_ERROR, e.to_string()))?;
        if done {
            self.finalize_backend_switch();
        }
        Ok(())
    }

    fn feed_rendered_slice(&mut self, slice: &Slice, serial: u64) -> Result<(), Error> {
        let samples = slice.duration as usize * self.channels;
        if trace::ENABLED {
            trace::f32(
                &format!(
                    "{}.40-render.f32",
                    trace::slice_key(slice.epoch, slice.generation, slice.start)
                ),
                &self.render_output[..samples],
            );
        }
        let frame_end = slice.start + i64::from(slice.duration);
        let mut hidden = 0u32;
        if slice.start < self.target_sample {
            let hidden_end = frame_end.min(self.target_sample);
            hidden = (hidden_end - slice.start) as u32;
        }
        let mut output = std::mem::take(&mut self.render_output);
        let result = (|| {
            if hidden > 0 {
                self.add_preroll_samples(u64::from(hidden));
                self.feed_samples(
                    &output[..hidden as usize * self.channels],
                    false,
                    false,
                    false,
                    true,
                    serial,
                )?;
            }
            if hidden == slice.duration {
                return Ok(());
            }
            self.finish_preroll();
            let state_complete = slice.flags & frame_flag::STATE_COMPLETE != 0;
            let warmup = slice.flags & frame_flag::WARMUP != 0;
            let audible = &mut output[hidden as usize * self.channels..samples];
            let force_silence = !state_complete || warmup;
            if force_silence {
                audible.fill(0.);
            }
            self.feed_samples(audible, true, true, force_silence, false, serial)
        })();
        self.render_output = output;
        result
    }

    fn process_frame(&mut self, item: &WorkItem) -> Result<(), Error> {
        if self.shared.reset_or_quit(item.serial) {
            return Ok(());
        }
        let switched = self.switch_generation(item)?;
        let frame: &OwnedFrame = &item.frame;
        let mut update_index = 0;
        let mut slice_start = 0u32;
        let mut first = true;
        while slice_start < frame.duration {
            if self.shared.reset_or_quit(item.serial) {
                return Ok(());
            }
            self.apply_available_controls();
            if self.shared.reset_or_quit(item.serial) {
                return Ok(());
            }
            let maximum = if self.shared.head_tracking_recent() {
                TRACKING_CHUNK_FRAMES
            } else {
                WORKER_CHUNK_FRAMES
            };
            let count = maximum.min(frame.duration - slice_start);
            let slice_end = slice_start + count;
            let mut slice = Slice {
                epoch: frame.epoch,
                generation: frame.generation,
                start: frame.start + i64::from(slice_start),
                duration: count,
                flags: if first {
                    frame.flags
                } else {
                    frame.flags & !frame_flag::DISCONTINUITY
                },
                pcm: frame
                    .pcm
                    .iter()
                    .map(|p| OwnedPlane {
                        element_id: p.element_id,
                        has_signal: p.has_signal,
                        samples: if p.has_signal {
                            p.samples[slice_start as usize..slice_end as usize].to_vec()
                        } else {
                            Vec::new()
                        },
                    })
                    .collect(),
                initial: Vec::new(),
                updates: Vec::new(),
            };
            if first {
                self.apply_initial_states(&frame.initial, switched, &mut slice.initial);
            }
            let incoming_snapshot = if self.incoming.is_some() && self.incoming_needs_initial_state
            {
                self.effective_state_snapshot()
            } else {
                Vec::new()
            };
            let mut order = 0;
            self.append_policy_retargets(&mut slice.updates, &mut order);
            let mut producer = Vec::new();
            while update_index < frame.updates.len()
                && frame.updates[update_index].update.offset < slice_end
            {
                let source = frame.updates[update_index];
                if trace::ENABLED {
                    let mut copy = source;
                    copy.update.offset -= slice_start;
                    producer.push(copy);
                }
                self.append_producer_update(&source, slice_start, &mut slice.updates, &mut order);
                update_index += 1;
            }
            if trace::ENABLED {
                // Producer state before semantic policy; offsets are slice-relative like the
                // effective rows.
                trace::states(
                    &format!(
                        "{}.15-producer",
                        trace::slice_key(slice.epoch, slice.generation, slice.start)
                    ),
                    if first { &frame.initial } else { &[] },
                    &producer,
                );
            }
            self.render_slice(&slice, &incoming_snapshot)?;
            if self.shared.reset_or_quit(item.serial) {
                return Ok(());
            }
            self.feed_rendered_slice(&slice, item.serial)?;
            slice_start = slice_end;
            first = false;
        }
        Ok(())
    }

    fn add_preroll_samples(&mut self, input_frames: u64) {
        let product = input_frames.wrapping_mul(u64::from(self.shared.output_rate));
        let numerator = self.preroll_rational_remainder.wrapping_add(product);
        let rate = u64::from(self.rate());
        self.preroll_output_boundary = self.preroll_output_boundary.wrapping_add(numerator / rate);
        self.preroll_rational_remainder = numerator % rate;
    }

    fn finish_preroll(&mut self) {
        if self.target_reached {
            return;
        }
        let boundary =
            self.preroll_output_boundary + u64::from(self.preroll_rational_remainder != 0);
        // Sinc lookahead can defer the final pre-target frames until audible input arrives.
        self.output_frames_to_skip = boundary.saturating_sub(self.preroll_output_frames_generated);
        self.target_reached = true;
    }

    fn add_timeline_samples(&mut self, input_frames: u64) {
        let product = input_frames.wrapping_mul(u64::from(self.shared.output_rate));
        let numerator = self.rational_remainder.wrapping_add(product);
        let rate = u64::from(self.rate());
        self.allowed_output_frames = self.allowed_output_frames.wrapping_add(numerator / rate);
        self.rational_remainder = numerator % rate;
    }

    fn compact_pending(&mut self) {
        if self.pending_frame_offset == 0 {
            return;
        }
        let total = self.pending_output.len() / self.channels;
        if self.pending_frame_offset >= total {
            self.pending_output.clear();
            self.pending_frame_offset = 0;
            return;
        }
        if self.pending_frame_offset >= RESAMPLER_OUTPUT_FRAMES {
            self.pending_output
                .drain(..self.pending_frame_offset * self.channels);
            self.pending_frame_offset = 0;
        }
    }

    /// Pushes `frames` frames into the ring under the lookahead cap, waiting for the player.
    fn push_floats(
        shared: &Shared,
        host: &dyn Host,
        room: bool,
        pushed_total: &mut u64,
        samples: &[f32],
        frames: u64,
        serial: u64,
    ) {
        let channels = shared.channels;
        let wait = || {
            if room {
                host.room_wait(RING_WAIT);
            } else {
                std::thread::sleep(RING_WAIT);
            }
        };
        let mut offset = 0u64;
        while offset < frames {
            if shared.reset_or_quit(serial) {
                return;
            }
            let lookahead = shared.output_lookahead();
            let buffered = shared.buffered_frames();
            // Head tracking can lower the cap after an untracked partial fill: publish readiness
            // even when nothing more fits under the cap.
            shared.publish_readiness(buffered, lookahead);
            if buffered >= lookahead {
                wait();
                continue;
            }
            let permitted = (frames - offset).min((lookahead - buffered) as u64);
            let start = offset as usize * channels;
            let pushed = shared
                .ring
                .push(&samples[start..start + permitted as usize * channels]);
            let pushed_frames = (pushed / channels) as u64;
            offset += pushed_frames;
            *pushed_total += pushed_frames;
            shared.publish_readiness(shared.buffered_frames(), lookahead);
            if pushed_frames == 0 {
                wait();
            }
        }
    }

    fn drain_pending(&mut self, serial: u64) {
        let room = self.room_active();
        while self.output_frames_pushed < self.allowed_output_frames {
            let total = self.pending_output.len() / self.channels;
            if self.pending_frame_offset >= total {
                break;
            }
            let available = (total - self.pending_frame_offset) as u64;
            let permitted = self.allowed_output_frames - self.output_frames_pushed;
            let frames = available.min(permitted);
            let start = self.pending_frame_offset * self.channels;
            Self::push_floats(
                &self.shared,
                self.host.as_ref(),
                room,
                &mut self.output_frames_pushed,
                &self.pending_output[start..],
                frames,
                serial,
            );
            self.pending_frame_offset += frames as usize;
            self.compact_pending();
        }
    }

    fn accept_resampled(
        &mut self,
        samples: &mut [f32],
        collect_output: bool,
        force_silence: bool,
        before_target: bool,
        serial: u64,
    ) -> Result<(), Error> {
        let frames = samples.len() / self.channels;
        let key = if trace::ENABLED {
            format!(
                "scene/e{}-out{}",
                self.shared.epoch_status.load(Ordering::SeqCst),
                self.trace_output_frames
            )
        } else {
            String::new()
        };
        if trace::ENABLED && frames != 0 {
            trace::f32(&format!("{key}.50-resampled.f32"), samples);
        }
        self.transitions
            .process_output(samples, frames, force_silence)
            .map_err(|e| error(code::INTERNAL_ERROR, e.to_string()))?;
        if trace::ENABLED {
            if frames != 0 {
                trace::f32(&format!("{key}.60-transition.f32"), samples);
            }
            self.trace_output_frames += frames as u64;
        }
        if before_target {
            self.preroll_output_frames_generated += frames as u64;
        }
        if !collect_output || frames == 0 {
            return Ok(());
        }
        let skipped = self.output_frames_to_skip.min(frames as u64);
        self.output_frames_to_skip -= skipped;
        let kept = &samples[skipped as usize * self.channels..];
        if kept.is_empty() {
            return Ok(());
        }
        self.compact_pending();
        self.pending_output.extend_from_slice(kept);
        self.drain_pending(serial);
        Ok(())
    }

    fn feed_samples(
        &mut self,
        samples: &[f32],
        count_timeline: bool,
        collect_output: bool,
        force_silence: bool,
        before_target: bool,
        serial: u64,
    ) -> Result<(), Error> {
        let channels = self.channels;
        let frames = samples.len() / channels;
        if count_timeline {
            self.add_timeline_samples(frames as u64);
        }
        let mut buffer = std::mem::take(&mut self.resample_output);
        let result = (|| {
            let Some(mut converter) = self.resampler.take() else {
                let mut offset = 0;
                while offset < frames {
                    let count = (frames - offset).min(RESAMPLER_OUTPUT_FRAMES);
                    buffer[..count * channels]
                        .copy_from_slice(&samples[offset * channels..(offset + count) * channels]);
                    self.accept_resampled(
                        &mut buffer[..count * channels],
                        collect_output,
                        force_silence,
                        before_target,
                        serial,
                    )?;
                    offset += count;
                }
                return Ok(());
            };
            let result = (|| {
                let mut input = samples;
                loop {
                    let progress = converter
                        .process(input, &mut buffer)
                        .map_err(resampler_error)?;
                    self.accept_resampled(
                        &mut buffer[..progress.output_frames * channels],
                        collect_output,
                        force_silence,
                        before_target,
                        serial,
                    )?;
                    input = &input[progress.input_frames * channels..];
                    // Drain this slice's buffered output with the same preroll / silence flags,
                    // even after the resampler has taken all of the input.
                    if input.is_empty() && progress.output_frames == 0 {
                        break;
                    }
                    if progress.input_frames == 0 && progress.output_frames == 0 {
                        return Err(error(
                            code::RENDER_FAILED,
                            "live Scene resampler made no progress",
                        ));
                    }
                }
                Ok(())
            })();
            self.resampler = Some(converter);
            result
        })();
        self.resample_output = buffer;
        result
    }

    fn flush_resampler(&mut self, serial: u64) -> Result<(), Error> {
        if let Some(mut converter) = self.resampler.take() {
            let mut buffer = std::mem::take(&mut self.resample_output);
            let result = (|| {
                loop {
                    let generated = converter.finish(&mut buffer).map_err(resampler_error)?;
                    if generated == 0 {
                        break;
                    }
                    self.accept_resampled(
                        &mut buffer[..generated * self.channels],
                        true,
                        false,
                        false,
                        serial,
                    )?;
                }
                Ok(())
            })();
            self.resampler = Some(converter);
            self.resample_output = buffer;
            result?;
        }
        self.drain_pending(serial);
        Ok(())
    }

    /// Renders the `tail` frames that the end of input counted on the timeline; tail slices
    /// continue the timeline after the end marker.
    fn flush_backend(&mut self, item: &WorkItem, tail: u64) -> Result<(), Error> {
        self.apply_available_controls();
        if !self.has_current_generation {
            return Ok(());
        }
        let mut remaining = tail;
        let mut start = item.frame.start;
        while remaining > 0 {
            if self.shared.reset_or_quit(item.serial) {
                return Ok(());
            }
            self.apply_available_controls();
            let maximum = if self.shared.head_tracking_recent() {
                TRACKING_CHUNK_FRAMES
            } else {
                WORKER_CHUNK_FRAMES
            };
            let count = remaining.min(u64::from(maximum)) as u32;
            let mut tail = Slice {
                epoch: item.frame.epoch,
                generation: self.current_generation,
                start,
                duration: count,
                flags: frame_flag::STATE_COMPLETE,
                pcm: Vec::new(),
                initial: Vec::new(),
                updates: Vec::new(),
            };
            let incoming_snapshot = if self.incoming.is_some() && self.incoming_needs_initial_state
            {
                self.effective_state_snapshot()
            } else {
                Vec::new()
            };
            let mut order = 0;
            self.append_policy_retargets(&mut tail.updates, &mut order);
            self.render_slice(&tail, &incoming_snapshot)?;
            let output = std::mem::take(&mut self.render_output);
            let fed = self.feed_samples(
                &output[..count as usize * self.channels],
                false,
                true,
                false,
                false,
                item.serial,
            );
            self.render_output = output;
            fed?;
            remaining -= u64::from(count);
            start = start.saturating_add(i64::from(count));
        }
        Ok(())
    }

    fn process_end(&mut self, item: &WorkItem) -> Result<(), Error> {
        self.finish_preroll();
        let mut tail = end_input(&mut self.renderer)?;
        if let Some((incoming, _)) = &mut self.incoming {
            tail = tail.max(end_input(incoming)?);
        }
        // Round the combined material and room tail once, including resampling.
        self.add_timeline_samples(tail);
        if self.rational_remainder != 0 {
            self.allowed_output_frames += 1;
            self.rational_remainder = 0;
        }
        self.flush_backend(item, tail)?;
        self.flush_resampler(item.serial)?;
        let room = self.room_active();
        while self.output_frames_pushed < self.allowed_output_frames {
            let count = (self.allowed_output_frames - self.output_frames_pushed)
                .min(RESAMPLER_OUTPUT_FRAMES as u64);
            Self::push_floats(
                &self.shared,
                self.host.as_ref(),
                room,
                &mut self.output_frames_pushed,
                &self.zero_output,
                count,
                item.serial,
            );
            if self.shared.reset_or_quit(item.serial) {
                break;
            }
        }
        if self.shared.reset_or_quit(item.serial) {
            return Ok(());
        }
        let s = &self.shared;
        // Draining first: a pull that sees production done may end the stream at once.
        s.advance(&[State::Buffering, State::Running], State::Draining);
        s.output_ready.store(true, Ordering::Release);
        s.production_done.store(true, Ordering::Release);
        Ok(())
    }
}

/// The C++ `dsp::Resampler` error: the FFI status, its text and the context `resampler`.
fn resampler_error(e: mradm_dsp::Error) -> Error {
    let code = match e {
        mradm_dsp::Error::InvalidArgument(_) => 1,
        mradm_dsp::Error::Unsupported(_) => 2,
        mradm_dsp::Error::Io(_) => 3,
        mradm_dsp::Error::RenderFailed(_) => 4,
    };
    Error {
        code,
        message: e.to_string().into_bytes(),
        context: b"resampler".to_vec(),
    }
}
