//! The monitor worker (`MonitorEngine::worker_loop` and its helpers), in the C++ order.
use super::{
    CHUNK_FRAMES, Cancel, Control, IDLE_NAP, Overrides, Receipt, Shared, Source, State,
    TRACKING_CHUNK_FRAMES, TRACKING_LOOKAHEAD_FRAMES, Warmed, Warmup, code, error, level, meter,
    store_f32,
};
use mradm_dsp::meter::Meter;
use mradm_dsp::monitor::Crossfade;
use mradm_render::routing::Error;
use std::sync::atomic::{AtomicU64, Ordering};
use std::sync::{Arc, MutexGuard};
use std::time::Duration;

type Guard<'a> = MutexGuard<'a, Control>;

pub(super) struct Worker {
    shared: Arc<Shared>,
    stream: Box<dyn Source>,
    xfade: Option<Box<dyn Source>>,
    crossfade: Crossfade,
    scratch: Vec<f32>,
    scratch_b: Vec<f32>,
    meter_scratch: Vec<f32>,
    meter: Option<Meter>,
    lufs_counter: u64,
    integrated_throttle: u32,
    producer_pos: u64,
    current_overrides: Overrides,
    current_orientation: [f32; 3],
    /// The running warmup's target, which production advances (`switch_warmup_->target`); only
    /// the worker starts and collects warmups.
    warmup_target: Option<Arc<AtomicU64>>,
}

fn abort_on_contract() -> ! {
    // IRenderStream must not return more frames than requested (C++ std::terminate).
    std::process::abort()
}

fn message(e: &Error) -> &[u8] {
    &e.message
}

impl Worker {
    pub(super) fn new(
        shared: Arc<Shared>,
        stream: Box<dyn Source>,
        crossfade: Crossfade,
        meter_scratch: Vec<f32>,
    ) -> Self {
        let chunk = CHUNK_FRAMES as usize;
        Self {
            scratch: vec![0.; chunk * shared.ring_channels],
            scratch_b: vec![0.; chunk * shared.channels],
            shared,
            stream,
            xfade: None,
            crossfade,
            meter_scratch,
            meter: None,
            lufs_counter: 0,
            integrated_throttle: 0,
            producer_pos: 0,
            current_overrides: Overrides::default(),
            current_orientation: [0.; 3],
            warmup_target: None,
        }
    }

    /// The output format is fixed: a seek resets integration in place.
    pub(super) fn rebuild_meter(&mut self) {
        let s = &self.shared;
        s.silence_lufs();
        self.lufs_counter = 0;
        self.integrated_throttle = 0;
        if let Some(m) = &mut self.meter {
            m.reset();
            return;
        }
        match meter(s.channels, s.sample_rate) {
            Ok(m) => self.meter = Some(m),
            Err(e) => s.log(level::WARNING, "meter", e.to_string().as_bytes()),
        }
    }

    fn feed_meter(&mut self, frames: usize, from_meter_scratch: bool) {
        let Some(m) = &mut self.meter else {
            return;
        };
        let s = &self.shared;
        let samples = if from_meter_scratch {
            &self.meter_scratch[..frames * s.channels]
        } else {
            &self.scratch[..frames * s.channels]
        };
        if let Err(e) = m.add_frames(samples) {
            s.log(level::WARNING, "meter", e.to_string().as_bytes());
            self.meter = None;
            s.silence_lufs();
            return;
        }
        self.lufs_counter += frames as u64;
        if self.lufs_counter < u64::from(s.sample_rate / 10) {
            return;
        }
        self.lufs_counter = 0;
        let lufs = |v: f64| {
            if v.is_finite() {
                v as f32
            } else {
                f32::NEG_INFINITY
            }
        };
        if let Ok(v) = m.momentary() {
            store_f32(&s.momentary, lufs(v));
        }
        if let Ok(v) = m.shortterm() {
            store_f32(&s.shortterm, lufs(v));
        }
        self.integrated_throttle += 1;
        if self.integrated_throttle >= 10 {
            self.integrated_throttle = 0;
            if let Ok(v) = m.integrated() {
                store_f32(&s.integrated, lufs(v));
            }
        }
    }

    fn set_room_control(&self) {
        *self
            .shared
            .room_control
            .lock()
            .unwrap_or_else(|e| e.into_inner()) = self.stream.room_control();
    }

