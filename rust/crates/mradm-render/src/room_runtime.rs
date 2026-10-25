//! The room session's runtime (PCM consistency 4.6, moved from the C++ `room_internal::Runtime`):
//! the render-worker session plus a preparation thread that builds switch candidates and disposes
//! of retired instances, so that neither preparation nor a destructor runs on the worker. The
//! request, epoch, candidate and disposal rules, `pump` and `publish` keep the C++ order.
//!
//! Every call that crossed the former C++/Rust boundary runs through the caller's [`Scope`], which
//! enters the room floating-point environment and catches panics as that boundary did; the
//! surrounding bookkeeping stays in the caller's environment.
use crate::routing::Error;
use mradm_dsp::room::control::{RoomCommand, RoomDescription, RoomOverride};
use mradm_dsp::room::loading::Package;
use mradm_dsp::room::quality::Quality;
use mradm_dsp::room::session::{Instance, PreparedSet, RoomInfo, Session};
use std::sync::{Arc, Condvar, Mutex, MutexGuard};
use std::thread::JoinHandle;

/// The former boundary of one call: its error code and static text, `(6, "Rust DSP panic")` for a
/// panic.
pub type Fault = (i32, &'static str);
pub type Scope = fn(&mut dyn FnMut() -> mradm_dsp::Result<()>) -> Result<(), Fault>;

/// A prepared room set and whether its realtime work runs on CUDA (`RoomPreparedHandle`).
pub struct Resources {
    pub set: Arc<PreparedSet>,
    pub cuda: bool,
}

/// `RoomConfig`: the package, the personal HRIR path (UTF-8, empty for the default), the
/// coordinate scale and the quality.
#[derive(Clone)]
pub struct Config {
    pub package: Arc<Package>,
    pub hrir: String,
    pub metres: f64,
    pub quality: Quality,
}

/// Prepares a switch candidate, pinned to `reuse`'s listener HRIR, inside its own boundary.
pub type Prepare = Arc<dyn Fn(&Config, &Resources) -> Result<Resources, Fault> + Send + Sync>;

/// `RoomSwitchState`.
pub mod state {
    pub const APPLIED: u32 = 0;
    pub const PREPARING: u32 = 1;
    pub const TRANSITIONING: u32 = 2;
    pub const DRAINING: u32 = 3;
    pub const FAILED: u32 = 4;
}

/// `RoomStatus`.
#[derive(Clone, Debug, PartialEq)]
pub struct Status {
    pub state: u32,
    pub requested_revision: u64,
    pub applied_revision: u64,
    pub transition_sample: u64,
    pub material_frames: u64,
    pub tail_frames: u64,
    pub buffer_frames: u64,
    pub material_filter_guard_frames: u64,
    pub constrained_sources: u32,
    pub message: String,
    pub quality: Quality,
    pub reflection_update_frames: u32,
}

/// `RoomSourcePosition`.
#[derive(Clone, Copy, Debug, Default, PartialEq)]
pub struct SourcePosition {
    pub element_id: u64,
    pub position: [f64; 3],
    pub direction: [f64; 3],
    pub distance: f64,
    pub constrained: bool,
    pub present: bool,
}

const DISPOSE: usize = 8;
const DEFAULT_TEXT: &str = "房间 DSP 操作失败";

/// Moving the queue to the preparer leaves another fixed-capacity queue on the worker.
#[derive(Default)]
struct Disposal {
    instances: [Option<Box<Instance>>; DISPOSE],
    count: usize,
}

impl Disposal {
    fn len(&self) -> usize {
        self.count
    }

    fn is_empty(&self) -> bool {
        self.count == 0
    }

    fn push(&mut self, instance: Box<Instance>) {
        let slot = &mut self.instances[self.count];
        debug_assert!(slot.is_none());
        *slot = Some(instance);
        self.count += 1;
    }
}

struct Request {
    config: Config,
    revision: u64,
    epoch: u64,
}

struct Candidate {
    instance: Option<Box<Instance>>,
    revision: u64,
    epoch: u64,
    preroll: u64,
    capacity: usize,
}

struct State {
    status: Status,
    request: Option<Request>,
    candidate: Option<Candidate>,
    /// Boxes as the session hands them over: parking moves a pointer, never an instance.
    dispose: Disposal,
    capacity: usize,
    epoch: u64,
    latest: u64,
    preparing: u64,
    failed: u64,
    preroll: u64,
    ended: bool,
    positions: Vec<SourcePosition>,
    stop: bool,
}

struct Shared {
    state: Mutex<State>,
    cv: Condvar,
    base: Config,
}

impl Shared {
    // A panic in a scoped call cannot leave the state half-written: keep using it.
    fn lock(&self) -> MutexGuard<'_, State> {
        self.state.lock().unwrap_or_else(|e| e.into_inner())
    }
}

