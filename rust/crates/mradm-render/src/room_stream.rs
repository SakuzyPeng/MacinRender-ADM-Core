//! The room file stream and Scene stream renderer (PCM consistency 4.6, moved from the C++
//! `RoomStream` and `LiveRoomRenderer`): input in 2048-frame blocks, the event table replayed on
//! the input clock, the room tail after the material, seeks that prime the event anchors and
//! re-render only the finite acoustic history (stepped, so the caller can cancel), live edits and
//! the Scene generations. Checks and texts are the C++ ones.
use crate::live::{self, Diagnostic, Element, FrameView, PlaneSource, Role, code, field};
use crate::offline::{Driver, Error, Location};
use crate::room::Prepared;
use crate::room_runtime::{Config, Control, Prepare, Resources, Runtime, Scope};
use crate::routing::{self, Override};
use crate::triple_balance_stream::LiveOverride;
use mradm_dsp::room::control::{RoomCommand, RoomDescription, RoomOverride, State};
use mradm_dsp::room::loading;
use mradm_dsp::room::screen::RoomScreens;
use mradm_wav::Reader;
use std::collections::HashMap;
use std::fs::File;
use std::io::BufReader;
use std::sync::Arc;

const READ_FRAMES: usize = 2048;

fn error(code: i32, message: &str) -> Error {
    Error {
        code,
        message: message.to_owned(),
        location: Location::None,
    }
}

/// A runtime error: its context is the room's.
fn runtime(e: routing::Error) -> Error {
    Error {
        code: e.code,
        message: String::from_utf8_lossy(&e.message).into_owned(),
        location: if e.context.is_empty() {
            Location::None
        } else {
            Location::Room
        },
    }
}

/// `live_value`: the override of one source; without one, neutral.
fn live_value(value: Option<&LiveOverride>) -> RoomOverride {
    let mut result = RoomOverride {
        gain_db: 0.,
        mute: 0,
        extent: [1., 1., 1.],
        diffuse: 1.,
        divergence: 1.,
        has_head: 0,
        head: 0,
        reserved: 0,
    };
    if let Some(v) = value {
        result.gain_db = v.base.gain_db;
        result.mute = u32::from(v.base.mute);
        result.extent = [
            v.extent_scale * v.extent_width_scale,
            v.extent_scale * v.extent_height_scale,
            v.extent_scale * v.extent_depth_scale,
        ];
        result.diffuse = v.diffuse_scale;
        result.divergence = v.divergence_scale;
        result.has_head = u32::from(v.base.head_locked.is_some());
        result.head = u32::from(v.base.head_locked.unwrap_or(false));
    }
    result
}

pub struct FileStream {
    table: Arc<Prepared>,
    reader: Reader<BufReader<File>>,
    runtime: Runtime,
    scope: Scope,
    input: Vec<f32>,
    planar: Vec<f32>,
    events: Vec<RoomCommand>,
    overrides: Vec<RoomOverride>,
    scratch: Vec<f32>,
    error: Option<Error>,
    position: u64,
    end: u64,
    event: usize,
    ended: bool,
    target: Option<u64>,
}

impl FileStream {
    /// Opens `path` for `table` (the caller checked its format against the scene) and seeks to 0.
    pub fn open(
        path: &str,
        table: Arc<Prepared>,
        resources: Arc<Resources>,
        config: Config,
        pose: [f32; 3],
        scope: Scope,
        prepare: Prepare,
    ) -> Result<Self, Error> {
        let reader = Driver::open_reader(path)?;
        let mut runtime = Runtime::create(resources, config, false, table.spread, scope, prepare)
            .map_err(runtime_error)?;
        runtime
            .configure(&table.descriptions)
            .map_err(runtime_error)?;
        runtime.pose(pose).map_err(runtime_error)?;
        let sources = table.descriptions.len();
        let mut stream = Self {
            input: vec![0.; usize::from(table.input_channels) * READ_FRAMES],
            planar: vec![0.; sources * READ_FRAMES],
            events: Vec::with_capacity(4096),
            overrides: vec![live_value(None); sources],
            scratch: vec![0.; READ_FRAMES * 2],
            table,
            reader,
            runtime,
            scope,
            error: None,
            position: 0,
            end: 0,
            event: 0,
            ended: false,
            target: None,
        };
        if stream.seek_begin(0)? {
            while stream.seek_step()? {}
        }
        Ok(stream)
    }

    pub fn control(&self) -> Control {
        self.runtime.control()
    }

    pub fn tail_frames(&self) -> u64 {
        self.runtime.tail_frames()
    }

    pub fn sample_rate(&self) -> u32 {
        self.runtime.sample_rate()
    }

