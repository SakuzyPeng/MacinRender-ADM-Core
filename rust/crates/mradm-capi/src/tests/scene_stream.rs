//! The live Scene stream through its C ABI (from `scene_stream_c_api_test`): creation and backend
//! switches, the producer timeline and its validation, backpressure, pulls and status, resampling,
//! sample-accurate events, renderer-native roles, head tracking, semantic policies, binaural
//! geometry and Triple Balance.
use super::common::text;
use super::common::*;
use crate::scene::*;
use crate::string::adm_free_string;
use std::ffi::{CStr, c_char};
use std::ptr;
use std::time::{Duration, Instant};

const SPEAKERS: u32 = 6;

const STATE_ACTIVE: u64 = 1 << 0;
const STATE_LINEAR_GAIN: u64 = 1 << 1;
const STATE_POSITION: u64 = 1 << 2;
const STATE_EXTENT: u64 = 1 << 3;
const STATE_DIFFUSE: u64 = 1 << 4;
const STATE_DIVERGENCE: u64 = 1 << 5;
const STATE_CHANNEL_LOCK: u64 = 1 << 6;
const STATE_SCREEN_REFERENCE: u64 = 1 << 7;
const STATE_HEAD_LOCKED: u64 = 1 << 8;
const STATE_DIVERGENCE_RANGE: u64 = 1 << 9;
const STATE_CHANNEL_LOCK_MAX_DISTANCE: u64 = 1 << 10;
const FRAME_STATE_COMPLETE: u32 = 1 << 0;
const FRAME_WARMUP: u32 = 1 << 1;
const FRAME_DISCONTINUITY: u32 = 1 << 2;
const PULL_BUFFERING: u32 = 1 << 0;
const PULL_UNDERRUN: u32 = 1 << 1;
const PULL_EOS: u32 = 1 << 2;
const PULL_FAILED: u32 = 1 << 3;
const ELEMENT_OBJECT: i32 = 0;
const ELEMENT_DIRECT_SPEAKER: i32 = 1;
const ELEMENT_LFE: i32 = 2;
const ACCEPTED: i32 = 0;
const WOULD_BLOCK: i32 = 1;
const TIMED_OUT: i32 = 2;
const CLOSED: i32 = 3;
const STREAM_BUFFERING: i32 = 1;
const STREAM_RUNNING: i32 = 2;
const STREAM_DRAINING: i32 = 3;
const STREAM_ENDED: i32 = 4;
const DIAGNOSTIC_BACKEND_FAILURE: i32 = 6;
const SAF: i32 = 2;
const SAF_BINAURAL: i32 = 6;
const TRIPLE_BALANCE: i32 = 7;

/// SAF prepares floating-point HRTF state independently for each stream, so equivalent geometry
/// can differ by a few float ULPs; one order of magnitude below the 1e-6 distinctness checks.
const BINAURAL_EQUIVALENCE: f32 = 1.0e-7;

fn zeroed<T>() -> T {
    // SAFETY: the C ABI records are plain data whose all-zero value is valid (null pointers).
    unsafe { std::mem::zeroed() }
}

fn sized<T>() -> T {
    let mut value: T = zeroed();
    // SAFETY: every record starts with its `u32 struct_size`.
    unsafe {
        ptr::from_mut(&mut value)
            .cast::<u32>()
            .write(size_of::<T>() as u32)
    };
    value
}

fn renderer(layout: &'static CStr) -> adm_scene_renderer_config_t {
    let mut config: adm_scene_renderer_config_t = sized();
    config.renderer = SAF;
    config.output_layout = layout.as_ptr();
    config
}

fn binaural(sofa: Option<&CStr>) -> adm_scene_renderer_config_t {
    let mut config = renderer(c"binaural");
    config.renderer = SAF_BINAURAL;
    config.sofa_path = sofa.map_or(ptr::null(), CStr::as_ptr);
    config
}

fn stream_config(input: u32, output: u32) -> adm_scene_stream_config_t {
    let mut config: adm_scene_stream_config_t = sized();
    config.rendering = renderer(c"0+5+0");
    config.input_sample_rate = input;
    config.output_sample_rate = output;
    config.input_queue_samples = 4096;
    config.input_queue_bytes = 1024 * 1024;
    config.output_ring_frames = 2048;
    config.startup_watermark_frames = 1;
    config
}

fn config() -> adm_scene_stream_config_t {
    stream_config(48000, 48000)
}

fn binaural_config(ring: u32) -> adm_scene_stream_config_t {
    let mut config = config();
    config.rendering = binaural(None);
    config.output_ring_frames = ring;
    config
}

fn complete_state(gain: f32) -> adm_scene_object_state_t {
    let mut state: adm_scene_object_state_t = sized();
    state.valid_fields = STATE_ACTIVE
        | STATE_LINEAR_GAIN
        | STATE_POSITION
        | STATE_EXTENT
        | STATE_DIFFUSE
        | STATE_DIVERGENCE
        | STATE_CHANNEL_LOCK
        | STATE_SCREEN_REFERENCE
        | STATE_HEAD_LOCKED
        | STATE_DIVERGENCE_RANGE
        | STATE_CHANNEL_LOCK_MAX_DISTANCE;
    state.active = 1;
    state.linear_gain = gain;
    state.position_y = 1.0;
    state.divergence_azimuth_range = 45.0;
    state
}

/// A stream, destroyed on drop.
struct Stream(*mut adm_scene_stream_t);

impl Stream {
    fn new(context: &Context, config: &adm_scene_stream_config_t) -> Self {
        let mut stream = ptr::null_mut();
        assert_eq!(
            unsafe { adm_create_scene_stream(context.0, config, &mut stream) },
            OK
        );
        assert!(!stream.is_null());
        Self(stream)
    }

    fn begin(&self, epoch: u64, target: i64) -> i32 {
        unsafe { adm_scene_stream_begin_epoch(self.0, epoch, target) }
    }

    fn configure(
        &self,
        epoch: u64,
        generation: u64,
        descriptors: &[adm_scene_element_descriptor_t],
    ) -> i32 {
        unsafe {
            adm_scene_stream_configure_generation(
                self.0,
                epoch,
                generation,
                descriptors.as_ptr(),
                descriptors.len() as u32,
            )
        }
    }

    /// The C++ `configure_object`: one object (element 7) at (0, 1, `z`), described by a record
    /// larger than this build's, as a newer caller would pass.
    fn configure_object(&self, epoch: u64, generation: u64, z: f32) {
        #[repr(C)]
        struct Forward {
            descriptor: adm_scene_element_descriptor_t,
            future_field: u64,
        }
        let mut forward: Forward = zeroed();
        forward.descriptor.struct_size = size_of::<Forward>() as u32;
        forward.descriptor.role = ELEMENT_OBJECT;
        forward.descriptor.element_id = 7;
        forward.descriptor.has_position = 1;
        forward.descriptor.position_y = 1.0;
        forward.descriptor.position_z = z;
        let code = unsafe {
            adm_scene_stream_configure_generation(self.0, epoch, generation, &forward.descriptor, 1)
        };
        assert_eq!(code, OK);
    }

    fn submit(&self, frame: &adm_scene_frame_t, timeout_ms: u32) -> (i32, i32) {
        let mut result = -1;
        let code = unsafe { adm_scene_stream_submit_frame(self.0, frame, timeout_ms, &mut result) };
        (code, result)
    }

    fn accept(&self, frame: &adm_scene_frame_t) {
        assert_eq!(self.submit(frame, 0), (OK, ACCEPTED));
    }

    fn end(&self, epoch: u64, end: i64) {
        assert_eq!(
            unsafe { adm_scene_stream_signal_end(self.0, epoch, end) },
            OK
        );
    }

    fn status(&self) -> adm_scene_stream_status_t {
        let mut status: adm_scene_stream_status_t = sized();
        assert_eq!(
            unsafe { adm_scene_stream_get_status(self.0, &mut status) },
            OK
        );
        status
    }

    fn format(&self) -> adm_scene_output_format_t {
        let mut format: adm_scene_output_format_t = sized();
        assert_eq!(
            unsafe { adm_scene_stream_get_output_format(self.0, &mut format) },
            OK
        );
        format
    }

    fn pull(&self, output: &mut [f32], frames: u32) -> adm_scene_pull_result_t {
        let mut pulled: adm_scene_pull_result_t = sized();
        assert_eq!(
            unsafe { adm_scene_stream_pull(self.0, output.as_mut_ptr(), frames, &mut pulled) },
            OK
        );
        pulled
    }

    fn policy(&self, json: Option<&CStr>, revision: u64) -> i32 {
        unsafe {
            adm_scene_stream_set_semantic_policy_json(
                self.0,
                json.map_or(ptr::null(), CStr::as_ptr),
                revision,
            )
        }
    }

    /// Installs a policy and waits for the worker to apply its revision.
    fn apply_policy(&self, json: Option<&CStr>, revision: u64) {
        assert_eq!(self.policy(json, revision), OK);
        self.wait(
            |s| s.semantic_policy_revision == revision,
            "the policy revision",
        );
    }

    fn orient(&self, yaw: f32, pitch: f32, roll: f32) {
        assert_eq!(
            unsafe { adm_scene_stream_set_listener_orientation(self.0, yaw, pitch, roll) },
            OK
        );
    }

    fn switch(&self, config: &adm_scene_renderer_config_t) -> i32 {
        unsafe { adm_scene_stream_switch_backend(self.0, config) }
    }

    /// Polls the status until `done`, for up to five seconds.
    fn wait(
        &self,
        done: impl Fn(&adm_scene_stream_status_t) -> bool,
        what: &str,
    ) -> adm_scene_stream_status_t {
        let deadline = Instant::now() + Duration::from_secs(5);
        loop {
            let status = self.status();
            if done(&status) {
                return status;
            }
            assert!(Instant::now() < deadline, "timed out waiting for {what}");
            std::thread::sleep(Duration::from_millis(1));
        }
    }