    /// Snaps to the incoming stream; under the control lock.
    fn finalize_crossfade(&mut self, c: &mut Control) {
        if let Some(next) = self.xfade.take() {
            self.stream = next;
            c.current_validator = c
                .xfade_validator
                .take()
                .unwrap_or_else(|| self.stream.validator());
            self.set_room_control();
            self.crossfade.reset();
        }
    }

    fn switch_target(&self) -> u64 {
        if self.shared.output_stage {
            self.shared.frames_played.load(Ordering::SeqCst)
        } else {
            self.producer_pos
        }
    }

    /// Only the incoming stream is touched by the background job.
    fn launch_warmup(
        &mut self,
        c: &mut Control,
        mut stream: Box<dyn Source>,
        position: u64,
        positioned: bool,
        receipt: Option<Arc<Receipt>>,
    ) {
        let cancel = Cancel::new();
        let target = Arc::new(AtomicU64::new(self.switch_target()));
        let validator = stream.validator();
        let overrides = self.current_overrides.clone();
        let orientation = self.current_orientation;
        let (job_cancel, job_target) = (cancel.clone(), target.clone());
        let spawned = std::thread::Builder::new().spawn(move || {
            let mut position = position;
            let result = (|| {
                stream.set_overrides(&overrides);
                stream.set_orientation(orientation);
                let channels = stream.out_channels();
                let mut scratch = vec![0f32; CHUNK_FRAMES as usize * channels];
                if !positioned {
                    let t = job_target.load(Ordering::SeqCst);
                    stream.seek_with_cancel(t, &job_cancel)?;
                    position = t;
                }
                while !job_cancel.requested() {
                    let t = job_target.load(Ordering::SeqCst);
                    if position == t {
                        return Ok(());
                    }
                    if position > t {
                        stream.seek_with_cancel(t, &job_cancel)?;
                        position = t;
                        continue;
                    }
                    let frames = CHUNK_FRAMES.min(t - position) as usize;
                    let got = stream.process(&mut scratch, frames)?;
                    if got == 0 {
                        return Err(error(
                            code::RENDER_FAILED,
                            "incoming monitor stream ended during warmup",
                        ));
                    }
                    position += got as u64;
                }
                Err(error(code::CANCELLED, "monitor backend warmup cancelled"))
            })();
            Warmed {
                stream,
                position,
                result,
            }
        });
        // The C++ std::async throws when no thread can start; that surfaces as a worker failure.
        let handle = spawned.unwrap_or_else(|_| std::process::abort());
        self.warmup_target = Some(target.clone());
        c.warmup = Some(Warmup {
            cancel,
            target,
            receipt,
            validator,
            handle: Some(handle),
        });
    }

    fn poll_switch(&mut self, c: &mut Control) {
        let target = self.switch_target();
        if let Some(w) = &mut c.warmup {
            w.target.store(target, Ordering::SeqCst);
            if !w.handle.as_ref().is_some_and(|h| h.is_finished()) {
                return;
            }
            let mut job = c.warmup.take().expect("warmup");
            self.warmup_target = None;
            let warmed = job
                .handle
                .take()
                .expect("handle")
                .join()
                .unwrap_or_else(|_| std::process::abort());
            if !c.switch_pending {
                if job.cancel.requested() {
                    c.pending_stream = Some(warmed.stream);
                    c.pending_receipt = job.receipt.take();
                    c.switch_pending = true;
                } else if let Err(e) = &warmed.result {
                    Receipt::finish(&job.receipt, Receipt::REJECTED);
                    self.shared.log(level::ERROR, "monitor", message(e));
                } else if warmed.position != target {
                    self.launch_warmup(c, warmed.stream, warmed.position, true, job.receipt.take());
                } else {
                    c.pending_stream = Some(warmed.stream);
                    c.pending_receipt = job.receipt.take();
                    c.pending_stream_ready = true;
                    c.switch_pending = true;
                }
            } else {
                Receipt::finish(&job.receipt, Receipt::REJECTED);
            }
        }
        if c.switch_pending && !c.pending_stream_ready && c.warmup.is_none() {
            let pending = c.pending_stream.as_mut().expect("pending stream");
            if !pending.seek_requires_preroll() {
                pending.set_overrides(&self.current_overrides);
                if let Err(e) = pending.seek(target) {
                    Receipt::finish(&c.pending_receipt, Receipt::REJECTED);
                    c.pending_receipt = None;
                    self.shared.log(level::ERROR, "monitor", message(&e));
                    c.pending_stream = None;
                    c.switch_pending = false;
                    return;
                }
                c.pending_stream_ready = true;
                return;
            }
            let stream = c.pending_stream.take().expect("pending stream");
            let receipt = c.pending_receipt.take();
            self.launch_warmup(c, stream, 0, false, receipt);
            c.switch_pending = false;
        }
    }