    pub fn process(&mut self, output: &mut [f32], frames: usize) -> Result<usize, Error> {
        if frames > output.len() / 2 {
            return Err(error(1, "room-binaural 输出缓冲区不足"));
        }
        if let Some(e) = &self.error {
            return Err(e.clone());
        }
        let total = self.table.frames;
        let channels = usize::from(self.table.input_channels);
        let mut produced = 0;
        while produced < frames {
            if self.position == total && !self.ended {
                let tail = self.runtime.end_input().map_err(runtime_error)?;
                self.ended = true;
                self.end = total + tail;
            }
            if self.ended && self.position == self.end {
                break;
            }
            let limit = if self.ended { self.end } else { total };
            let take = READ_FRAMES
                .min(frames - produced)
                .min((limit - self.position) as usize);
            self.events.clear();
            if !self.ended {
                let read = self
                    .reader
                    .read_frames(&mut self.input[..take * channels])
                    .map_err(Error::wav)?;
                if read != take as u64 {
                    return Err(error(3, "room-binaural 读取素材提前结束"));
                }
                for (source, &channel) in self.table.channels.iter().enumerate() {
                    let plane = &mut self.planar[source * READ_FRAMES..][..take];
                    for (frame, sample) in plane.iter_mut().enumerate() {
                        *sample = self.input[frame * channels + usize::from(channel)];
                    }
                }
                let events = &self.table.events;
                while self.event < events.len()
                    && events[self.event].sample < self.position + take as u64
                {
                    let mut command = events[self.event].command;
                    command.offset = (events[self.event].sample - self.position) as u32;
                    self.events.push(command);
                    self.event += 1;
                }
            }
            let ended = self.ended;
            let planar = &self.planar;
            let planes = (0..self.table.descriptions.len())
                .map(|s| (!ended).then(|| &planar[s * READ_FRAMES..][..take]));
            self.runtime
                .process(
                    take,
                    planes,
                    &[],
                    &self.events,
                    &mut output[produced * 2..(produced + take) * 2],
                )
                .map_err(runtime_error)?;
            self.position += take as u64;
            produced += take;
        }
        Ok(produced)
    }

    /// Starts a seek: replays the event anchors before the history start, including
    /// independently running ramps, without replaying old PCM. Whether the history (rendered by
    /// `seek_step`) is pending; the caller may cancel between steps.
    pub fn seek_begin(&mut self, frame: u64) -> Result<bool, Error> {
        let maximum = self.table.frames + self.runtime.tail_frames();
        if frame > maximum {
            return Err(error(1, "room-binaural seek 超过节目及尾声长度"));
        }
        let history = self.runtime.preroll_frames();
        let start = if frame > history {
            (frame - history) / READ_FRAMES as u64 * READ_FRAMES as u64
        } else {
            0
        };
        self.runtime.reset(start, false).map_err(runtime_error)?;
        self.event = 0;
        let events = &self.table.events;
        while self.event < events.len() && events[self.event].sample < start {
            self.runtime
                .prime(&events[self.event].command, events[self.event].sample)
                .map_err(runtime_error)?;
            self.event += 1;
        }
        self.reader
            .seek_frame(start.min(self.table.frames))
            .map_err(Error::wav)?;
        self.position = start;
        self.ended = false;
        self.end = maximum;
        self.error = None;
        self.target = Some(frame);
        Ok(self.position < frame)
    }

    /// Renders one history block towards the seek target; whether another is pending.
    pub fn seek_step(&mut self) -> Result<bool, Error> {
        let Some(target) = self.target.filter(|&t| self.position < t) else {
            return Ok(false);
        };
        let mut scratch = std::mem::take(&mut self.scratch);
        let count = self.process(
            &mut scratch,
            READ_FRAMES.min((target - self.position) as usize),
        );
        self.scratch = scratch;
        if count? == 0 {
            return Err(error(6, "room-binaural 预热提前结束"));
        }
        Ok(self.position < target)
    }

    pub fn validate_overrides(&self, overrides: &[LiveOverride]) -> Result<(), (i32, String)> {
        for item in overrides {
            let scales = [
                item.extent_scale,
                item.extent_width_scale,
                item.extent_height_scale,
                item.extent_depth_scale,
            ];
            if scales.iter().any(|s| !s.is_finite() || *s < 0.) {
                return Err((1, "房间 extent 比例必须有限且非负".to_owned()));
            }
            let value = live_value(Some(item));
            (self.scope)(&mut || value.state().map(|_| ()))
                .map_err(|(code, text)| (code, text.to_owned()))?;
        }
        Ok(())
    }