    fn diagnostics(&self) -> Vec<(i32, u64, u64, Option<String>)> {
        (0..unsafe { adm_scene_stream_log_count(self.0) })
            .filter_map(|i| {
                let mut d: adm_scene_diagnostic_t = sized();
                (unsafe { adm_scene_stream_log_entry(self.0, i, &mut d) } != 0).then(|| {
                    (d.code, d.epoch_id, d.generation_id, unsafe {
                        text(d.message)
                    })
                })
            })
            .collect()
    }

    fn count_messages(&self, needle: &str) -> usize {
        self.diagnostics()
            .iter()
            .filter(|d| d.3.as_deref().is_some_and(|m| m.contains(needle)))
            .count()
    }

    /// The C++ `wait_for_output`: pulls 64-frame blocks until EOS, checking that every non-media
    /// sample is zero; the media samples and whether any was audible.
    fn drain(&self, channels: u32, expected_frames: u64) -> (Vec<f32>, bool) {
        let mut captured = Vec::new();
        let mut frames = 0u64;
        let mut signal = false;
        for _ in 0..5000 {
            let mut output = vec![-99.0f32; 64 * channels as usize];
            let pulled = self.pull(&mut output, 64);
            frames += u64::from(pulled.media_frames);
            let media = pulled.media_frames as usize * channels as usize;
            signal |= output[..media].iter().any(|s| s.abs() > 1.0e-7);
            assert!(
                output[media..].iter().all(|&s| s == 0.0),
                "non-media samples are zero"
            );
            captured.extend_from_slice(&output[..media]);
            if pulled.flags & PULL_EOS != 0 {
                assert_eq!(
                    frames, expected_frames,
                    "the output length follows the rational timeline"
                );
                return (captured, signal);
            }
            if pulled.media_frames == 0 {
                std::thread::sleep(Duration::from_millis(1));
            }
        }
        panic!("the stream did not reach EOS");
    }

    /// The C++ `pull_open_stream`: exactly `frames` media frames of a healthy, open stream.
    fn take(&self, channels: u32, frames: u32) -> Vec<f32> {
        let mut captured = Vec::new();
        let mut got = 0u32;
        for _ in 0..5000 {
            if got == frames {
                break;
            }
            let requested = 64.min(frames - got);
            let mut output = vec![-99.0f32; (requested * channels) as usize];
            let pulled = self.pull(&mut output, requested);
            assert_eq!(
                pulled.flags & PULL_FAILED,
                0,
                "the open stream stays healthy"
            );
            captured.extend_from_slice(&output[..(pulled.media_frames * channels) as usize]);
            got += pulled.media_frames;
            if pulled.media_frames == 0 {
                std::thread::sleep(Duration::from_millis(1));
            }
        }
        assert_eq!(
            got, frames,
            "the open pull reaches the requested media frame"
        );
        captured
    }
}

impl Drop for Stream {
    fn drop(&mut self) {
        unsafe { adm_destroy_scene_stream(self.0) };
    }
}

/// The PCM, initial states and updates one frame points at; kept alive with it.
struct Frame {
    planes: Vec<adm_scene_pcm_plane_t>,
    initial: Vec<adm_scene_initial_state_t>,
    updates: Vec<adm_scene_metadata_update_t>,
    samples: Vec<Vec<f32>>,
    frame: adm_scene_frame_t,
}

impl Frame {
    /// The C++ `object_frame`: element 7 with `samples`, a complete initial state, state-complete.
    fn object(epoch: u64, generation: u64, start: i64, samples: Vec<f32>) -> Self {
        let mut initial: adm_scene_initial_state_t = sized();
        initial.element_id = 7;
        initial.state = complete_state(1.0);
        Self::elements(epoch, generation, start, vec![(7, samples)], vec![initial])
    }

    fn elements(
        epoch: u64,
        generation: u64,
        start: i64,
        signals: Vec<(u64, Vec<f32>)>,
        initial: Vec<adm_scene_initial_state_t>,
    ) -> Self {
        let duration = signals.first().map_or(0, |s| s.1.len() as u32);
        let mut planes = Vec::new();
        let mut samples = Vec::new();
        for (id, signal) in signals {
            let mut plane: adm_scene_pcm_plane_t = sized();
            plane.element_id = id;
            plane.sample_count = signal.len() as u32;
            plane.stride = 1;
            plane.has_signal = 1;
            planes.push(plane);
            samples.push(signal);
        }
        let mut frame: adm_scene_frame_t = sized();
        frame.flags = FRAME_STATE_COMPLETE;
        frame.epoch_id = epoch;
        frame.generation_id = generation;
        frame.media_sample_start = start;
        frame.duration_samples = duration;
        Self {
            planes,
            initial,
            updates: Vec::new(),
            samples,
            frame,
        }
    }

    fn without_initial(mut self) -> Self {
        self.initial.clear();
        self
    }

    fn state(&mut self) -> &mut adm_scene_object_state_t {
        &mut self.initial[0].state
    }

    /// The frame with its pointers bound to this storage.
    fn get(&mut self) -> &adm_scene_frame_t {
        for (plane, samples) in self.planes.iter_mut().zip(&self.samples) {
            if plane.has_signal != 0 {
                plane.samples = samples.as_ptr();
            }
        }
        let f = &mut self.frame;
        f.pcm_count = self.planes.len() as u32;
        f.pcm = self.planes.as_ptr();
        f.initial_state_count = self.initial.len() as u32;
        f.initial_states = if self.initial.is_empty() {
            ptr::null()
        } else {
            self.initial.as_ptr()
        };
        f.metadata_update_count = self.updates.len() as u32;
        f.metadata_updates = if self.updates.is_empty() {
            ptr::null()
        } else {
            self.updates.as_ptr()
        };
        &self.frame
    }
}

fn update(
    offset: u32,
    ramp: u32,
    changed: u64,
    state: adm_scene_object_state_t,
) -> adm_scene_metadata_update_t {
    let mut update: adm_scene_metadata_update_t = sized();
    update.element_id = 7;
    update.offset_samples = offset;
    update.ramp_duration_samples = ramp;
    update.changed_fields = changed;
    update.state = state;
    update
}

fn descriptor(role: i32, id: u64, label: Option<&'static CStr>) -> adm_scene_element_descriptor_t {
    let mut d: adm_scene_element_descriptor_t = sized();
    d.role = role;
    d.element_id = id;
    d.speaker_label = label.map_or(ptr::null(), CStr::as_ptr);
    d
}

fn max_difference(a: &[f32], b: &[f32]) -> f32 {
    if a.len() != b.len() {
        return f32::INFINITY;
    }
    a.iter()
        .zip(b)
        .map(|(x, y)| (x - y).abs())
        .fold(0.0, f32::max)
}

fn equivalent(a: &[f32], b: &[f32], tolerance: f32, what: &str) {
    assert_eq!(a.len(), b.len(), "{what}: output sizes");
    assert!(a.iter().chain(b).all(|s| s.is_finite()), "{what}: finite");
    let difference = max_difference(a, b);
    assert!(
        difference <= tolerance,
        "{what}: maximum difference {difference}"
    );
}

fn mean_tail(samples: &[f32], channels: usize, channel: Option<usize>, tail: usize) -> f32 {
    let frames = samples.len() / channels;
    let first = frames.saturating_sub(tail);
    let (mut sum, mut count) = (0.0f64, 0usize);
    for frame in first..frames {
        for c in 0..channels {
            if channel.is_none_or(|only| only == c) {
                sum += f64::from(samples[frame * channels + c].abs());
                count += 1;
            }
        }
    }
    if count == 0 {
        0.0
    } else {
        (sum / count as f64) as f32
    }
}

#[test]
fn speaker_stereo_is_rejected_by_every_creation_and_switch() {
    let context = Context::new();
    let mut config = config();
    config.rendering = binaural(None);
    let stream = Stream::new(&context, &config);
    assert_eq!(stream.begin(1, 0), OK);
    stream.configure_object(1, 7, 0.0);
    let mut frame = Frame::object(1, 7, 0, vec![0.1; 128]);
    stream.accept(frame.get());
    stream.take(2, 128);
    for renderer_id in [SAF, TRIPLE_BALANCE] {
        for geometry in [0, 1] {
            for layout in [c"0+2+0", c"stereo", c"2.0"] {
                let mut invalid = config;
                invalid.rendering = renderer(layout);
                invalid.rendering.renderer = renderer_id;
                invalid.rendering.speaker_geometry = geometry;
                for entry in 0..4 {
                    let mut rejected = ptr::null_mut();
                    let code = unsafe {
                        match entry {
                            0 => adm_create_scene_stream(context.0, &invalid, &mut rejected),
                            1 => adm_create_scene_stream_with_room(
                                context.0,
                                &invalid,
                                ptr::null(),
                                &mut rejected,
                            ),
                            2 => adm_create_scene_stream_with_room_ex(
                                context.0,
                                &invalid,
                                ptr::null(),
                                ptr::null(),
                                &mut rejected,
                            ),
                            _ => adm_create_scene_stream_with_room_quality(
                                context.0,
                                &invalid,
                                ptr::null(),
                                0,
                                ptr::null(),
                                &mut rejected,
                            ),
                        }
                    };
                    assert_eq!(code, UNSUPPORTED);
                    assert!(rejected.is_null());
                }
                assert_eq!(stream.switch(&invalid.rendering), UNSUPPORTED);
                assert!(
                    !unsafe { text(adm_scene_stream_last_error_message(stream.0)) }
                        .unwrap()
                        .is_empty()
                );
                let mut error: *mut c_char = ptr::null_mut();
                assert_eq!(
                    unsafe {
                        adm_scene_stream_switch_backend_ex(stream.0, &invalid.rendering, &mut error)
                    },
                    UNSUPPORTED
                );
                assert!(!unsafe { text(error) }.unwrap().is_empty());
                unsafe { adm_free_string(error) };
            }
        }
    }
    let status = stream.status();
    assert_eq!(stream.format().channels, 2);
    assert_eq!(
        (status.failed, status.epoch_id, status.generation_id),
        (0, 1, 7)
    );
    frame.frame.media_sample_start = 128;
    stream.accept(frame.get());
    stream.end(1, 256);
    assert!(stream.drain(2, 128).1);
}