/// `checked`: the boundary's text when the call had a message buffer, else the default.
fn checked(result: Result<(), Fault>, message: bool) -> Result<(), Error> {
    result.map_err(|(code, text)| Error {
        code,
        message: if message { text } else { DEFAULT_TEXT }
            .as_bytes()
            .to_vec(),
        context: b"room-binaural".to_vec(),
    })
}

/// One former boundary call returning a value.
fn call<T>(scope: Scope, mut f: impl FnMut() -> mradm_dsp::Result<T>) -> Result<T, Fault> {
    let mut out = None;
    scope(&mut || {
        out = Some(f()?);
        Ok(())
    })?;
    Ok(out.expect("the scope ran the call"))
}

fn separator(c: char) -> bool {
    c == '/' || (cfg!(windows) && c == '\\')
}

/// The elements `std::filesystem::path` compares: the root name (Windows `X:` or `\\host`),
/// whether a root directory follows, then the filenames. Repeated separators collapse; `.` and
/// `..` stay elements and a trailing separator adds an empty one.
fn elements(p: &str) -> (&str, bool, Vec<&str>) {
    let mut root = 0;
    if cfg!(windows) {
        let bytes = p.as_bytes();
        if bytes.len() >= 2 && bytes[1] == b':' && bytes[0].is_ascii_alphabetic() {
            root = 2;
        } else if bytes.len() > 2
            && separator(bytes[0] as char)
            && separator(bytes[1] as char)
            && !separator(bytes[2] as char)
        {
            root = p[2..].find(separator).map_or(p.len(), |i| i + 2);
        }
    }
    let (name, rest) = p.split_at(root);
    let directory = rest.starts_with(separator);
    let rest = rest.trim_start_matches(separator);
    let mut parts: Vec<&str> = rest.split(separator).filter(|e| !e.is_empty()).collect();
    if !rest.is_empty() && rest.ends_with(separator) {
        parts.push("");
    }
    (name, directory, parts)
}

/// `std::filesystem::path` equality.
fn same_path(a: &str, b: &str) -> bool {
    let ((an, ad, ae), (bn, bd, be)) = (elements(a), elements(b));
    an.replace('\\', "/") == bn.replace('\\', "/") && ad == bd && ae == be
}

/// The control-thread side (`IRoomControl`); it may outlive the runtime.
#[derive(Clone)]
pub struct Control(Arc<Shared>);

impl Control {
    pub fn request(&self, config: &Config) -> Result<u64, Error> {
        let base = &self.0.base;
        if config.quality != base.quality
            || config.metres != base.metres
            || (!config.hrir.is_empty() && !same_path(&config.hrir, &base.hrir))
        {
            return Err(Error {
                code: 1,
                message: "房间切换必须保持当前档位、HRIR 和坐标比例"
                    .as_bytes()
                    .to_vec(),
                context: Vec::new(),
            });
        }
        let mut s = self.0.lock();
        if s.ended {
            return Err(Error {
                code: 1,
                message: "节目已结束，不能再切换房间".as_bytes().to_vec(),
                context: Vec::new(),
            });
        }
        // Pin the active listener bank, independent of the package default.
        let selected = Config {
            hrir: String::new(),
            ..config.clone()
        };
        s.latest += 1;
        let revision = s.latest;
        let epoch = s.epoch;
        s.request = Some(Request {
            config: selected,
            revision,
            epoch,
        });
        s.preparing = revision;
        s.status.requested_revision = revision;
        s.status.state = state::PREPARING;
        s.status.message.clear();
        self.0.cv.notify_all();
        Ok(revision)
    }