    /// Latches an invalid update, as the C++ stream did, for the next `process`.
    pub fn set_overrides(&mut self, overrides: &[LiveOverride]) {
        if let Err((code, message)) = self.validate_overrides(overrides) {
            self.error = Some(error(code, &message));
            return;
        }
        let bases: Vec<Override> = overrides.iter().map(|o| o.base).collect();
        for source in 0..self.overrides.len() {
            let object = &self.table.objects[source];
            let label = &self.table.labels[source];
            let mut whole = None;
            let mut specific = None;
            for item in overrides {
                if item.base.object_id != object.as_slice() {
                    continue;
                }
                if item.base.speaker_label.is_empty() {
                    whole = Some(item);
                } else if !label.is_empty()
                    && routing::canonical_label(item.base.speaker_label) == *label
                {
                    specific = Some(item);
                }
            }
            let mut value = live_value(specific.or(whole));
            let head = routing::live_head_locked(&bases, object, label);
            value.has_head = u32::from(head.is_some());
            value.head = u32::from(head.unwrap_or(false));
            self.overrides[source] = value;
        }
        if let Err(e) = self.runtime.overrides(&self.overrides) {
            self.error = Some(runtime_error(e));
        }
    }

    pub fn set_pose(&mut self, pose: [f32; 3]) {
        if let Err(e) = self.runtime.pose(pose) {
            self.error = Some(runtime_error(e));
        }
    }
}

fn runtime_error(e: routing::Error) -> Error {
    runtime(e)
}

/// `room_internal::polar` through the room boundary, with its default text and context.
fn polar(
    scope: Scope,
    azimuth: f32,
    elevation: f32,
    distance: f32,
) -> Result<[f32; 3], routing::Error> {
    let mut out = [0.; 3];
    scope(&mut || {
        out = loading::polar(azimuth, elevation, distance)?;
        Ok(())
    })
    .map_err(|(code, _)| routing::Error {
        code,
        message: "房间 DSP 操作失败".as_bytes().to_vec(),
        context: b"room-binaural".to_vec(),
    })?;
    Ok(out)
}

fn live_error(message: &str) -> routing::Error {
    routing::Error {
        code: 1,
        message: message.as_bytes().to_vec(),
        context: Vec::new(),
    }
}

/// `convert`: a Scene state as the room session's state.
fn convert(s: &live::State) -> State {
    State {
        valid: s.valid_fields,
        active: u32::from(s.active),
        channel_lock: u32::from(s.channel_lock),
        screen_reference: u32::from(s.screen_reference),
        head_locked: u32::from(s.head_locked),
        gain: s.linear_gain,
        position: s.position,
        extent: [s.width, s.height, s.depth],
        diffuse: s.diffuse,
        divergence: s.divergence,
        divergence_range: [s.divergence_azimuth_range, s.divergence_position_range],
        has_max_distance: u32::from(s.channel_lock_max_distance.is_some()),
        max_distance: s.channel_lock_max_distance.unwrap_or(0.),
    }
}

pub const FRONT_WARNING: &str = "未知扬声器标签，room-binaural 使用正前方";
pub const SCREEN_WARNING: &str = "room-binaural 未配置重放屏幕，screenRef 保留原方向";

/// The Scene stream room renderer.
pub struct Live {
    runtime: Runtime,
    scope: Scope,
    screens: RoomScreens,
    generation: u64,
    index: HashMap<u64, u32>,
    sources: usize,
    planes: Vec<Option<usize>>,
    initial: Vec<RoomCommand>,
    events: Vec<RoomCommand>,
    error: Option<routing::Error>,
    ended: bool,
    warned_screen: bool,
    /// Whether the caller has a diagnostics sink: without one, as in C++, an unknown label keeps
    /// the element's own position and no warning is raised.
    sink: bool,
    diagnostics: Vec<Diagnostic>,
}

impl Live {
    pub fn new(
        resources: Arc<Resources>,
        config: Config,
        spread: bool,
        screens: RoomScreens,
        sink: bool,
        scope: Scope,
        prepare: Prepare,
    ) -> Result<Self, routing::Error> {
        Ok(Self {
            runtime: Runtime::create(resources, config, true, spread, scope, prepare)?,
            scope,
            screens,
            generation: 0,
            index: HashMap::new(),
            sources: 0,
            planes: Vec::new(),
            initial: Vec::with_capacity(512),
            events: Vec::with_capacity(4096),
            error: None,
            ended: false,
            warned_screen: false,
            sink,
            diagnostics: Vec::new(),
        })
    }

    pub fn control(&self) -> Control {
        self.runtime.control()
    }

    pub fn sample_rate(&self) -> u32 {
        self.runtime.sample_rate()
    }

    pub fn tail_frames(&self) -> u64 {
        self.runtime.tail_frames()
    }

