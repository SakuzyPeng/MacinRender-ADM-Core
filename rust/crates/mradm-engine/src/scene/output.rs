//! The Scene output session (PCM consistency 5.4 C, formerly the C++ `SceneOutputSession`)
//! without its device: the device callback's pull, the stereo headphone feed's prefetch, HpTF and
//! sample-peak guard, the volume, the epoch reset and the status. The device itself (miniaudio,
//! the system spatial sinks, test captures) stays a C++ island, so every step that touches it is
//! split out: the shell parks the callback here, pauses and flushes the device, then resets.
//!
//! Callback rules: [`Session::pull`] neither waits nor allocates. Its callback-private state
//! (guard, prefetch buffer, HpTF processor) sits behind locks that, by the park protocol, only
//! the callback holds while it runs; it takes them with `try_lock`.
use super::stream::{Status as StreamStatus, Stream, pull_flag};
use mradm_dsp::diagnostics::{self, Words};
use mradm_dsp::gain;
use mradm_dsp::hptf::{self, Live, StereoSnapshot};
use mradm_dsp::peak_guard::{PULL_FRAMES, StereoPeakGuard};
use mradm_render::routing::Error;
use std::sync::atomic::{AtomicBool, AtomicU32, AtomicU64, Ordering};
use std::sync::{Arc, Mutex, MutexGuard, TryLockError};

/// `adm::ErrorCode` values of the session's own errors.
const INVALID_ARGUMENT: i32 = 1;
const UNSUPPORTED: i32 = 2;

fn error(code: i32, message: &str) -> Error {
    Error {
        code,
        message: message.as_bytes().to_vec(),
        context: Vec::new(),
    }
}

/// `SceneOutputState`.
#[derive(Clone, Copy, Debug, PartialEq, Eq)]
#[repr(u32)]
pub enum State {
    Paused = 0,
    Playing = 1,
    Buffering = 2,
    Draining = 3,
    Ended = 4,
    Failed = 5,
}

/// `AudioDeviceProgress`, reported by the device.
#[derive(Clone, Copy, Debug, Default, PartialEq, Eq)]
pub struct DeviceProgress {
    pub presented_frames: u64,
    pub media_clock: bool,
    pub failed: bool,
    pub recovering: bool,
    pub underruns: u64,
}

/// `SceneDeviceStatus`.
#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub struct Status {
    pub state: State,
    pub epoch: u64,
    pub consumed_frames: u64,
    pub presented_frames: u64,
    pub queued_frames: u64,
    pub underruns: u64,
    pub media_clock: bool,
    pub recovering: bool,
    pub failed: bool,
    pub ended: bool,
}

/// One callback's result. While `active`, the pull is still counted: the shell marks the
/// device's end of stream (when `eos`) and then calls [`Session::release`].
#[derive(Clone, Copy, Debug, Default, PartialEq, Eq)]
pub struct Pulled {
    pub frames: usize,
    pub eos: bool,
    pub active: bool,
}

/// The stereo headphone feed: prefetch buffer and sample-peak guard, owned by the callback.
struct Feed {
    guard: StereoPeakGuard,
    input: Vec<f32>,
    ended: bool,
}

struct Stereo {
    feed: Mutex<Feed>,
    hptf: Live,
}

fn owned<T>(slot: &Mutex<T>) -> Option<MutexGuard<'_, T>> {
    match slot.try_lock() {
        Ok(guard) => Some(guard),
        Err(TryLockError::Poisoned(poisoned)) => Some(poisoned.into_inner()),
        Err(TryLockError::WouldBlock) => None,
    }
}

/// One output consumer of a [`Stream`].
pub struct Session {
    stream: Arc<Stream>,
    channels: u32,
    sample_rate: u32,
    device_volume: bool,
    stereo: Option<Stereo>,
    playing: AtomicBool,
    pulls: AtomicU32,
    volume: AtomicU32,
    consumed: AtomicU64,
    presented: AtomicU64,
    eos: AtomicBool,
    failed: AtomicBool,
    buffering: AtomicBool,
}