    pub fn status(&self) -> Status {
        self.0.lock().status.clone()
    }

    pub fn source_position(&self, id: u64) -> SourcePosition {
        let s = self.0.lock();
        s.positions
            .iter()
            .find(|p| p.element_id == id)
            .copied()
            .unwrap_or(SourcePosition {
                element_id: id,
                ..SourcePosition::default()
            })
    }
}

/// The render-worker side: the session and the preparation thread.
pub struct Runtime {
    session: Session,
    shared: Arc<Shared>,
    sample_rate: u32,
    scope: Scope,
    preparer: Option<JoinHandle<()>>,
}

impl Runtime {
    pub fn create(
        resources: Arc<Resources>,
        config: Config,
        scene: bool,
        spread: bool,
        scope: Scope,
        prepare: Prepare,
    ) -> Result<Self, Error> {
        let set = Arc::clone(&resources.set);
        let session = call(scope, || {
            Session::new(Arc::clone(&set), config.metres, scene, spread)
        });
        let session = session.map_err(|f| checked(Err(f), true).unwrap_err())?;
        let rate = set.room.profile.sample_rate;
        let status = Status {
            state: state::APPLIED,
            requested_revision: 0,
            applied_revision: 0,
            transition_sample: 0,
            material_frames: 0,
            tail_frames: set.tail_frames() as u64,
            buffer_frames: 256,
            material_filter_guard_frames: set.room.profile.band_offset as u64,
            constrained_sources: 0,
            message: String::new(),
            quality: config.quality,
            reflection_update_frames: set.room.quality.reflection_frames_at(rate) as u32,
        };
        let shared = Arc::new(Shared {
            state: Mutex::new(State {
                status,
                request: None,
                candidate: None,
                dispose: Disposal::default(),
                capacity: 0,
                epoch: 0,
                latest: 0,
                preparing: 0,
                failed: 0,
                preroll: set.preroll_frames() as u64,
                ended: false,
                positions: Vec::new(),
                stop: false,
            }),
            cv: Condvar::new(),
            base: config,
        });
        let worker = Arc::clone(&shared);
        let preparer =
            std::thread::spawn(move || prepare_loop(&worker, &resources, scope, &prepare));
        Ok(Self {
            session,
            shared,
            sample_rate: rate,
            scope,
            preparer: Some(preparer),
        })
    }

    pub fn control(&self) -> Control {
        Control(Arc::clone(&self.shared))
    }

    pub fn sample_rate(&self) -> u32 {
        self.sample_rate
    }

    pub fn preroll_frames(&self) -> u64 {
        self.shared.lock().preroll
    }

    pub fn tail_frames(&self) -> u64 {
        self.shared.lock().status.tail_frames
    }

    fn info(&self) -> RoomInfo {
        let session = &self.session;
        call(self.scope, || Ok(session.info())).unwrap_or_default()
    }

    pub fn configure(&mut self, descriptions: &[RoomDescription]) -> Result<(), Error> {
        let session = &mut self.session;
        checked((self.scope)(&mut || session.configure(descriptions)), true)?;
        let info = self.info();
        {
            let mut s = self.shared.lock();
            s.positions.clear();
            s.positions
                .extend(descriptions.iter().map(|d| SourcePosition {
                    element_id: d.id,
                    ..SourcePosition::default()
                }));
        }
        self.publish(&info);
        Ok(())
    }