    /// The warnings of the last call, also of a failed one: they were published as they arose.
    pub fn diagnostics(&self) -> &[Diagnostic] {
        &self.diagnostics
    }

    pub fn configure(
        &mut self,
        generation: u64,
        elements: &[Element],
    ) -> Result<(), routing::Error> {
        self.diagnostics.clear();
        let mut descriptions = Vec::with_capacity(elements.len());
        let mut index = HashMap::with_capacity(elements.len());
        for element in elements {
            let role = element
                .role
                .ok_or_else(|| live_error("room-binaural 收到无效声源"))?;
            let mut position = element.position;
            if !element.has_position && role == Role::DirectSpeaker {
                if let Some([azimuth, elevation]) =
                    routing::label_position(&[element.speaker_label])
                {
                    position = polar(self.scope, azimuth, elevation, 1.)?;
                } else if self.sink {
                    self.diagnostics.push(Diagnostic {
                        code: code::DIRECT_SPEAKER_FALLBACK,
                        element_id: element.id,
                        field: field::POSITION,
                        message: FRONT_WARNING,
                    });
                    position = [0., 1., 0.];
                }
            }
            index.insert(element.id, descriptions.len() as u32);
            descriptions.push(RoomDescription {
                id: element.id,
                generation,
                role: match role {
                    Role::Object => 0,
                    Role::DirectSpeaker => 1,
                    Role::Lfe => 2,
                },
                position,
                screens: self.screens,
                ..RoomDescription::default()
            });
        }
        self.runtime.configure(&descriptions)?;
        self.generation = generation;
        self.index = index;
        self.sources = elements.len();
        self.planes.resize(elements.len(), None);
        self.ended = false;
        Ok(())
    }

    pub fn reset(&mut self) {
        self.error = self.runtime.reset(0, true).err();
        self.index.clear();
        self.sources = 0;
        self.planes.clear();
        self.ended = false;
    }

    fn warn_screen(&mut self, state: &live::State, element: u64) {
        if state.valid_fields & field::SCREEN_REFERENCE != 0
            && state.screen_reference
            && self.screens.enabled == 0
            && !self.warned_screen
            && self.sink
        {
            self.diagnostics.push(Diagnostic {
                code: code::SEMANTIC_DEGRADED,
                element_id: element,
                field: field::SCREEN_REFERENCE,
                message: SCREEN_WARNING,
            });
            self.warned_screen = true;
        }
    }

    pub fn render<P: PlaneSource + ?Sized>(
        &mut self,
        frame: &FrameView<'_, P>,
        output: &mut [f32],
    ) -> Result<(), routing::Error> {
        self.diagnostics.clear();
        if let Some(e) = &self.error {
            return Err(e.clone());
        }
        if frame.generation != self.generation {
            return Err(live_error("room-binaural generation 不匹配"));
        }
        self.planes.clear();
        self.planes.resize(self.sources, None);
        self.initial.clear();
        self.events.clear();
        if !self.ended {
            for (j, plane) in frame.planes.iter().enumerate() {
                let Some(&i) = self.index.get(&plane.element_id) else {
                    return Err(live_error("room-binaural 收到未知声源 PCM"));
                };
                self.planes[i as usize] = plane.has_signal.then_some(j);
            }
            for entry in frame.initial {
                self.warn_screen(&entry.state, entry.element_id);
                let Some(&i) = self.index.get(&entry.element_id) else {
                    return Err(live_error("room-binaural 收到未知声源状态"));
                };
                self.initial.push(RoomCommand {
                    element: i,
                    state: convert(&entry.state),
                    ..RoomCommand::default()
                });
            }
            for update in frame.updates {
                self.warn_screen(&update.state, update.element_id);
                let Some(&i) = self.index.get(&update.element_id) else {
                    return Err(live_error("room-binaural 收到未知声源控制"));
                };
                self.events.push(RoomCommand {
                    element: i,
                    offset: update.offset,
                    duration: if update.jump { 0 } else { update.ramp },
                    changed: update.changed,
                    cleared: update.cleared,
                    state: convert(&update.state),
                    ..RoomCommand::default()
                });
            }
        }
        let planes = &self.planes;
        self.runtime.process(
            frame.duration as usize,
            planes
                .iter()
                .map(|p| p.map(|j| frame.planes.plane(j).samples)),
            &self.initial,
            &self.events,
            output,
        )
    }

    pub fn set_pose(&mut self, pose: [f32; 3]) {
        if let Err(e) = self.runtime.pose(pose) {
            self.error = Some(e);
        }
    }

    pub fn end_input(&mut self) -> Result<u64, routing::Error> {
        self.ended = true;
        self.runtime.end_input()
    }
}
