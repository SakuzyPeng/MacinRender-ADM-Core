//! The Scene stream element records both speaker renderers read (`live_scene::ElementDescriptor`,
//! `ObjectState`, `Frame` and `Diagnostic`) and the state rules of `live_scene_state.h`.

/// The fields of an element state (`live_scene::StateField`).
pub mod field {
    pub const ACTIVE: u64 = 1 << 0;
    pub const LINEAR_GAIN: u64 = 1 << 1;
    pub const POSITION: u64 = 1 << 2;
    pub const EXTENT: u64 = 1 << 3;
    pub const DIFFUSE: u64 = 1 << 4;
    pub const DIVERGENCE: u64 = 1 << 5;
    pub const CHANNEL_LOCK: u64 = 1 << 6;
    pub const SCREEN_REFERENCE: u64 = 1 << 7;
    pub const HEAD_LOCKED: u64 = 1 << 8;
    pub const DIVERGENCE_RANGE: u64 = 1 << 9;
    pub const CHANNEL_LOCK_MAX_DISTANCE: u64 = 1 << 10;
    pub const KNOWN: u64 = (1 << 11) - 1;
}

/// An element state (`live_scene::ObjectState`); `valid_fields` says which fields are set.
#[derive(Clone, Copy, Debug, PartialEq)]
pub struct State {
    pub valid_fields: u64,
    pub active: bool,
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
    pub channel_lock_max_distance: Option<f32>,
}

impl Default for State {
    fn default() -> Self {
        Self {
            valid_fields: 0,
            active: true,
            linear_gain: 1.,
            position: [0., 1., 0.],
            width: 0.,
            height: 0.,
            depth: 0.,
            diffuse: 0.,
            divergence: 0.,
            channel_lock: false,
            screen_reference: false,
            head_locked: false,
            divergence_azimuth_range: 45.,
            divergence_position_range: 0.,
            channel_lock_max_distance: None,
        }
    }
}

#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub enum Role {
    Object,
    DirectSpeaker,
    Lfe,
}

/// An element of a generation; `role` is `None` for a value outside the three roles.
#[derive(Clone, Copy, Debug)]
pub struct Element<'a> {
    pub id: u64,
    pub role: Option<Role>,
    pub speaker_label: &'a [u8],
    /// The semantic identity's AudioObject id; empty without one.
    pub object_id: &'a [u8],
    pub has_position: bool,
    pub position: [f32; 3],
}

#[derive(Clone, Copy, Debug)]
pub struct Plane<'a> {
    pub element_id: u64,
    pub has_signal: bool,
    pub samples: &'a [f32],
}

/// Indexed PCM views borrowed for one render call. Transport records can project each view
/// directly without allocating an intermediate array or retaining the caller's samples.
pub trait PlaneSource {
    fn plane(&self, index: usize) -> Plane<'_>;
    fn iter(&self) -> impl Iterator<Item = Plane<'_>> + Clone;
}

impl PlaneSource for [Plane<'_>] {
    fn plane(&self, index: usize) -> Plane<'_> {
        self[index]
    }

    fn iter(&self) -> impl Iterator<Item = Plane<'_>> + Clone {
        self.iter().copied()
    }
}

#[derive(Clone, Copy, Debug)]
pub struct Entry {
    pub element_id: u64,
    pub state: State,
}

#[derive(Clone, Copy, Debug)]
pub struct Update {
    pub element_id: u64,
    pub offset: u32,
    pub ramp: u32,
    pub jump: bool,
    pub changed: u64,
    pub cleared: u64,
    pub state: State,
}

/// One rendered slice. `trace` is the checkpoint prefix (`<scene renderer key>.30-vbap<N>` or
/// `.30-triple`); empty writes none.
#[derive(Debug)]
pub struct FrameView<'a, P: ?Sized> {
    pub generation: u64,
    pub duration: u32,
    pub planes: &'a P,
    pub initial: &'a [Entry],
    pub updates: &'a [Update],
    pub trace: &'a [u8],
}

impl<P: ?Sized> Copy for FrameView<'_, P> {}