    pub fn reset(&mut self, sample: u64, clear: bool) -> Result<(), Error> {
        let session = &mut self.session;
        checked(
            (self.scope)(&mut || {
                session.reset(sample)?;
                if clear {
                    session.clear_generation();
                }
                Ok(())
            }),
            true,
        )?;
        {
            let mut s = self.shared.lock();
            s.epoch += 1;
            s.request = None;
            s.preparing = 0;
            s.failed = 0;
            s.ended = false;
            s.status.message.clear();
        }
        let info = self.info();
        self.publish(&info);
        self.shared.cv.notify_all();
        Ok(())
    }

    pub fn prime(&mut self, command: &RoomCommand, sample: u64) -> Result<(), Error> {
        let session = &mut self.session;
        checked((self.scope)(&mut || session.prime(command, sample)), true)
    }

    pub fn pose(&mut self, pose: [f32; 3]) -> Result<(), Error> {
        let session = &mut self.session;
        checked((self.scope)(&mut || session.set_pose(pose)), false)
    }

    pub fn overrides(&mut self, values: &[RoomOverride]) -> Result<(), Error> {
        let session = &mut self.session;
        checked((self.scope)(&mut || session.set_overrides(values)), true)
    }

    pub fn end_input(&mut self) -> Result<u64, Error> {
        let session = &mut self.session;
        checked(
            (self.scope)(&mut || {
                session.end_input();
                Ok(())
            }),
            false,
        )?;
        {
            let mut s = self.shared.lock();
            s.ended = true;
            s.epoch += 1;
            s.request = None;
            s.preparing = 0;
        }
        let info = self.info();
        self.publish(&info);
        self.shared.cv.notify_all();
        Ok(info.tail_frames)
    }

    pub fn process<'a, I>(
        &mut self,
        frames: usize,
        planes: I,
        initial: &[RoomCommand],
        events: &[RoomCommand],
        output: &mut [f32],
    ) -> Result<(), Error>
    where
        I: Iterator<Item = Option<&'a [f32]>> + Clone + ExactSizeIterator,
    {
        self.pump()?;
        let session = &mut self.session;
        let info = call(self.scope, || {
            session.process(frames, planes.clone(), initial, events, &mut *output)
        })
        .map_err(|f| checked(Err(f), true).unwrap_err())?;
        self.publish(&info);
        Ok(())
    }

    /// Hands a ready candidate to the session and parks displaced instances for disposal. A
    /// stalled preparer never forces a destructor onto the render worker.
    fn pump(&mut self) -> Result<(), Error> {
        let scope = self.scope;
        let session = &mut self.session;
        let mut s = self.shared.lock();
        if s.latest != 0 {
            let latest = s.latest;
            checked(scope(&mut || session.note_room_request(latest)), false)?;
        }
        if s.dispose.len() + 3 > DISPOSE {
            return Ok(());
        }
        if let Ok(Some(retired)) = call(scope, || Ok(session.take_retired())) {
            s.dispose.push(retired);
        }
        let ready = s.candidate.as_ref().is_some_and(|c| {
            c.epoch == s.epoch && c.revision == s.latest && c.capacity >= s.capacity
        }) && !s.ended;
        if ready {
            let mut candidate = s.candidate.take().expect("checked above");
            let revision = candidate.revision;
            let mut next = candidate.instance.take();
            let mut displaced: [Option<Box<Instance>>; 2] = [None, None];
            let queued = scope(&mut || {
                let Some(instance) = next.take() else {
                    return Err(mradm_dsp::Error::InvalidArgument(
                        "Invalid Scene DSP buffer or parameter",
                    ));
                };
                if let Err(e) = session.validate_room(&instance, revision) {
                    // Ownership returns for disposal off the render worker.
                    displaced[0] = Some(instance);
                    return Err(e);
                }
                displaced = session.queue_room(instance, revision)?;
                Ok(())
            });
            match queued {
                Err((_, text)) => {
                    s.failed = s.latest;
                    s.status.message = text.to_owned();
                }
                Ok(()) => s.preroll = s.preroll.max(candidate.preroll),
            }
            for instance in displaced.into_iter().flatten() {
                s.dispose.push(instance);
            }
            s.preparing = 0;
        }
        self.shared.cv.notify_all();
        Ok(())
    }

    fn publish(&self, info: &RoomInfo) {
        let mut s = self.shared.lock();
        if info.source_capacity as usize > s.capacity {
            self.shared.cv.notify_all();
        }
        s.capacity = info.source_capacity as usize;
        let status = &mut s.status;
        status.material_frames = info.input_frames;
        status.buffer_frames = info.latency_frames;
        status.tail_frames = info.tail_frames;
        status.material_filter_guard_frames = info.material_guard_frames;
        status.applied_revision = info.applied_request;
        status.transition_sample = info.transition_sample;
        status.constrained_sources = info.constrained_sources;
        status.state = match info.phase {
            1 => state::PREPARING,
            2 => state::TRANSITIONING,
            3 => state::DRAINING,
            _ => state::APPLIED,
        };
        if s.preparing == s.latest && s.preparing != 0 {
            s.status.state = state::PREPARING;
        }
        if s.failed == s.latest && s.failed != 0 {
            s.status.state = state::FAILED;
        }
        let session = &self.session;
        for position in &mut s.positions {
            let id = position.element_id;
            if let Ok(found) = call(self.scope, || Ok(session.source_position(id))) {
                *position = match found {
                    Some(p) => SourcePosition {
                        element_id: id,
                        position: p.point,
                        direction: p.direction,
                        distance: p.distance,
                        constrained: p.constrained,
                        present: true,
                    },
                    None => SourcePosition {
                        element_id: id,
                        ..SourcePosition::default()
                    },
                };
            }
        }
    }
}