    fn apply_ready_switch(&mut self, c: &mut Control) {
        self.finalize_crossfade(c);
        let s = self.shared.clone();
        let switch_to = if s.output_stage {
            s.frames_played.load(Ordering::Relaxed)
        } else {
            self.producer_pos
        };
        let mut pending = c.pending_stream.take().expect("pending stream");
        pending.set_overrides(&self.current_overrides);
        match pending.validator().validate(&self.current_overrides) {
            Ok(()) => {
                pending.set_orientation(self.current_orientation);
                if s.output_stage {
                    // Hard cut: park the callback, swap the stream, refill from the playhead.
                    s.park();
                    c.current_validator = pending.validator();
                    s.stage().renderer = pending.output_stage();
                    s.consumer_src_pos.store(switch_to, Ordering::Relaxed);
                    self.stream = pending;
                    self.set_room_control();
                    self.producer_pos = switch_to;
                    s.ring.clear();
                    s.meter_ring.clear();
                    s.ended.store(false, Ordering::Relaxed);
                    s.failed.store(false, Ordering::Relaxed);
                    // The 10 ms bridge covers the splice; the HpTF cascade restarts.
                    s.seek_generation.fetch_add(1, Ordering::Release);
                    s.hptf.reset_state();
                    s.unpark();
                } else {
                    c.xfade_validator = Some(pending.validator());
                    self.xfade = Some(pending);
                    self.crossfade.reset();
                    s.ended.store(false, Ordering::Relaxed);
                    s.failed.store(false, Ordering::Relaxed);
                }
                Receipt::finish(&c.pending_receipt, Receipt::APPLIED);
            }
            Err(e) => {
                Receipt::finish(&c.pending_receipt, Receipt::REJECTED);
                s.log(level::ERROR, "monitor", message(&e));
            }
        }
        c.pending_receipt = None;
        c.switch_pending = false;
        c.pending_stream_ready = false;
    }