#[test]
fn submitted_pcm_is_copied_and_the_timeline_is_checked() {
    let context = Context::new();
    let stream = Stream::new(&context, &config());
    let format = stream.format();
    assert_eq!(
        (format.sample_format, format.sample_rate, format.channels),
        (0, 48000, SPEAKERS)
    );
    assert_ne!(format.interleaved, 0);
    // A smaller caller record is written without touching a future tail.
    let mut prefix: adm_scene_output_format_t = zeroed();
    prefix.struct_size = std::mem::offset_of!(adm_scene_output_format_t, channels) as u32;
    prefix.channels = 0xA5A5_A5A5;
    assert_eq!(
        unsafe { adm_scene_stream_get_output_format(stream.0, &mut prefix) },
        OK
    );
    assert_eq!(
        (prefix.sample_format, prefix.sample_rate, prefix.channels),
        (0, 48000, 0xA5A5_A5A5)
    );

    assert_eq!(stream.begin(1, 100), OK);
    stream.configure_object(1, 11, 0.0);
    let mut frame = Frame::object(1, 11, 100, vec![0.25; 128]);
    stream.accept(frame.get());
    // The caller may overwrite its memory as soon as the submit returns.
    frame.samples[0].fill(0.0);
    frame.planes[0].samples = ptr::null();
    stream.end(1, 228);
    assert!(stream.drain(SPEAKERS, 128).1);

    assert_eq!(stream.begin(2, 1000), OK);
    stream.configure_object(2, 12, 0.0);
    let mut frame = Frame::object(2, 12, 1001, vec![0.1; 16]);
    assert_eq!(stream.submit(frame.get(), 0).0, INVALID_ARGUMENT);
    frame.frame.media_sample_start = 1000;
    assert_eq!(stream.submit(frame.get(), 0).0, OK);
    frame.frame.media_sample_start = 1017;
    assert_eq!(stream.submit(frame.get(), 0).0, INVALID_ARGUMENT);
    assert_eq!(stream.begin(2, 0), INVALID_ARGUMENT);
}

#[test]
fn bounded_queues_push_back_and_invalid_frames_are_rejected() {
    let context = Context::new();
    let mut c = config();
    c.input_queue_samples = 64;
    c.output_ring_frames = 1;
    let stream = Stream::new(&context, &c);
    assert_eq!(stream.begin(10, 0), OK);
    stream.configure_object(10, 1, 0.0);
    let mut frame = Frame::object(10, 1, 0, vec![0.2; 64]);
    stream.accept(frame.get());
    frame.frame.media_sample_start = 64;
    assert_eq!(stream.submit(frame.get(), 0), (OK, WOULD_BLOCK));
    assert_eq!(stream.submit(frame.get(), 3), (OK, TIMED_OUT));
    let mut oversized = Frame::object(10, 1, 64, vec![0.2; 65]);
    assert_eq!(stream.submit(oversized.get(), 0).0, INVALID_ARGUMENT);
    stream.end(10, 64);
    assert_eq!(stream.submit(frame.get(), 0), (OK, CLOSED));

    // A new epoch interrupts a worker blocked on the full output ring.
    assert_eq!(stream.begin(11, 0), OK);
    stream.configure_object(11, 1, 0.0);
    let mut frame = Frame::object(11, 1, 0, vec![0.1, 0.1, f32::NAN, 0.1]);
    let mut bad = *frame.get();
    bad.pcm = ptr::null();
    assert_eq!(stream.submit(&bad, 0).0, INVALID_ARGUMENT);
    let mut unknown = *frame.get();
    unknown.generation_id = 999;
    assert_eq!(stream.submit(&unknown, 0).0, INVALID_ARGUMENT);
    assert_eq!(
        stream.submit(frame.get(), 0).0,
        INVALID_ARGUMENT,
        "non-finite PCM"
    );
    frame.samples[0][2] = 0.1;
    frame.planes[0].sample_count = 3;
    assert_eq!(
        stream.submit(frame.get(), 0).0,
        INVALID_ARGUMENT,
        "wrong sample count"
    );
    frame.planes[0].sample_count = 4;
    for (gain, x) in [(f32::INFINITY, 0.0), (-0.1, 0.0), (1.0, 1.1)] {
        frame.state().linear_gain = gain;
        frame.state().position_x = x;
        assert_eq!(
            stream.submit(frame.get(), 0).0,
            INVALID_ARGUMENT,
            "state {gain} {x}"
        );
    }
    frame.state().linear_gain = 1.0;
    frame.state().position_x = 0.0;
    // has_signal = 0 accepts a null pointer as exact-duration silence.
    frame.planes[0].has_signal = 0;
    frame.planes[0].samples = ptr::null();
    stream.accept(frame.get());
    stream.end(11, 4);
    let (silent, signal) = stream.drain(SPEAKERS, 4);
    assert!(!signal && silent.iter().all(|&s| s == 0.0));

    let mut bytes = config();
    bytes.input_queue_bytes = 64;
    let limited = Stream::new(&context, &bytes);
    assert_eq!(limited.begin(12, 0), OK);
    limited.configure_object(12, 1, 0.0);
    let mut frame = Frame::object(12, 1, 0, vec![0.1; 4]);
    assert_eq!(limited.submit(frame.get(), 0).0, INVALID_ARGUMENT);
}

#[test]
fn resampled_streams_keep_the_rational_length() {
    let context = Context::new();
    for (epoch, rate, expected) in [(20, 44100, 441), (21, 96000, 960)] {
        let stream = Stream::new(&context, &stream_config(48000, rate));
        assert_eq!(stream.begin(epoch, 0), OK);
        stream.configure_object(epoch, 1, 0.0);
        stream.accept(Frame::object(epoch, 1, 0, vec![0.1; 480]).get());
        stream.end(epoch, 480);
        assert!(stream.drain(SPEAKERS, expected).1);
    }
    for input in [8000u32, 44100, 96000, 192000] {
        let mut c = stream_config(input, 48000);
        c.rendering.renderer = SAF_BINAURAL;
        c.rendering.output_layout = c"binaural".as_ptr();
        let stream = Stream::new(&context, &c);
        assert_eq!(stream.begin(50, 0), OK);
        stream.configure_object(50, 1, 0.0);
        stream.accept(Frame::object(50, 1, 0, vec![0.01; 1025]).get());
        stream.end(50, 1025);
        let expected = (1025u64 * 48000).div_ceil(u64::from(input));
        let (output, signal) = stream.drain(2, expected);
        assert!(signal && output.len() as u64 == expected * 2);
        assert!(output.iter().all(|s| s.is_finite()));
    }
}

/// Hidden preroll before the target never leaks into the audible prefix through the resampler.
#[test]
fn resampled_preroll_stays_hidden() {
    let context = Context::new();
    let mut c = stream_config(48000, 44100);
    c.output_ring_frames = 512;
    let stream = Stream::new(&context, &c);
    assert_eq!(stream.begin(32, 0), OK);
    stream.configure_object(32, 1, 0.0);
    let mut first = Frame::object(32, 1, -512, vec![-1.0; 256]);
    first.frame.flags |= FRAME_DISCONTINUITY;
    stream.accept(first.get());
    let mut crossing = vec![-1.0f32; 416];
    crossing[256..].fill(1.0);
    stream.accept(Frame::object(32, 1, -256, crossing).without_initial().get());
    stream.end(32, 160);
    let (output, signal) = stream.drain(SPEAKERS, 147);
    assert!(signal && output.len() == 147 * SPEAKERS as usize);
    let leading: f32 =
        output[..16 * SPEAKERS as usize].iter().sum::<f32>() / (16 * SPEAKERS) as f32;
    assert!(leading > 0.0);
}

#[test]
fn many_irregular_frames_keep_one_rational_timeline() {
    let context = Context::new();
    let mut c = stream_config(48000, 44100);
    c.input_queue_samples = 65536;
    c.input_queue_bytes = 4 * 1024 * 1024;
    c.output_ring_frames = 50000;
    let stream = Stream::new(&context, &c);
    assert_eq!(stream.begin(22, 0), OK);
    stream.configure_object(22, 1, 0.0);
    let total = 48001i64;
    let mut start = 0i64;
    while start < total {
        let duration = 127.min(total - start) as usize;
        let mut frame = Frame::object(22, 1, start, vec![0.05; duration]);
        if start != 0 {
            frame = frame.without_initial();
        }
        assert_eq!(stream.submit(frame.get(), 1000), (OK, ACCEPTED));
        start += duration as i64;
    }
    stream.end(22, total);
    let (output, signal) = stream.drain(SPEAKERS, 44101);
    assert!(signal && output.iter().all(|s| s.is_finite()));
    let status = stream.status();
    assert_eq!((status.media_frames_pulled, status.failed), (44101, 0));
    assert_ne!(status.ended, 0);
}