impl Drop for Runtime {
    fn drop(&mut self) {
        self.shared.lock().stop = true;
        self.shared.cv.notify_all();
        if let Some(preparer) = self.preparer.take() {
            let _ = preparer.join();
        }
        // The session and resources outlive preparation; parked instances go with the runtime.
        let parked = std::mem::take(&mut self.shared.lock().dispose);
        drop(parked);
    }
}

fn stale(s: &State, capacity: usize) -> bool {
    s.candidate
        .as_ref()
        .is_some_and(|c| c.epoch != s.epoch || c.revision != s.latest || c.capacity < capacity)
}

fn prepare_loop(shared: &Shared, resources: &Resources, scope: Scope, prepare: &Prepare) {
    loop {
        let (queued, mut growing, resize, capacity, dispose) = {
            let s = shared.lock();
            let mut s = shared
                .cv
                .wait_while(s, |s| {
                    !(s.stop
                        || s.request.is_some()
                        || !s.dispose.is_empty()
                        || stale(s, s.capacity))
                })
                .unwrap_or_else(|e| e.into_inner());
            if s.stop {
                return;
            }
            let queued = s.request.take();
            let capacity = s.capacity;
            let dispose = std::mem::take(&mut s.dispose);
            let (mut growing, mut resize) = (None, false);
            if stale(&s, capacity) {
                let c = s.candidate.as_ref().expect("stale");
                resize = c.epoch == s.epoch && c.revision == s.latest;
                growing = s.candidate.take();
            }
            (queued, growing, resize, capacity, dispose)
        };
        // Destructors run here, outside the worker's mutex.
        drop(dispose);
        if queued.is_none() && !resize {
            continue;
        }
        let candidate;
        let result: Result<(), Error>;
        if let Some(queued) = queued {
            // Stale candidate disposal stays outside the worker's mutex.
            drop(growing.take());
            let prepared = prepare(&queued.config, resources).map_err(|(code, text)| Error {
                code,
                message: text.as_bytes().to_vec(),
                context: b"room-binaural".to_vec(),
            });
            let (instance, preroll) = match &prepared {
                Ok(next) => {
                    let set = Arc::clone(&next.set);
                    let created = call(scope, || Instance::new(Arc::clone(&set), capacity));
                    match created {
                        Ok(instance) => (
                            (Some(Box::new(instance)), Ok(())),
                            next.set.preroll_frames() as u64,
                        ),
                        Err(f) => (
                            (None, checked(Err(f), true)),
                            next.set.preroll_frames() as u64,
                        ),
                    }
                }
                Err(e) => ((None, Err(e.clone())), 0),
            };
            result = instance.1;
            candidate = Candidate {
                instance: instance.0,
                revision: queued.revision,
                epoch: queued.epoch,
                preroll,
                capacity,
            };
        } else if let Some(mut grown) = growing.take() {
            let reserved = match grown.instance.as_mut() {
                Some(instance) => scope(&mut || instance.reserve(capacity)),
                None => Err((1, "Invalid Scene DSP buffer or parameter")),
            };
            result = checked(reserved, true);
            grown.capacity = capacity;
            candidate = grown;
        } else {
            continue;
        }
        let mut s = shared.lock();
        if candidate.epoch != s.epoch || candidate.revision != s.latest {
            // The candidate is destroyed after the lock is released.
            drop(s);
            drop(candidate);
            continue;
        }
        match result {
            Err(e) => {
                s.failed = s.latest;
                s.preparing = 0;
                s.status.state = state::FAILED;
                s.status.message = String::from_utf8_lossy(&e.message).into_owned();
            }
            Ok(()) => s.candidate = Some(candidate),
        }
    }
}