impl Session {
    /// Attach to `stream`. A protected two-channel output is a headphone feed: it gets the
    /// sample-peak guard and HpTF, designed for the stream's rate (the device resamples after).
    pub fn new(
        stream: Arc<Stream>,
        protect_stereo: bool,
        device_volume: bool,
    ) -> Result<Self, Error> {
        if !stream.attach_output() {
            return Err(error(
                INVALID_ARGUMENT,
                "Scene stream already has an output consumer",
            ));
        }
        let (sample_rate, channels) = stream.output_format();
        let stereo = if protect_stereo && channels == 2 {
            let made = StereoPeakGuard::new(sample_rate).and_then(|guard| {
                Ok(Stereo {
                    feed: Mutex::new(Feed {
                        guard,
                        input: vec![0.; PULL_FRAMES * 2],
                        ended: false,
                    }),
                    hptf: Live::new(2, sample_rate)?,
                })
            });
            match made {
                Ok(stereo) => Some(stereo),
                Err(e) => {
                    stream.detach_output();
                    return Err(Error {
                        code: 6,
                        message: e.to_string().into_bytes(),
                        context: Vec::new(),
                    });
                }
            }
        } else {
            None
        };
        Ok(Self {
            stream,
            channels,
            sample_rate,
            device_volume,
            stereo,
            playing: AtomicBool::new(false),
            pulls: AtomicU32::new(0),
            volume: AtomicU32::new(1.0f32.to_bits()),
            consumed: AtomicU64::new(0),
            presented: AtomicU64::new(0),
            eos: AtomicBool::new(false),
            failed: AtomicBool::new(false),
            buffering: AtomicBool::new(true),
        })
    }

    pub fn channels(&self) -> u32 {
        self.channels
    }

    pub fn sample_rate(&self) -> u32 {
        self.sample_rate
    }

    /// Whether this output owns the stereo headphone feed (peak guard and HpTF).
    pub fn hptf_supported(&self) -> bool {
        self.stereo.is_some()
    }

    fn volume(&self) -> f32 {
        f32::from_bits(self.volume.load(Ordering::SeqCst))
    }

    /// Device callback: fill `output` (`frames` interleaved frames) with media.
    pub fn pull(&self, output: &mut [f32], frames: usize) -> Pulled {
        output.fill(0.);
        self.pulls.fetch_add(1, Ordering::SeqCst);
        if !self.playing.load(Ordering::SeqCst) {
            self.pulls.fetch_sub(1, Ordering::SeqCst);
            return Pulled::default();
        }
        // The previous callback has been submitted: a callback-resolution estimate, kept apart
        // from a buffered sink's media presentation clock.
        self.presented
            .store(self.consumed.load(Ordering::SeqCst), Ordering::SeqCst);
        let channels = self.channels as usize;
        let count = frames
            .min(u32::MAX as usize)
            .min(output.len() / channels.max(1)) as u32;
        let (flags, first, media) = match &self.stereo {
            Some(stereo) => self.pull_stereo(stereo, output, count),
            None => {
                let pulled = self.stream.pull(output, count);
                (pulled.flags, pulled.first_media_frame, pulled.media_frames)
            }
        };
        self.consumed
            .store(first + u64::from(media), Ordering::SeqCst);
        self.buffering.store(
            flags & (pull_flag::BUFFERING | pull_flag::UNDERRUN) != 0,
            Ordering::SeqCst,
        );
        self.failed
            .store(flags & pull_flag::FAILED != 0, Ordering::SeqCst);
        let eos = flags & pull_flag::EOS != 0;
        if eos {
            self.eos.store(true, Ordering::SeqCst);
        }
        let samples = media as usize * channels;
        if self.stereo.is_none() && !self.device_volume {
            let _ = gain::scale(&mut output[..samples], self.volume());
        }
        if diagnostics::ENABLED && media != 0 {
            let name = format!(
                "device/e{}-s{first}.90-output.f32",
                self.stream.status().epoch
            );
            let _ = diagnostics::process(&name, Words::F32(&output[..samples]));
        }
        Pulled {
            frames: media as usize,
            eos,
            active: true,
        }
    }

    /// Ends an `active` pull.
    pub fn release(&self) {
        self.pulls.fetch_sub(1, Ordering::SeqCst);
    }