impl<P: ?Sized> Clone for FrameView<'_, P> {
    fn clone(&self) -> Self {
        *self
    }
}

/// A render slice backed by an array of already borrowed PCM views.
pub type Frame<'a> = FrameView<'a, [Plane<'a>]>;

/// `live_scene::DiagnosticCode`.
pub mod code {
    pub const SEMANTIC_DEGRADED: u32 = 1;
    pub const DIRECT_SPEAKER_FALLBACK: u32 = 2;
    pub const MISSING_LFE_OUTPUT: u32 = 3;
}

/// A warning of the last rendered frame.
#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub struct Diagnostic {
    pub code: u32,
    pub element_id: u64,
    pub field: u64,
    pub message: &'static str,
}

/// `default_state`: every field but the position is valid; an object's position is too, and a
/// producer position seeds it.
pub(crate) fn default_state(role: Role, position: Option<[f32; 3]>) -> State {
    let mut state = State {
        valid_fields: field::KNOWN & !field::POSITION,
        ..State::default()
    };
    if role == Role::Object {
        state.valid_fields |= field::POSITION;
    }
    if let Some(position) = position {
        state.position = position;
    }
    state
}

pub(crate) fn copy_state_fields(destination: &mut State, source: &State, requested: u64) {
    let fields = source.valid_fields & requested;
    let has = |f: u64| fields & f != 0;
    if has(field::ACTIVE) {
        destination.active = source.active;
    }
    if has(field::LINEAR_GAIN) {
        destination.linear_gain = source.linear_gain;
    }
    if has(field::POSITION) {
        destination.position = source.position;
    }
    if has(field::EXTENT) {
        destination.width = source.width;
        destination.height = source.height;
        destination.depth = source.depth;
    }
    if has(field::DIFFUSE) {
        destination.diffuse = source.diffuse;
    }
    if has(field::DIVERGENCE) {
        destination.divergence = source.divergence;
    }
    if has(field::CHANNEL_LOCK) {
        destination.channel_lock = source.channel_lock;
    }
    if has(field::SCREEN_REFERENCE) {
        destination.screen_reference = source.screen_reference;
    }
    if has(field::HEAD_LOCKED) {
        destination.head_locked = source.head_locked;
    }
    if has(field::DIVERGENCE_RANGE) {
        destination.divergence_azimuth_range = source.divergence_azimuth_range;
        destination.divergence_position_range = source.divergence_position_range;
    }
    if has(field::CHANNEL_LOCK_MAX_DISTANCE) {
        destination.channel_lock_max_distance = source.channel_lock_max_distance;
    }
    destination.valid_fields |= fields;
}

/// Finite values (policy-expanded coordinates and extents may leave the producer ranges), a
/// non-negative gain, divergence ranges and lock distance.
pub(crate) fn valid_state(s: &State) -> bool {
    let has = |f: u64| s.valid_fields & f != 0;
    let finite = |v: &[f32]| v.iter().all(|x| x.is_finite());
    !(s.valid_fields & !field::KNOWN != 0
        || has(field::LINEAR_GAIN) && (!s.linear_gain.is_finite() || s.linear_gain < 0.)
        || has(field::POSITION) && !finite(&s.position)
        || has(field::EXTENT) && !finite(&[s.width, s.height, s.depth])
        || has(field::DIFFUSE) && !s.diffuse.is_finite()
        || has(field::DIVERGENCE) && !s.divergence.is_finite()
        || has(field::DIVERGENCE_RANGE)
            && (!s.divergence_azimuth_range.is_finite()
                || s.divergence_azimuth_range < 0.
                || !s.divergence_position_range.is_finite()
                || s.divergence_position_range < 0.)
        || has(field::CHANNEL_LOCK_MAX_DISTANCE)
            && s.channel_lock_max_distance
                .is_some_and(|d| !d.is_finite() || d < 0.))
}

pub(crate) fn level_for(state: &State) -> f32 {
    if state.valid_fields & field::ACTIVE != 0 && !state.active {
        return 0.;
    }
    if state.valid_fields & field::LINEAR_GAIN != 0 {
        state.linear_gain
    } else {
        1.
    }
}