#[cfg(test)]
mod tests {
    use super::*;
    use mradm_dsp::room::loading;
    use std::sync::mpsc;

    fn scope(call: &mut dyn FnMut() -> mradm_dsp::Result<()>) -> Result<(), Fault> {
        call().map_err(|_| (1, "failed"))
    }

    #[test]
    fn a_full_disposal_list_keeps_the_candidate_off_the_worker() {
        let manifest = concat!(
            env!("CARGO_MANIFEST_DIR"),
            "/../../../tests/fixtures/room/phase2/room.json"
        );
        let package = Arc::new(loading::load_package(manifest.as_bytes()).unwrap());
        let set = loading::prepare(&package, b"", b"", None, Quality::Exact).unwrap();
        let (started, wait_started) = mpsc::channel();
        let (open, opened) = mpsc::channel::<()>(); // closed on drop
        let opened = Mutex::new(opened);
        let prepare: Prepare = Arc::new(move |_: &Config, reuse: &Resources| {
            started.send(()).unwrap();
            let _ = opened.lock().unwrap().recv();
            Ok(Resources {
                set: Arc::clone(&reuse.set),
                cuda: false,
            })
        });
        let config = Config {
            package,
            hrir: String::new(),
            metres: 1.,
            quality: Quality::Exact,
        };
        let resources = Arc::new(Resources {
            set: Arc::clone(&set),
            cuda: false,
        });
        let mut runtime =
            Runtime::create(resources, config.clone(), false, true, scope, prepare).unwrap();
        // Dropped before the runtime, also by a failed assertion: the preparer's wait ends first.
        let _open = open;
        runtime.control().request(&config).unwrap();
        // The preparation thread is busy and cannot drain the list.
        wait_started.recv().unwrap();
        let instance = || Box::new(Instance::new(Arc::clone(&set), 0).unwrap());
        {
            let mut s = runtime.shared.lock();
            for _ in 0..DISPOSE - 2 {
                s.dispose.push(instance());
            }
            s.candidate = Some(Candidate {
                instance: Some(instance()),
                revision: s.latest,
                epoch: s.epoch,
                preroll: 0,
                capacity: s.capacity,
            });
        }
        runtime.pump().unwrap();
        {
            let s = runtime.shared.lock();
            assert!(
                s.candidate.is_some(),
                "no queueing that could displace an instance"
            );
            assert_eq!(s.dispose.len(), DISPOSE - 2);
        }
    }

    #[test]
    fn paths_compare_by_their_elements() {
        for (a, b, equal) in [
            ("a/b", "a//b", true),
            ("/r/a/b", "/r/a/b", true),
            ("a/b/", "a/b//", true),
            ("a/b", "a/./b", false),
            ("a/b", "a/b/", false),
            ("a/../b", "b", false),
            ("/a", "a", false),
            ("", "", true),
        ] {
            assert_eq!(same_path(a, b), equal, "{a} {b}");
        }
    }
}