    /// The stereo feed: guarded output, refilled with HpTF-processed prefetches.
    fn pull_stereo(&self, stereo: &Stereo, output: &mut [f32], frames: u32) -> (u32, u64, u32) {
        let first = self.consumed.load(Ordering::SeqCst);
        let mut flags = 0;
        let mut media = 0u32;
        let Some(mut feed) = owned(&stereo.feed) else {
            return (pull_flag::BUFFERING, first, 0);
        };
        let feed = &mut *feed;
        let volume = self.volume();
        while media < frames {
            let destination = &mut output[media as usize * 2..frames as usize * 2];
            let produced = feed.guard.pop(destination, volume, feed.ended).unwrap_or(0);
            media += produced as u32;
            if media == frames || feed.ended {
                break;
            }
            // Unsigned as in the former C++: never negative after a short pop, wraps otherwise.
            let needed = ((frames - media) as usize)
                .wrapping_add(feed.guard.lookahead_frames())
                .wrapping_sub(feed.guard.buffered_frames());
            let status = self.stream.status();
            // Prefetch only available PCM. A short speculative read must not count as an
            // underrun; the one-frame probe also discovers EOS on an empty ring.
            let request = PULL_FRAMES
                .min(feed.guard.writable_frames())
                .min(needed)
                .min((status.buffered_output_frames as usize).max(1));
            let pulled = self.stream.pull(&mut feed.input, request as u32);
            flags |=
                pulled.flags & (pull_flag::BUFFERING | pull_flag::UNDERRUN | pull_flag::FAILED);
            feed.ended = pulled.flags & pull_flag::EOS != 0;
            let samples = pulled.media_frames as usize * 2;
            // Headphone compensation, upstream of the guard so a boosted band meets its ceiling.
            let key = diagnostics::ENABLED
                .then(|| format!("device/e{}-s{}", pulled.epoch, pulled.first_media_frame));
            if let Some(key) = &key
                && samples != 0
            {
                let _ = diagnostics::process(
                    &format!("{key}.70-before-hptf.f32"),
                    Words::F32(&feed.input[..samples]),
                );
            }
            stereo.hptf.process(&mut feed.input[..samples]);
            if let Some(key) = &key
                && samples != 0
            {
                let _ = diagnostics::process(
                    &format!("{key}.80-hptf.f32"),
                    Words::F32(&feed.input[..samples]),
                );
            }
            let _ = feed.guard.push(&feed.input[..samples]);
            if pulled.media_frames == 0 && !feed.ended {
                break;
            }
        }
        if feed.ended && feed.guard.buffered_frames() == 0 {
            flags |= pull_flag::EOS;
        }
        (flags, first, media)
    }

    /// Stop the callback and wait until no pull is inside the session. The shell then pauses
    /// the device.
    pub fn park(&self) {
        self.playing.store(false, Ordering::SeqCst);
        while self.pulls.load(Ordering::SeqCst) != 0 {
            std::thread::yield_now();
        }
    }

    /// Let the callback pull again; the shell then resumes the device.
    pub fn play(&self) {
        self.playing.store(true, Ordering::SeqCst);
    }

    /// The stale-epoch check that runs before the device queue is disturbed.
    pub fn check_epoch(&self, epoch: u64) -> Result<(), Error> {
        if epoch <= self.stream.status().epoch {
            return Err(error(INVALID_ARGUMENT, "Scene output epoch must increase"));
        }
        Ok(())
    }

    /// After [`park`](Self::park) and the device pause and flush: reset the stream and the
    /// session for `epoch`. Pausing alone keeps the guard's lookahead and the HpTF history, so
    /// the signal and the filter stay sample-continuous.
    pub fn reset_epoch(&self, epoch: u64, target: i64) -> Result<(), Error> {
        self.stream.begin_epoch(epoch, target)?;
        self.consumed.store(0, Ordering::SeqCst);
        self.presented.store(0, Ordering::SeqCst);
        self.eos.store(false, Ordering::SeqCst);
        self.failed.store(false, Ordering::SeqCst);
        self.buffering.store(true, Ordering::SeqCst);
        if let Some(stereo) = &self.stereo {
            let mut feed = stereo.feed.lock().unwrap_or_else(|e| e.into_inner());
            feed.guard.reset();
            feed.ended = false;
            stereo.hptf.reset_state();
        }
        Ok(())
    }