#[test]
fn generation_topologies_are_immutable_and_strided_by_the_first_element() {
    let context = Context::new();
    let stream = Stream::new(&context, &config());
    assert_eq!(stream.begin(23, 0), OK);
    let mut d = descriptor(ELEMENT_OBJECT, 7, None);
    d.has_position = 1;
    d.position_y = 1.0;
    assert_eq!(stream.configure(23, 5, &[d]), OK);
    assert_eq!(
        stream.configure(23, 5, &[d]),
        OK,
        "an identical repeat is idempotent"
    );
    let mut moved = d;
    moved.position_x = 0.25;
    assert_eq!(stream.configure(23, 5, &[moved]), INVALID_ARGUMENT);
    assert_eq!(
        stream.configure(23, 6, &[d, d]),
        INVALID_ARGUMENT,
        "duplicate element IDs"
    );
    #[repr(C)]
    #[derive(Clone, Copy)]
    struct Extended {
        descriptor: adm_scene_element_descriptor_t,
        future_field: u64,
    }
    let mut inconsistent = [Extended {
        descriptor: d,
        future_field: 0,
    }; 2];
    inconsistent[0].descriptor.struct_size = size_of::<Extended>() as u32;
    inconsistent[0].descriptor.element_id = 8;
    inconsistent[1].descriptor.element_id = 9;
    assert_eq!(
        unsafe {
            adm_scene_stream_configure_generation(stream.0, 23, 7, &inconsistent[0].descriptor, 2)
        },
        INVALID_ARGUMENT
    );
    assert_eq!(
        stream.configure(999, 6, &[d]),
        INVALID_ARGUMENT,
        "an unknown epoch"
    );

    assert_eq!(stream.begin(33, 0), OK);
    stream.configure_object(33, 0, 0.0);
    stream.accept(Frame::object(33, 0, 0, vec![0.25; 64]).get());
    stream.end(33, 64);
    assert!(stream.drain(SPEAKERS, 64).1, "generation zero renders");
}

/// A gain ramp starts at its exact input-sample offset and runs across frames; same-offset updates
/// apply in array order. The Live VBAP samples are exact.
#[test]
fn metadata_ramps_are_sample_accurate() {
    let context = Context::new();
    let mut c = config();
    c.output_ring_frames = 1024;
    let stream = Stream::new(&context, &c);
    assert_eq!(stream.begin(24, 0), OK);
    stream.configure_object(24, 1, 0.0);
    let mut preroll = Frame::object(24, 1, -512, vec![1.0; 512]);
    preroll.frame.flags |= FRAME_DISCONTINUITY;
    stream.accept(preroll.get());
    let mut first = Frame::object(24, 1, 0, vec![1.0; 4]).without_initial();
    first
        .updates
        .push(update(2, 4, STATE_LINEAR_GAIN, complete_state(0.0)));
    first.frame.flags |= FRAME_DISCONTINUITY;
    assert_eq!(
        stream.submit(first.get(), 0).0,
        INVALID_ARGUMENT,
        "a later discontinuity needs an epoch"
    );
    first.frame.flags &= !FRAME_DISCONTINUITY;
    stream.accept(first.get());
    stream.accept(
        Frame::object(24, 1, 4, vec![1.0; 4])
            .without_initial()
            .get(),
    );
    let mut ordered = Frame::object(24, 1, 8, vec![1.0; 1]).without_initial();
    ordered
        .updates
        .push(update(0, 0, STATE_LINEAR_GAIN, complete_state(0.25)));
    ordered
        .updates
        .push(update(0, 0, STATE_LINEAR_GAIN, complete_state(0.5)));
    stream.accept(ordered.get());
    stream.end(24, 9);
    let (output, signal) = stream.drain(SPEAKERS, 9);
    assert!(signal && output.len() == 9 * SPEAKERS as usize);
    let envelope = [1.0f32, 1.0, 1.0, 0.75, 0.5, 0.25, 0.0, 0.0, 0.5];
    assert!(output[2] > 0.0);
    for (f, gain) in envelope.iter().enumerate() {
        for ch in 0..SPEAKERS as usize {
            let expected = output[ch] * gain;
            assert_eq!(
                output[f * SPEAKERS as usize + ch].to_bits(),
                expected.to_bits(),
                "frame {f} channel {ch}"
            );
        }
    }
}

#[test]
fn incomplete_and_warmup_frames_stay_silent() {
    let context = Context::new();
    let mut c = config();
    c.output_ring_frames = 1024;
    let stream = Stream::new(&context, &c);
    assert_eq!(stream.begin(26, 0), OK);
    stream.configure_object(26, 1, 0.0);
    stream.accept(Frame::object(26, 1, -512, vec![1.0; 512]).get());
    stream.configure_object(26, 2, 0.0);
    let mut incomplete = Frame::object(26, 2, 0, vec![1.0; 16]);
    incomplete.frame.flags = 0;
    stream.accept(incomplete.get());
    let mut warmup = Frame::object(26, 2, 16, vec![1.0; 16]).without_initial();
    warmup.frame.flags = FRAME_STATE_COMPLETE | FRAME_WARMUP;
    stream.accept(warmup.get());
    stream.end(26, 32);
    let (output, signal) = stream.drain(SPEAKERS, 32);
    assert!(!signal && output.iter().all(|&s| s == 0.0));
}

#[test]
fn pulls_buffer_underrun_and_end_without_blocking() {
    let context = Context::new();
    let mut c = config();
    c.output_ring_frames = 128;
    c.startup_watermark_frames = 32;
    let stream = Stream::new(&context, &c);
    assert_eq!(stream.begin(27, 0), OK);
    stream.configure_object(27, 1, 0.0);
    let mut silence = vec![-1.0f32; 16 * SPEAKERS as usize];
    let pulled = stream.pull(&mut silence, 16);
    assert_eq!((pulled.media_frames, pulled.first_media_frame), (0, 0));
    assert_ne!(pulled.flags & PULL_BUFFERING, 0);
    assert!(silence.iter().all(|&s| s == 0.0));
    stream.accept(Frame::object(27, 1, 0, vec![0.2; 64]).get());
    stream.wait(|s| s.buffered_output_frames >= 64, "the startup watermark");
    let mut output = vec![-1.0f32; 128 * SPEAKERS as usize];
    let pulled = stream.pull(&mut output, 128);
    assert_eq!((pulled.media_frames, pulled.first_media_frame), (64, 0));
    assert_ne!(pulled.flags & PULL_UNDERRUN, 0);
    let media = 64 * SPEAKERS as usize;
    assert!(output[..media].iter().any(|&s| s != 0.0) && output[media..].iter().all(|&s| s == 0.0));
    stream.end(27, 64);
    let deadline = Instant::now() + Duration::from_secs(5);
    let pulled = loop {
        let mut tail = [0.0f32; 16 * SPEAKERS as usize];
        let pulled = stream.pull(&mut tail, 16);
        if pulled.flags & PULL_EOS != 0 {
            break pulled;
        }
        assert!(Instant::now() < deadline);
        std::thread::sleep(Duration::from_millis(1));
    };
    assert_eq!((pulled.first_media_frame, pulled.media_frames), (64, 0));
    let status = stream.status();
    assert!(
        status.ended != 0
            && status.failed == 0
            && status.media_frames_pulled == 64
            && status.underruns >= 1
    );
}

#[test]
fn an_unrenderable_direct_speaker_fails_the_worker_with_a_diagnostic() {
    let context = Context::new();
    let mut c = config();
    c.output_ring_frames = 64;
    let stream = Stream::new(&context, &c);
    assert_eq!(stream.begin(28, 0), OK);
    let d = descriptor(ELEMENT_DIRECT_SPEAKER, 42, Some(c"UNKNOWN-LIVE-SPEAKER"));
    assert_eq!(stream.configure(28, 1, &[d]), OK);
    let mut initial: adm_scene_initial_state_t = sized();
    initial.element_id = 42;
    initial.state = sized();
    initial.state.valid_fields = STATE_ACTIVE | STATE_LINEAR_GAIN;
    initial.state.active = 1;
    initial.state.linear_gain = 1.0;
    let mut frame = Frame::elements(28, 1, 0, vec![(42, vec![0.2; 8])], vec![initial]);
    stream.accept(frame.get());
    stream.wait(|s| s.failed != 0, "the worker failure");
    let mut output = [-1.0f32; 16 * SPEAKERS as usize];
    let pulled = stream.pull(&mut output, 16);
    assert_eq!(pulled.media_frames, 0);
    assert_ne!(pulled.flags & PULL_FAILED, 0);
    assert!(output.iter().all(|&s| s == 0.0));
    assert!(
        stream
            .diagnostics()
            .iter()
            .any(|d| d.0 == DIAGNOSTIC_BACKEND_FAILURE && d.1 == 28)
    );
}

/// Objects, DirectSpeakers and LFE render together with an update ramp crossing the frame; the
/// best-effort degradation is diagnosed with its timeline identity.
#[test]
fn renderer_native_roles_render_with_events() {
    let context = Context::new();
    let stream = Stream::new(&context, &config());
    assert_eq!(stream.begin(25, 0), OK);
    let mut object = descriptor(ELEMENT_OBJECT, 1, None);
    object.has_position = 1;
    object.position_y = 1.0;
    let descriptors = [
        object,
        descriptor(ELEMENT_DIRECT_SPEAKER, 2, Some(c"M+030")),
        descriptor(ELEMENT_LFE, 3, Some(c"LFE1")),
    ];
    assert_eq!(stream.configure(25, 9, &descriptors), OK);
    let initial: Vec<_> = (1..=3u64)
        .map(|id| {
            let mut s: adm_scene_initial_state_t = sized();
            s.element_id = id;
            s.state = complete_state(1.0);
            s
        })
        .collect();
    let mut frame = Frame::elements(
        25,
        9,
        0,
        vec![
            (1, vec![0.05; 128]),
            (2, vec![0.03; 128]),
            (3, vec![0.02; 128]),
        ],
        initial,
    );
    frame.initial[0].state.extent_width = 0.25;
    frame.initial[0].state.diffuse = 0.2;
    let mut target = complete_state(0.5);
    target.position_x = 0.5;
    target.extent_width = 0.5;
    target.extent_height = 0.25;
    target.diffuse = 0.4;
    target.divergence = 0.3;
    let mut u = update(
        17,
        200,
        STATE_LINEAR_GAIN | STATE_POSITION | STATE_EXTENT | STATE_DIFFUSE | STATE_DIVERGENCE,
        target,
    );
    u.element_id = 1;
    frame.updates.push(u);
    stream.accept(frame.get());
    stream.end(25, 128);
    assert!(stream.drain(6, 128).1);
    let diagnostics = stream.diagnostics();
    assert!(!diagnostics.is_empty());
    let first = &diagnostics[0];
    assert!(first.3.is_some() && first.1 == 25 && first.2 == 9);
}