    /// The seek under the lock: parks the callback, seeks without the lock, publishes the new
    /// position. False while a newer request supersedes it (the callback stays parked).
    fn apply_seek<'a>(&mut self, s: &'a Shared, frame: u64, mut c: Guard<'a>) -> (Guard<'a>, bool) {
        self.finalize_crossfade(&mut c);
        s.park();
        s.ring.clear();
        s.meter_ring.clear();
        let cancel = Cancel::new();
        c.seek_cancel = cancel.clone();
        c.seek_in_progress = true;
        drop(c);
        let result = self.stream.seek_with_cancel(frame, &cancel);
        let mut c = s.lock();
        c.seek_in_progress = false;
        if cancel.requested() || c.seek_pending || s.quit.load(Ordering::Acquire) {
            return (c, false);
        }
        match result {
            Ok(()) => {
                self.producer_pos = frame;
                s.consumer_src_pos.store(frame, Ordering::Relaxed);
                s.frames_played.store(frame, Ordering::Relaxed);
                s.ended.store(false, Ordering::Relaxed);
                s.failed.store(false, Ordering::Relaxed);
                s.seek_generation.fetch_add(1, Ordering::Release);
            }
            Err(e) => {
                s.log(level::ERROR, "monitor", message(&e));
                s.failed.store(true, Ordering::Release);
            }
        }
        // A running device drops stale output now; a paused one keeps its queue until play.
        if s.state() == State::Playing {
            s.host.device_flush();
            c.device_flush_pending = false;
        } else {
            c.device_flush_pending = true;
        }
        self.rebuild_meter();
        s.hptf.reset_state();
        s.unpark();
        (c, true)
    }

    fn apply_pending_seek<'a>(&mut self, s: &'a Shared, mut c: Guard<'a>) -> Guard<'a> {
        if !c.seek_pending {
            return c;
        }
        let target = c.seek_target;
        c.seek_pending = false;
        let (mut c, applied) = self.apply_seek(s, target, c);
        if !applied || !c.play_pending_after_seek {
            return c;
        }
        if c.device_flush_pending {
            s.host.device_flush();
            c.device_flush_pending = false;
        }
        s.set_state(State::Playing);
        c.play_pending_after_seek = false;
        // Resume before releasing the lock so a following pause cannot be overtaken.
        s.host.device_resume();
        c
    }

    pub(super) fn run(mut self) {
        let s = self.shared.clone();
        let mut room_worker_active = false;
        while !s.quit.load(Ordering::Acquire) {
            {
                let mut c = s.lock();
                if c.overrides_pending {
                    self.stream.set_overrides(&c.pending_overrides);
                    if let Some(x) = &mut self.xfade {
                        x.set_overrides(&c.pending_overrides);
                    }
                    self.current_overrides = c.pending_overrides.clone();
                    s.applied_override_revision
                        .store(c.pending_overrides.revision, Ordering::Relaxed);
                    c.overrides_pending = false;
                }
                c = self.apply_pending_seek(&s, c);
                if c.seek_pending || s.quit.load(Ordering::Acquire) {
                    continue;
                }
                if c.orientation_pending {
                    self.stream.set_orientation(c.pending_orientation);
                    if let Some(x) = &mut self.xfade {
                        x.set_orientation(c.pending_orientation);
                    }
                    self.current_orientation = c.pending_orientation;
                    c.orientation_pending = false;
                }
                self.poll_switch(&mut c);
                if c.switch_pending && c.pending_stream_ready {
                    self.apply_ready_switch(&mut c);
                }
                let done = s.ended.load(Ordering::Relaxed) || s.failed.load(Ordering::Relaxed);
                if s.state() != State::Playing || done {
                    let timeout = if c.warmup.is_some() {
                        IDLE_NAP
                    } else {
                        Duration::from_millis(1000)
                    };
                    let _ = s
                        .wake
                        .wait_timeout_while(c, timeout, |c| {
                            let done =
                                s.ended.load(Ordering::Relaxed) || s.failed.load(Ordering::Relaxed);
                            !(s.quit.load(Ordering::Acquire)
                                || c.seek_pending
                                || c.overrides_pending
                                || c.orientation_pending
                                || (c.switch_pending && c.warmup.is_none())
                                || (s.state() == State::Playing && !done))
                        })
                        .unwrap_or_else(|e| e.into_inner());
                    continue;
                }
            }
            let room_active = self.room_active();
            if room_active || room_worker_active {
                s.host.room_worker(room_active);
                room_worker_active = room_active;
            }
            if s.output_stage {
                self.drain_meter_ring();
                // The worker owns the current stream; ask it, not the callback's renderer slot.
                let failed = self
                    .stream
                    .output_stage()
                    .is_some_and(|stage| stage.render_failed());
                if failed && !s.failed.load(Ordering::Relaxed) {
                    s.log(
                        level::ERROR,
                        "monitor",
                        "apple 输出级渲染失败(AudioUnit 错误),已静音并停止".as_bytes(),
                    );
                    s.failed.store(true, Ordering::Release);
                }
            }
            if !self.top_up_ring() {
                if self.room_active() {
                    s.host.room_wait(IDLE_NAP);
                } else {
                    std::thread::sleep(IDLE_NAP);
                }
            }
        }
    }

    fn room_active(&self) -> bool {
        self.stream.room_control().is_some()
            || self
                .xfade
                .as_ref()
                .is_some_and(|x| x.room_control().is_some())
    }

    fn set_warmup_target(&self) {
        if let Some(target) = &self.warmup_target {
            target.store(self.producer_pos, Ordering::SeqCst);
        }
    }

    fn top_up_output_stage(&mut self) -> bool {
        let s = self.shared.clone();
        let rc = s.ring_channels;
        let mut produced = false;
        let mut budget = s.ring.capacity() / self.scratch.len() + 1;
        while s.ring.available_write() >= self.scratch.len() && budget > 0 {
            budget -= 1;
            let mut frames = CHUNK_FRAMES;
            let loop_end = s.loop_end.load(Ordering::Relaxed);
            let loop_start = s.loop_start.load(Ordering::Relaxed);
            let looping = loop_end > loop_start;
            if looping && self.producer_pos < loop_end {
                frames = frames.min(loop_end - self.producer_pos);
            }
            let frames = frames as usize;
            let got = match self
                .stream
                .produce_intermediate(&mut self.scratch[..frames * rc], frames)
            {
                Ok(got) => got,
                Err(e) => {
                    s.log(level::ERROR, "monitor", message(&e));
                    s.failed.store(true, Ordering::Release);
                    return produced;
                }
            };
            if got == 0 {
                s.ended.store(true, Ordering::Relaxed);
                return produced;
            }
            s.ring.push(&self.scratch[..got * rc]);
            self.producer_pos += got as u64;
            self.set_warmup_target();
            produced = true;
            if looping && self.producer_pos >= loop_end {
                // Stage A wrap: only the source read; the consumer wraps the render later.
                if let Err(e) = self.stream.reposition_source(loop_start) {
                    s.log(level::ERROR, "monitor", message(&e));
                    s.failed.store(true, Ordering::Release);
                    return produced;
                }
                self.producer_pos = loop_start;
            }
            if got < frames {
                return produced;
            }
        }
        produced
    }

    fn restore_loop(&mut self, loop_start: u64) -> bool {
        let s = self.shared.clone();
        let cancel;
        {
            let mut c = s.lock();
            if s.quit.load(Ordering::Acquire) || c.seek_pending {
                return false;
            }
            cancel = Cancel::new();
            c.seek_cancel = cancel.clone();
            c.seek_in_progress = true;
            c.seek_target = loop_start;
        }
        let result = self.stream.seek_with_cancel(loop_start, &cancel);
        let mut c = s.lock();
        c.seek_in_progress = false;
        if cancel.requested() {
            return false;
        }
        if let Err(e) = result {
            s.log(level::ERROR, "monitor", message(&e));
            s.failed.store(true, Ordering::Release);
            return false;
        }
        self.producer_pos = loop_start;
        true
    }

    fn top_up_ring(&mut self) -> bool {
        if self.shared.output_stage {
            return self.top_up_output_stage();
        }
        let s = self.shared.clone();
        let ch = s.channels;
        let mut produced = false;
        // At most one ring's worth per call, so queued commands are not starved.
        let tracking = s.realtime && s.head_tracking_recent();
        let lead_target = if tracking {
            TRACKING_LOOKAHEAD_FRAMES * ch
        } else {
            s.ring.capacity()
        };
        let mut budget = s.ring.capacity() / self.scratch.len() + 1;
        while s.ring.available_write() >= self.scratch.len()
            && s.ring.available_read() < lead_target
            && budget > 0
        {
            budget -= 1;
            let mut frames = if tracking {
                TRACKING_CHUNK_FRAMES
            } else {
                CHUNK_FRAMES
            };
            let loop_end = s.loop_end.load(Ordering::Relaxed);
            let loop_start = s.loop_start.load(Ordering::Relaxed);
            // Loop clamping is suspended during a crossfade.
            let looping = self.xfade.is_none() && loop_end > loop_start;
            if looping && self.producer_pos < loop_end {
                frames = frames.min(loop_end - self.producer_pos);
            }
            let frames = frames as usize;
            let mut got = match self
                .stream
                .process(&mut self.scratch[..frames * ch], frames)
            {
                Ok(got) => got,
                Err(e) => {
                    s.log(level::ERROR, "monitor", message(&e));
                    s.failed.store(true, Ordering::Release);
                    return produced;
                }
            };
            if got > frames {
                abort_on_contract();
            }
            if let Some(x) = &mut self.xfade {
                let incoming = match x.process(&mut self.scratch_b[..frames * ch], frames) {
                    Ok(got) => got,
                    Err(e) => {
                        s.log(level::ERROR, "monitor", message(&e));
                        s.failed.store(true, Ordering::Release);
                        return produced;
                    }
                };
                if incoming > frames {
                    abort_on_contract();
                }
                got = got.min(incoming);
                let complete = self
                    .crossfade
                    .process(
                        &mut self.scratch[..got * ch],
                        &self.scratch_b[..got * ch],
                        got,
                    )
                    .unwrap_or_else(|_| std::process::abort());
                if complete || got < frames {
                    let mut c = s.lock();
                    self.finalize_crossfade(&mut c);
                }
            }
            if got == 0 {
                s.ended.store(true, Ordering::Relaxed);
                return produced;
            }
            self.feed_meter(got, false);
            s.ring.push(&self.scratch[..got * ch]);
            self.producer_pos += got as u64;
            self.set_warmup_target();
            produced = true;
            if looping && self.producer_pos >= loop_end && !self.restore_loop(loop_start) {
                return produced;
            }
            if got < frames {
                return produced;
            }
        }
        produced
    }

    /// Feeds the LUFS meter from the callback's output-stage tap.
    fn drain_meter_ring(&mut self) {
        if self.meter_scratch.is_empty() {
            return;
        }
        let s = self.shared.clone();
        let mut budget = s.meter_ring.capacity() / self.meter_scratch.len() + 1;
        while s.meter_ring.available_read() >= s.channels && budget > 0 {
            budget -= 1;
            let got = s.meter_ring.pop(&mut self.meter_scratch);
            if got < s.channels {
                break;
            }
            self.feed_meter(got / s.channels, true);
        }
    }
}