    /// Validate and store the volume; the shell then forwards it to the device.
    pub fn set_volume(&self, gain: f32) -> Result<(), Error> {
        if !gain.is_finite() || !(0. ..=1.).contains(&gain) {
            return Err(error(
                INVALID_ARGUMENT,
                "Scene output volume must be finite and in [0, 1]",
            ));
        }
        self.volume.store(gain.to_bits(), Ordering::SeqCst);
        Ok(())
    }

    /// Publish designed coefficients, or the bypass when `None`; no park, the swap is a blend.
    pub fn set_hptf(
        &self,
        coefficients: Option<hptf::Coefficients>,
        revision: u64,
    ) -> Result<(), Error> {
        let Some(stereo) = &self.stereo else {
            return Err(error(
                UNSUPPORTED,
                "耳机补偿(HpTF)只适用于立体声耳机输出;系统空间音频与多声道输出不支持",
            ));
        };
        let published = match coefficients {
            Some(coefficients) => stereo.hptf.publish(
                hptf::Snapshot {
                    coefficients,
                    revision,
                }
                .into(),
            ),
            None => stereo.hptf.publish_bypass(revision),
        };
        published.map_err(|e| Error {
            code: INVALID_ARGUMENT,
            message: e.to_string().into_bytes(),
            context: Vec::new(),
        })
    }

    /// The refusal of a prepared profile on an output without the headphone feed.
    pub fn check_hptf_settings(&self) -> Result<(), Error> {
        if self.stereo.is_none() {
            return Err(error(UNSUPPORTED, "耳机补偿(HpTF)只适用于立体声耳机输出"));
        }
        Ok(())
    }

    /// Publish a snapshot designed for [`sample_rate`](Self::sample_rate).
    pub fn publish_hptf(&self, snapshot: StereoSnapshot) -> Result<(), Error> {
        self.check_hptf_settings()?;
        let stereo = self.stereo.as_ref().expect("checked");
        stereo.hptf.publish(snapshot).map_err(|e| Error {
            code: INVALID_ARGUMENT,
            message: e.to_string().into_bytes(),
            context: Vec::new(),
        })
    }

    /// The target the callback applied last (a bypass at the stream rate before any).
    pub fn hptf_snapshot(&self) -> StereoSnapshot {
        match &self.stereo {
            Some(stereo) => stereo.hptf.active_snapshot(),
            None => hptf::Snapshot {
                coefficients: hptf::Coefficients::default(),
                revision: 0,
            }
            .into(),
        }
    }

    /// `SceneDeviceStatus` from the device's progress and the stream status.
    pub fn status(&self, device: DeviceProgress) -> Status {
        let stream: StreamStatus = self.stream.status();
        let consumed = self.consumed.load(Ordering::SeqCst);
        let presented = consumed.min(if device.media_clock {
            device.presented_frames
        } else {
            self.presented.load(Ordering::SeqCst)
        });
        let queued = consumed - presented;
        let failed = device.failed || self.failed.load(Ordering::SeqCst) || stream.failed;
        let eos = self.eos.load(Ordering::SeqCst);
        let ended = eos && queued == 0 && !failed;
        let state = if failed {
            State::Failed
        } else if ended {
            State::Ended
        } else if !self.playing.load(Ordering::SeqCst) {
            State::Paused
        } else if eos {
            State::Draining
        } else if self.buffering.load(Ordering::SeqCst) {
            State::Buffering
        } else {
            State::Playing
        };
        Status {
            state,
            epoch: stream.epoch,
            consumed_frames: consumed,
            presented_frames: presented,
            queued_frames: queued,
            underruns: if device.media_clock {
                device.underruns
            } else {
                stream.underruns
            },
            media_clock: device.media_clock,
            recovering: device.recovering,
            failed,
            ended,
        }
    }
}

impl Drop for Session {
    fn drop(&mut self) {
        self.playing.store(false, Ordering::SeqCst);
        self.stream.detach_output();
    }
}