fn tracking_startup(context: &Context, resume: bool) {
    let mut c = config();
    c.output_ring_frames = 8192;
    c.startup_watermark_frames = 4096;
    let stream = Stream::new(context, &c);
    assert_eq!(stream.begin(1, 0), OK);
    stream.configure_object(1, 1, 0.0);
    let mut frame = Frame::object(1, 1, 0, vec![0.05; 2048]);
    stream.accept(frame.get());
    let status = stream.wait(|s| s.buffered_output_frames == 2048, "the partial prefill");
    assert_eq!(
        status.state, STREAM_BUFFERING,
        "untracked startup waits for 4096 frames"
    );
    stream.orient(3.0, 1.0, 0.0);
    if resume {
        frame.frame.media_sample_start = 2048;
        assert_eq!(stream.submit(frame.get(), 100), (OK, ACCEPTED));
        stream.wait(
            |s| s.state == STREAM_RUNNING,
            "readiness at the tracking cap",
        );
    }
    let mut output = [0.0f32; 64 * SPEAKERS as usize];
    assert_eq!(
        stream.pull(&mut output, 64).media_frames,
        64,
        "tracking lowers the watermark"
    );
}

#[test]
fn head_tracking_lowers_the_startup_watermark() {
    let context = Context::new();
    for _ in 0..8 {
        tracking_startup(&context, false);
        tracking_startup(&context, true);
    }
}

/// While the worker waits at a full ring after the end marker, the stream stays draining.
#[test]
fn an_end_marker_keeps_the_stream_draining() {
    let context = Context::new();
    let mut c = config();
    c.output_ring_frames = 64;
    let stream = Stream::new(&context, &c);
    assert_eq!(stream.begin(1, 0), OK);
    stream.configure_object(1, 1, 0.0);
    stream.accept(Frame::object(1, 1, 0, vec![0.05; 512]).get());
    stream.end(1, 512);
    stream.wait(|s| s.buffered_output_frames == 64, "the full ring");
    std::thread::sleep(Duration::from_millis(20));
    let status = stream.status();
    assert_eq!(
        (status.buffered_output_frames, status.state),
        (64, STREAM_DRAINING)
    );
    let mut frames = 0u64;
    let deadline = Instant::now() + Duration::from_secs(5);
    loop {
        let mut output = [0.0f32; 64 * SPEAKERS as usize];
        let pulled = stream.pull(&mut output, 64);
        frames += u64::from(pulled.media_frames);
        if pulled.flags & PULL_EOS != 0 {
            break;
        }
        assert_eq!(stream.status().state, STREAM_DRAINING);
        assert!(Instant::now() < deadline);
        if pulled.media_frames == 0 {
            std::thread::sleep(Duration::from_millis(1));
        }
    }
    assert_eq!((frames, stream.status().state), (512, STREAM_ENDED));
}

/// One binaural object at (`x`, 0) for 256 frames.
fn binaural_at(context: &Context, epoch: u64, x: f32) -> Vec<f32> {
    let stream = Stream::new(context, &binaural_config(1024));
    assert_eq!(stream.begin(epoch, 0), OK);
    stream.configure_object(epoch, 1, 0.0);
    let mut frame = Frame::object(epoch, 1, 0, vec![0.1; 256]);
    frame.state().position_x = x;
    frame.state().position_y = 0.0;
    stream.accept(frame.get());
    stream.end(epoch, 256);
    let (output, signal) = stream.drain(2, 256);
    assert!(signal);
    output
}

/// Binaural rendering follows the object state, and cached HRTFs share no tails between renderers.
#[test]
fn binaural_streams_follow_the_object_state() {
    let context = Context::new();
    let stream = Stream::new(&context, &binaural_config(1024));
    assert_eq!(stream.begin(30, 0), OK);
    stream.configure_object(30, 1, 0.0);
    stream.accept(Frame::object(30, 1, 0, vec![0.1; 128]).get());
    stream.end(30, 128);
    assert!(stream.drain(2, 128).1);

    let left = binaural_at(&context, 34, -1.0);
    let right = binaural_at(&context, 35, 1.0);
    assert!(!left.is_empty() && max_difference(&left, &right) > 1.0e-6);
    assert_eq!(binaural_at(&context, 36, -1.0), left);
}

fn sofa_source() -> Option<String> {
    std::env::var("MR_ADM_TEST_SOFA_PATH")
        .ok()
        .filter(|s| !s.is_empty())
}

/// A changed SOFA file is read again rather than served from the HRTF cache (with
/// `MR_ADM_TEST_SOFA_PATH`).
#[test]
fn a_changed_sofa_file_is_not_served_from_the_cache() {
    let Some(source) = sofa_source() else {
        return;
    };
    let context = Context::new();
    let dir = Dir::new("scene-sofa");
    let copy = dir.file("cached.sofa");
    std::fs::copy(&source, &copy).unwrap();
    let path = c(&copy);
    let mut c = config();
    c.rendering = binaural(Some(&path));
    let original = Stream::new(&context, &c);
    let _cached = Stream::new(&context, &c);
    std::fs::write(&copy, "invalid replacement").unwrap();
    let mut replaced = ptr::null_mut();
    assert_ne!(
        unsafe { adm_create_scene_stream(context.0, &c, &mut replaced) },
        OK
    );
    assert_eq!(original.begin(37, 0), OK);
}

/// Failed preparations keep the stream; rapid switches keep only the newest and the takeover
/// stays continuous, with generation, pose and policy retained.
#[test]
fn backend_hot_switches_are_continuous_and_keep_the_stream() {
    let context = Context::new();
    let stream = Stream::new(&context, &binaural_config(8192));
    assert_eq!(stream.begin(40, 0), OK);
    stream.configure_object(40, 1, 0.0);
    stream.apply_policy(
        Some(c"{\"schema\":\"mradm.semantic-policy.v1\",\"global\":{\"gain\":{\"scale\":1}}}"),
        71,
    );
    stream.orient(12.0, -4.0, 3.0);
    stream.accept(Frame::object(40, 1, 0, vec![0.05; 1024]).get());
    let mut output = stream.take(2, 1024);

    assert_ne!(
        stream.switch(&binaural(Some(
            c"/definitely/missing/mradm-scene-stream-test.sofa"
        ))),
        OK
    );
    let mut vbap_with_sofa = renderer(c"0+5+0");
    vbap_with_sofa.sofa_path = c"/definitely/missing/unused-vbap.sofa".as_ptr();
    assert_eq!(stream.switch(&vbap_with_sofa), UNSUPPORTED);
    assert_eq!(
        stream.switch(&renderer(c"0+5+0")),
        UNSUPPORTED,
        "a channel-count change"
    );
    let kept = stream.status();
    assert_eq!(
        (
            kept.failed,
            kept.generation_id,
            kept.semantic_policy_revision
        ),
        (0, 1, 71)
    );
    if let Some(sofa) = sofa_source() {
        let sofa = std::ffi::CString::new(sofa).unwrap();
        assert_eq!(stream.switch(&binaural(Some(&sofa))), OK);
        std::thread::sleep(Duration::from_millis(10));
        assert_eq!(stream.switch(&binaural(None)), OK);
    }
    let plain = binaural(None);
    let mut cloud = binaural(None);
    cloud.binaural_spread_mode = 2;
    for config in [&plain, &cloud, &plain] {
        assert_eq!(stream.switch(config), OK);
    }
    stream.accept(
        Frame::object(40, 1, 1024, vec![0.05; 1024])
            .without_initial()
            .get(),
    );
    output.extend(stream.take(2, 1024));
    assert_eq!(
        stream.switch(&cloud),
        OK,
        "a newer backend waits for the audible crossfade"
    );
    stream.accept(
        Frame::object(40, 1, 2048, vec![0.05; 4096])
            .without_initial()
            .get(),
    );
    stream.end(40, 6144);
    let (switched, signal) = stream.drain(2, 4096);
    output.extend(switched);
    assert!(signal && output.iter().all(|s| s.is_finite()));
    let step = output
        .windows(3)
        .map(|w| (w[2] - w[0]).abs())
        .fold(0.0, f32::max);
    assert!(step < 0.005, "maximum step {step}");
    let kept = stream.status();
    assert_eq!(
        (
            kept.failed,
            kept.generation_id,
            kept.semantic_policy_revision
        ),
        (0, 1, 71)
    );
}

const MUTE_AO_1: &CStr =
    c"{\"schema\":\"mradm.semantic-policy.v1\",\"objects\":[{\"id\":\"AO_1\",\"gain\":{\"mute\":true}}]}";
const HALF_MAIN: &CStr =
    c"{\"schema\":\"mradm.semantic-policy.v1\",\"objects\":[{\"programme\":\"Main\",\"gain\":{\"scale\":0.5}}]}";

/// Policies replace while running: an identity selector mutes, a replacement fades to its gain and
/// recomputes from the producer baseline, clearing restores it, an unmatched rule is diagnosed once.
#[test]
fn semantic_policies_replace_while_running() {
    let context = Context::new();
    let mut c = config();
    c.output_ring_frames = 4096;
    let stream = Stream::new(&context, &c);
    assert_eq!(stream.begin(41, 0), OK);
    let mut content: adm_scene_semantic_entity_t = sized();
    content.id = c"ACO_1".as_ptr();
    content.name = c"Dialogue".as_ptr();
    let mut programme: adm_scene_semantic_entity_t = sized();
    programme.id = c"APR_1".as_ptr();
    programme.name = c"Main".as_ptr();
    let mut identity: adm_scene_semantic_identity_t = sized();
    identity.object_id = c"AO_1".as_ptr();
    identity.object_name = c"Voice".as_ptr();
    identity.track_uid = c"ATU_1".as_ptr();
    identity.has_importance = 1;
    identity.importance = 7;
    identity.has_dialogue_id = 1;
    identity.dialogue_id = 1;
    identity.content_count = 1;
    identity.contents = &content;
    identity.programme_count = 1;
    identity.programmes = &programme;
    let mut d = descriptor(ELEMENT_OBJECT, 7, None);
    d.has_position = 1;
    d.position_y = 1.0;
    d.semantic_identity = &identity;
    assert_eq!(stream.configure(41, 1, &[d]), OK);
    stream.apply_policy(Some(MUTE_AO_1), 1);
    assert_eq!(stream.policy(Some(c"{not-json"), 99), INVALID_ARGUMENT);
    assert_eq!(stream.status().semantic_policy_revision, 1);

    stream.accept(Frame::object(41, 1, 0, vec![0.1; 1024]).get());
    assert!(stream.take(SPEAKERS, 1024).iter().all(|&s| s == 0.0));
    stream.apply_policy(Some(HALF_MAIN), 2);
    stream.accept(
        Frame::object(41, 1, 1024, vec![0.1; 2048])
            .without_initial()
            .get(),
    );
    let half = mean_tail(&stream.take(SPEAKERS, 2048), SPEAKERS as usize, None, 256);
    assert!(half > 0.0);
    stream.apply_policy(Some(HALF_MAIN), 3);
    stream.accept(
        Frame::object(41, 1, 3072, vec![0.1; 1024])
            .without_initial()
            .get(),
    );
    let repeated = mean_tail(&stream.take(SPEAKERS, 1024), SPEAKERS as usize, None, 256);
    assert!(
        (repeated - half).abs() < half * 0.05,
        "{repeated} vs {half}"
    );
    stream.apply_policy(None, 4);
    stream.accept(
        Frame::object(41, 1, 4096, vec![0.1; 2048])
            .without_initial()
            .get(),
    );
    stream.end(41, 6144);
    let (restored, signal) = stream.drain(SPEAKERS, 2048);
    let restored = mean_tail(&restored, SPEAKERS as usize, None, 256);
    assert!(
        signal && restored > half * 1.8 && restored < half * 2.2,
        "{restored} vs {half}"
    );
    stream.apply_policy(
        Some(c"{\"schema\":\"mradm.semantic-policy.v1\",\"objects\":[{\"id\":\"missing\",\"gain\":{\"mute\":true}}]}"),
        5,
    );
    assert_eq!(stream.count_messages("objects[0] did not match"), 1);
}

/// One track-UID match applies to every element of its object; global / all rules apply without an
/// identity; a DirectSpeakers operation filters by label.
#[test]
fn semantic_identities_group_tracks_and_roles() {
    let context = Context::new();
    let stream = Stream::new(&context, &config());
    assert_eq!(stream.begin(50, 0), OK);
    let identities: Vec<adm_scene_semantic_identity_t> = [c"ATU_A", c"ATU_B"]
        .iter()
        .map(|uid| {
            let mut identity: adm_scene_semantic_identity_t = sized();
            identity.object_id = c"AO_GROUP".as_ptr();
            identity.object_name = c"Grouped object".as_ptr();
            identity.track_uid = uid.as_ptr();
            identity
        })
        .collect();
    let descriptors: Vec<_> = identities
        .iter()
        .enumerate()
        .map(|(i, identity)| {
            let mut d = descriptor(ELEMENT_OBJECT, i as u64 + 1, None);
            d.has_position = 1;
            d.position_x = if i == 0 { -0.5 } else { 0.5 };
            d.position_y = 1.0;
            d.semantic_identity = identity;
            d
        })
        .collect();
    assert_eq!(stream.configure(50, 1, &descriptors), OK);
    stream.apply_policy(
        Some(c"{\"schema\":\"mradm.semantic-policy.v1\",\"objects\":[{\"track_uid\":\"ATU_B\",\"gain\":{\"mute\":true}}]}"),
        10,
    );
    let initial: Vec<_> = (1..=2u64)
        .map(|id| {
            let mut s: adm_scene_initial_state_t = sized();
            s.element_id = id;
            s.state = complete_state(1.0);
            s
        })
        .collect();
    let mut frame = Frame::elements(
        50,
        1,
        0,
        vec![(1, vec![0.05; 128]), (2, vec![0.05; 128])],
        initial,
    );
    stream.accept(frame.get());
    stream.end(50, 128);
    let (output, signal) = stream.drain(SPEAKERS, 128);
    assert!(!signal && output.iter().all(|&s| s == 0.0));

    assert_eq!(stream.begin(51, 0), OK);
    stream.configure_object(51, 1, 0.0);
    stream.apply_policy(
        Some(c"{\"schema\":\"mradm.semantic-policy.v1\",\"global\":{\"gain\":{\"scale\":1}},\"objects\":[{\"all\":true,\"gain\":{\"mute\":true}},{\"id\":\"missing\",\"gain\":{\"mute\":true}}]}"),
        11,
    );
    stream.accept(Frame::object(51, 1, 0, vec![0.05; 128]).get());
    stream.end(51, 128);
    assert!(!stream.drain(SPEAKERS, 128).1);
    assert_eq!(stream.count_messages("objects[1] did not match"), 1);

    assert_eq!(stream.begin(52, 0), OK);
    let mut direct_identity: adm_scene_semantic_identity_t = sized();
    direct_identity.object_id = c"AO_DS".as_ptr();
    let mut direct = descriptor(ELEMENT_DIRECT_SPEAKER, 9, Some(c"M+030"));
    direct.semantic_identity = &direct_identity;
    assert_eq!(stream.configure(52, 1, &[direct]), OK);
    stream.apply_policy(
        Some(c"{\"schema\":\"mradm.semantic-policy.v1\",\"objects\":[{\"id\":\"AO_DS\",\"direct_speakers\":{\"speaker_label\":\"M+030\",\"gain\":{\"mute\":true},\"head_locked\":true}}]}"),
        12,
    );
    let mut s: adm_scene_initial_state_t = sized();
    s.element_id = 9;
    s.state = complete_state(1.0);
    let mut frame = Frame::elements(52, 1, 0, vec![(9, vec![0.05; 128])], vec![s]);
    stream.accept(frame.get());
    stream.end(52, 128);
    assert!(!stream.drain(SPEAKERS, 128).1);
}

const DS_POSITION: &CStr = c"{\"schema\":\"mradm.semantic-policy.v1\",\"objects\":[{\"id\":\"AO_DS_POSITION\",\"direct_speakers\":{\"speaker_label\":\"M+030\",\"position\":{\"azimuth\":-30}}}]}";

/// A labelled DirectSpeakers element routes by its label (not its fallback position) under a
/// neutral policy; a position override moves it and clearing the override restores the label.
#[test]
fn direct_speaker_position_overrides_clear_back_to_the_label() {
    let context = Context::new();
    let mut c = config();
    c.output_ring_frames = 4096;
    let stream = Stream::new(&context, &c);
    assert_eq!(stream.begin(53, 0), OK);
    let mut identity: adm_scene_semantic_identity_t = sized();
    identity.object_id = c"AO_DS_POSITION".as_ptr();
    let mut d = descriptor(ELEMENT_DIRECT_SPEAKER, 10, Some(c"M+030"));
    d.has_position = 1;
    d.position_x = 0.5;
    d.position_y = 0.866_025_4;
    d.semantic_identity = &identity;
    assert_eq!(stream.configure(53, 1, &[d]), OK);
    stream.apply_policy(
        Some(c"{\"schema\":\"mradm.semantic-policy.v1\",\"global\":{\"gain\":{\"scale\":1}}}"),
        13,
    );
    let mut s: adm_scene_initial_state_t = sized();
    s.element_id = 10;
    s.state = complete_state(1.0);
    s.state.valid_fields &= !STATE_POSITION;
    let mut frame = Frame::elements(53, 1, 0, vec![(10, vec![0.05; 2048])], vec![s]);
    stream.accept(frame.get());
    let labelled = stream.take(SPEAKERS, 2048);
    stream.apply_policy(Some(DS_POSITION), 14);
    frame.frame.media_sample_start = 2048;
    frame.initial.clear();
    stream.accept(frame.get());
    let positioned = stream.take(SPEAKERS, 2048);
    stream.apply_policy(None, 15);
    frame.frame.media_sample_start = 4096;
    stream.accept(frame.get());
    let restored = stream.take(SPEAKERS, 2048);
    let side = |out: &[f32]| {
        let (a, b) = (
            mean_tail(out, SPEAKERS as usize, Some(0), 256),
            mean_tail(out, SPEAKERS as usize, Some(1), 256),
        );
        assert!(a.max(b) > a.min(b) * 10.0, "{a} {b}");
        a > b
    };
    let (l, p, r) = (side(&labelled), side(&positioned), side(&restored));
    assert!(l == r && l != p);
    stream.end(53, 6144);
    stream.drain(SPEAKERS, 0);
}

#[derive(PartialEq)]
enum DsRun {
    Transitions,
    LabelReference,
    PositionReference,
}

fn binaural_direct_speaker_run(context: &Context, epoch: u64, run: DsRun) -> (Vec<f32>, Vec<f32>) {
    let stream = Stream::new(context, &binaural_config(4096));
    assert_eq!(stream.begin(epoch, 0), OK);
    let mut identity: adm_scene_semantic_identity_t = sized();
    identity.object_id = c"AO_DS_BINAURAL_RAMP".as_ptr();
    let mut d = descriptor(ELEMENT_DIRECT_SPEAKER, 11, Some(c"M+030"));
    d.semantic_identity = &identity;
    assert_eq!(stream.configure(epoch, 1, &[d]), OK);
    let policy = c"{\"schema\":\"mradm.semantic-policy.v1\",\"objects\":[{\"id\":\"AO_DS_BINAURAL_RAMP\",\"direct_speakers\":{\"speaker_label\":\"M+030\",\"position\":{\"azimuth\":-30}}}]}";
    if run == DsRun::PositionReference {
        stream.apply_policy(Some(policy), 1);
    }
    // Head tracking splits the binaural DirectSpeakers control into 512-frame slices.
    stream.orient(0.0, 0.0, 0.0);
    let samples: Vec<f32> = (0..1024)
        .map(|i| (i as f32 * 0.113).sin() * 0.05 + (i as f32 * 0.173).cos() * 0.025)
        .collect();
    let mut start = 0i64;
    let mut block = |initial: bool| {
        let mut s: adm_scene_initial_state_t = sized();
        s.element_id = 11;
        s.state = complete_state(1.0);
        s.state.valid_fields &= !STATE_POSITION;
        let states = if initial { vec![s] } else { Vec::new() };
        let mut frame = Frame::elements(epoch, 1, start, vec![(11, samples.clone())], states);
        stream.accept(frame.get());
        start += 1024;
        stream.take(2, 1024)
    };
    block(true);
    block(false);
    let captures = if run == DsRun::Transitions {
        stream.apply_policy(Some(policy), 1);
        stream.orient(0.0, 0.0, 0.0);
        let first = block(false);
        block(false);
        block(false);
        stream.apply_policy(None, 2);
        stream.orient(0.0, 0.0, 0.0);
        (first, block(false))
    } else {
        (block(false), Vec::new())
    };
    stream.end(epoch, start);
    stream.drain(2, 0);
    captures
}

/// A binaural DirectSpeakers position override, and its clearing, start within the first
/// 512-frame control slice of the policy ramp.
#[test]
fn binaural_direct_speaker_policy_ramps_start_in_the_first_slice() {
    let context = Context::new();
    let (position, clear) = binaural_direct_speaker_run(&context, 54, DsRun::Transitions);
    let (label, _) = binaural_direct_speaker_run(&context, 55, DsRun::LabelReference);
    let (positioned, _) = binaural_direct_speaker_run(&context, 56, DsRun::PositionReference);
    let slice = 512 * 2;
    assert!(max_difference(&position[..slice], &label[..slice]) > 1.0e-6);
    assert!(max_difference(&clear[..slice], &positioned[..slice]) > 1.0e-6);
}

#[derive(Clone, Copy, Default)]
struct ObjectCase {
    azimuth: f32,
    elevation: f32,
    width: f32,
    height: f32,
    channel_lock: bool,
    max_distance: Option<f32>,
    pose: Option<(f32, f32, f32)>,
}

fn binaural_object(context: &Context, epoch: u64, case: ObjectCase) -> Vec<f32> {
    let stream = Stream::new(context, &binaural_config(4096));
    assert_eq!(stream.begin(epoch, 0), OK);
    stream.configure_object(epoch, 1, 0.0);
    if let Some((yaw, pitch, roll)) = case.pose {
        stream.orient(yaw, pitch, roll);
    }
    let samples: Vec<f32> = (0..2048)
        .map(|i| (i as f32 * 0.071).sin() * 0.05 + (i as f32 * 0.131).cos() * 0.025)
        .collect();
    let mut frame = Frame::object(epoch, 1, 0, samples);
    let (az, el) = (case.azimuth.to_radians(), case.elevation.to_radians());
    let state = frame.state();
    state.position_x = -az.sin() * el.cos();
    state.position_y = az.cos() * el.cos();
    state.position_z = el.sin();
    state.extent_width = case.width;
    state.extent_height = case.height;
    state.channel_lock = i32::from(case.channel_lock);
    state.has_channel_lock_max_distance = i32::from(case.max_distance.is_some());
    state.channel_lock_max_distance = case.max_distance.unwrap_or(0.0);
    stream.accept(frame.get());
    stream.end(epoch, 2048);
    let (output, signal) = stream.drain(2, 2048);
    assert!(signal);
    output
}

/// channelLock's maxDistance uses the full 3D distance, and an unbounded lock snaps azimuth and
/// elevation to the nearest stereo reference.
#[test]
fn binaural_channel_lock_uses_3d_geometry() {
    let context = Context::new();
    let elevated = ObjectCase {
        azimuth: 20.0,
        elevation: 60.0,
        ..Default::default()
    };
    let thresholded = ObjectCase {
        channel_lock: true,
        max_distance: Some(0.2),
        ..elevated
    };
    let unbounded = ObjectCase {
        channel_lock: true,
        ..elevated
    };
    let reference = ObjectCase {
        azimuth: 30.0,
        ..Default::default()
    };
    let elevated = binaural_object(&context, 60, elevated);
    equivalent(
        &elevated,
        &binaural_object(&context, 61, thresholded),
        BINAURAL_EQUIVALENCE,
        "maxDistance in 3D",
    );
    let reference = binaural_object(&context, 63, reference);
    equivalent(
        &binaural_object(&context, 62, unbounded),
        &reference,
        BINAURAL_EQUIVALENCE,
        "the lock snaps azimuth and elevation",
    );
    assert!(max_difference(&elevated, &reference) > 1.0e-6);
}

/// Listener roll rotates every world-space extent direction into head space.
#[test]
fn listener_pose_rotates_the_spread_cloud() {
    let context = Context::new();
    let horizontal = ObjectCase {
        width: 0.5,
        ..Default::default()
    };
    let rolled = ObjectCase {
        pose: Some((0.0, 0.0, 90.0)),
        ..horizontal
    };
    let vertical = ObjectCase {
        height: 2.0 / 3.0,
        ..Default::default()
    };
    let horizontal = binaural_object(&context, 64, horizontal);
    let vertical = binaural_object(&context, 66, vertical);
    equivalent(
        &binaural_object(&context, 65, rolled),
        &vertical,
        1.0e-5,
        "roll rotates the extent",
    );
    assert!(max_difference(&horizontal, &vertical) > 1.0e-6);
}

fn binaural_pose(
    context: &Context,
    epoch: u64,
    pose: Option<(f32, f32, f32)>,
    head_locked: bool,
) -> Vec<f32> {
    let stream = Stream::new(context, &binaural_config(4096));
    assert_eq!(stream.begin(epoch, 0), OK);
    stream.configure_object(epoch, 1, 0.0);
    if let Some((yaw, pitch, roll)) = pose {
        stream.orient(yaw, pitch, roll);
    }
    let mut frame = Frame::object(epoch, 1, 0, vec![0.1; 512]);
    frame.state().head_locked = i32::from(head_locked);
    stream.accept(frame.get());
    stream.end(epoch, 512);
    let (output, signal) = stream.drain(2, 512);
    assert!(signal);
    output
}

/// The listener pose rotates world-space elements (not head-locked ones), the orientation entry
/// validates its arguments, and active head tracking caps the worker lookahead at 2048 frames.
#[test]
fn listener_orientation_rotates_world_space_elements() {
    let context = Context::new();
    let baseline = binaural_pose(&context, 42, None, false);
    equivalent(
        &baseline,
        &binaural_pose(&context, 43, Some((0.0, 0.0, 0.0)), false),
        BINAURAL_EQUIVALENCE,
        "an explicit zero pose",
    );
    let left = binaural_pose(&context, 44, Some((90.0, 0.0, 0.0)), false);
    let right = binaural_pose(&context, 45, Some((-90.0, 0.0, 0.0)), false);
    assert!(max_difference(&left, &right) > 1.0e-6);
    assert!(
        max_difference(
            &baseline,
            &binaural_pose(&context, 46, Some((15.0, 25.0, -20.0)), false)
        ) > 1.0e-6
    );
    equivalent(
        &binaural_pose(&context, 47, None, true),
        &binaural_pose(&context, 48, Some((80.0, -30.0, 25.0)), true),
        BINAURAL_EQUIVALENCE,
        "head-locked elements ignore the pose",
    );
    unsafe {
        assert_eq!(
            adm_scene_stream_set_listener_orientation(ptr::null_mut(), 0.0, 0.0, 0.0),
            INVALID_ARGUMENT
        );
        assert_eq!(
            adm_scene_stream_set_listener_orientation(ptr::null_mut(), f32::NAN, 0.0, 0.0),
            INVALID_ARGUMENT
        );
    }

    let mut c = binaural_config(8192);
    c.startup_watermark_frames = 4096;
    let stream = Stream::new(&context, &c);
    assert_eq!(stream.begin(49, 0), OK);
    stream.configure_object(49, 1, 0.0);
    stream.orient(5.0, 0.0, 0.0);
    stream.accept(Frame::object(49, 1, 0, vec![0.05; 4096]).get());
    let status = stream.wait(|s| s.buffered_output_frames >= 2048, "the shallow lead");
    assert!(status.buffered_output_frames <= 2048 && status.queued_input_samples == 4096);
    stream.end(49, 4096);
    assert!(stream.drain(2, 4096).1);
}

/// The C++ `render_reference_frame_ramp`: a gain and position ramp, with or without a head-lock
/// update at `offset` (or, `independent`, position and gain ramps of different lengths).
#[allow(clippy::too_many_arguments)]
fn reference_frame_ramp(
    context: &Context,
    epoch: u64,
    binaural_output: bool,
    tracking: bool,
    initially_locked: bool,
    yaw: f32,
    offset: u32,
    independent: bool,
) -> Vec<f32> {
    let config = if binaural_output {
        binaural_config(2048)
    } else {
        config()
    };
    let stream = Stream::new(context, &config);
    assert_eq!(stream.begin(epoch, 0), OK);
    stream.configure_object(epoch, 1, 0.0);
    stream.orient(yaw, 0.0, 0.0);
    let mut frame = Frame::object(epoch, 1, 0, vec![0.02; 1024]);
    {
        let state = frame.state();
        state.linear_gain = 0.1;
        state.position_x = 0.0;
        state.position_y = 1.0;
        state.head_locked = i32::from(initially_locked);
    }
    let initial = frame.initial[0].state;
    let mut target = initial;
    target.linear_gain = 0.8;
    target.position_x = -0.5;
    let mut first = update(0, 512, STATE_LINEAR_GAIN | STATE_POSITION, target);
    if tracking && offset == 0 {
        first.changed_fields |= STATE_HEAD_LOCKED;
        first.state.head_locked = 1;
    }
    let mut updates = vec![first];
    if independent {
        updates[0].changed_fields = STATE_POSITION;
        updates[0].ramp_duration_samples = 256;
        let mut gain = updates[0];
        gain.changed_fields = STATE_LINEAR_GAIN;
        gain.ramp_duration_samples = 512;
        let mut deadline = updates[0];
        deadline.offset_samples = 256;
        deadline.ramp_duration_samples = 0;
        deadline.changed_fields = if tracking {
            STATE_HEAD_LOCKED
        } else {
            STATE_POSITION
        };
        deadline.state.head_locked = i32::from(tracking);
        let mut resumed = gain;
        resumed.offset_samples = 256;
        resumed.ramp_duration_samples = 256;
        updates.push(gain);
        updates.push(deadline);
        if !tracking {
            updates.push(resumed);
        }
    } else if offset != 0 {
        let mut second = updates[0];
        second.offset_samples = offset;
        second.ramp_duration_samples = if tracking { 0 } else { 512 - offset };
        if tracking {
            second.changed_fields = STATE_HEAD_LOCKED;
        }
        second.state.head_locked = if tracking { 1 } else { initial.head_locked };
        updates.push(second);
    }
    frame.updates = updates;
    stream.accept(frame.get());
    stream.end(epoch, 1024);
    let (output, signal) = stream.drain(if binaural_output { 2 } else { SPEAKERS }, 1024);
    assert!(signal);
    output
}

/// A head-only update leaves ongoing gain and position ramps intact, a position ramp ends at its own
/// deadline while a longer gain ramp continues, and a head lock in a mixed update applies at onset.
#[test]
fn reference_frame_updates_preserve_ramps() {
    let context = Context::new();
    for binaural_output in [false, true] {
        equivalent(
            &reference_frame_ramp(
                &context,
                101,
                binaural_output,
                false,
                false,
                0.0,
                128,
                false,
            ),
            &reference_frame_ramp(&context, 102, binaural_output, true, false, 0.0, 128, false),
            1.0e-5,
            "a head-only update",
        );
        equivalent(
            &reference_frame_ramp(&context, 106, binaural_output, false, false, 0.0, 256, true),
            &reference_frame_ramp(&context, 107, binaural_output, true, false, 0.0, 256, true),
            1.0e-5,
            "independent deadlines",
        );
    }
    let locked = reference_frame_ramp(&context, 103, true, false, true, 80.0, 0, false);
    let switched = reference_frame_ramp(&context, 104, true, true, false, 80.0, 0, false);
    let world = reference_frame_ramp(&context, 105, true, false, false, 80.0, 0, false);
    equivalent(&locked, &switched, 1.0e-5, "a head lock at onset");
    assert!(max_difference(&locked, &world) > 1.0e-6);
}

fn triple_config(layout: &'static CStr) -> adm_scene_stream_config_t {
    let mut c = config();
    c.rendering = renderer(layout);
    c.rendering.renderer = TRIPLE_BALANCE;
    c.output_ring_frames = 8192;
    c
}

/// Triple Balance renders size ramps after preroll and resampling on every room layout, with
/// policies replaced and cleared per epoch; unsupported configurations fail at creation.
#[test]
fn triple_balance_scene_streams_render_size_ramps() {
    let context = Context::new();
    for (layout, channels) in [
        (c"7.1.4", 12),
        (c"4+7+0", 12),
        (c"9.1.6", 16),
        (c"22.2", 24),
        (c"9+10+3", 24),
    ] {
        let mut c = triple_config(layout);
        c.output_sample_rate = 44100;
        let stream = Stream::new(&context, &c);
        for epoch in [1u64, 2] {
            assert_eq!(stream.begin(epoch, 13), OK);
            stream.configure_object(epoch, 1, 0.0);
            let policy = (epoch == 1).then_some(
                c"{\"schema\":\"mradm.semantic-policy.v1\",\"global\":{\"gain\":{\"scale\":0.5}}}",
            );
            stream.apply_policy(policy, epoch);
            let mut frame = Frame::object(epoch, 1, 0, vec![0.1; 2051]);
            let state = frame.state();
            state.position_z = 0.5;
            state.extent_width = 0.3;
            state.extent_height = 0.3;
            state.extent_depth = 0.3;
            let mut target = *state;
            target.extent_width = 0.6;
            target.extent_height = 0.6;
            target.extent_depth = 0.6;
            frame.updates.push(update(73, 137, STATE_EXTENT, target));
            assert_eq!(stream.submit(frame.get(), 100), (OK, ACCEPTED));
            stream.end(epoch, 2051);
            assert!(stream.drain(channels, (2038u64 * 44100).div_ceil(48000)).1);
        }
    }
    for kind in 0..4 {
        let mut c = triple_config(c"7.1.4");
        match kind {
            0 => c.rendering.speaker_geometry = 1,
            1 => c.rendering.speaker_spread_mode = 2,
            2 => c.rendering.sofa_path = c"unused.sofa".as_ptr(),
            _ => {
                c.rendering.output_layout = c"22.2".as_ptr();
                c.input_sample_rate = 96000;
            }
        }
        let mut stream = ptr::null_mut();
        assert_eq!(
            unsafe { adm_create_scene_stream(context.0, &c, &mut stream) },
            UNSUPPORTED
        );
        assert!(stream.is_null());
    }
}

/// Triple Balance renders the effective position: a descriptor default below the room is fine
/// once the state or a policy moves it inside.
#[test]
fn triple_balance_validates_the_effective_position() {
    let context = Context::new();
    for (layout, channels) in [(c"7.1.4", 12), (c"9.1.6", 16)] {
        for use_policy in [false, true] {
            let stream = Stream::new(&context, &triple_config(layout));
            assert_eq!(stream.begin(1, 0), OK);
            stream.configure_object(1, 1, -0.5);
            if use_policy {
                stream.apply_policy(
                    Some(c"{\"schema\":\"mradm.semantic-policy.v1\",\"global\":{\"position\":{\"elevation\":30}}}"),
                    1,
                );
            }
            let mut frame = Frame::object(1, 1, 0, vec![0.1; 257]);
            frame.state().position_z = 0.5;
            if use_policy {
                frame = frame.without_initial();
            }
            assert_eq!(stream.submit(frame.get(), 100), (OK, ACCEPTED));
            let output = stream.take(channels, 257);
            assert!(output.iter().any(|s| s.abs() > 1.0e-7));
            stream.end(1, 257);
            stream.drain(channels, 0);
        }
    }
}

/// A switch to Triple Balance validates the effective position and crossfades back to VBAP; an
/// incompatible state makes the candidate fail with a diagnostic while the stream keeps running.
#[test]
fn triple_balance_switches_keep_the_stream() {
    let context = Context::new();
    for incompatible in [false, true] {
        let mut c = triple_config(c"4+7+0");
        c.rendering.renderer = SAF;
        let stream = Stream::new(&context, &c);
        assert_eq!(stream.begin(1, 0), OK);
        stream.configure_object(1, 1, -0.5);
        let mut frame = Frame::object(1, 1, 0, vec![0.1; 1024]);
        frame.state().head_locked = i32::from(incompatible);
        assert_eq!(stream.submit(frame.get(), 100).0, OK);
        stream.take(12, 1024);
        let before = unsafe { adm_scene_stream_log_count(stream.0) };
        assert_eq!(stream.switch(&triple_config(c"7.1.4").rendering), OK);
        let mut frame = Frame::object(1, 1, 1024, vec![0.1; 4096]).without_initial();
        assert_eq!(stream.submit(frame.get(), 100).0, OK);
        stream.take(12, 4096);
        assert_eq!(stream.status().failed, 0);
        let after = unsafe { adm_scene_stream_log_count(stream.0) };
        if incompatible {
            assert!(after > before);
        } else {
            assert_eq!(after, before);
            assert_eq!(stream.switch(&renderer(c"4+7+0")), OK);
            frame.frame.media_sample_start = 5120;
            assert_eq!(stream.submit(frame.get(), 100).0, OK);
            stream.take(12, 4096);
        }
        stream.end(1, if incompatible { 5120 } else { 9216 });
        stream.drain(12, 0);
    }
}

/// An unsupported state fails the Triple Balance worker explicitly, naming the field.
#[test]
fn triple_balance_names_an_unsupported_field() {
    let context = Context::new();
    let stream = Stream::new(&context, &triple_config(c"7.1.4"));
    assert_eq!(stream.begin(1, 0), OK);
    stream.configure_object(1, 1, 0.0);
    let mut frame = Frame::object(1, 1, 0, vec![0.1; 31]);
    frame.state().head_locked = 1;
    assert_eq!(stream.submit(frame.get(), 100), (OK, ACCEPTED));
    stream.wait(|s| s.failed != 0, "the worker failure");
    assert!(stream.count_messages("headLocked") > 0);
}
