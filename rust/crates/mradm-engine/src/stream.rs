//! File streaming over a backend preparation (PCM consistency 5.3 A): one stream for the six Rust
//! backends, moved from the four C++ `IRenderStream` shells with their open checks, cancellation
//! points and texts. The caller samples its stop token before every seek call; the Triple Balance
//! seek finish runs in its last step, the room checks only between steps, and the binaural seek
//! checks again after its last step. Control-thread validation borrows only [`Validator`], never
//! the worker-owned [`Body`].
use crate::backend::{Host, Kind, Prepared};
use mradm_render::binaural::Spread;
use mradm_render::offline::{Error, Kernel, Location, Result};
use mradm_render::room_runtime::{Control, Scope};
use mradm_render::routing::LiveOverride;
use mradm_render::{binaural_stream, room_stream, stream, triple_balance_stream};
use std::sync::Arc;

/// The plan's input format and stream controls (`RenderPlan`).
pub struct Settings<'a> {
    /// The UTF-8 input path.
    pub path: &'a str,
    pub channels: usize,
    pub frames: u64,
    pub sample_rate: u32,
    pub default_interp_ms: u32,
    pub smoothing_frames: u32,
    pub spread: Spread,
    /// The listener orientation the room stream opens with.
    pub pose: [f32; 3],
}

/// What the stream reports to `IRenderStream`.
#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub struct Info {
    pub outputs: usize,
    pub sample_rate: u32,
    /// The output layout is "binaural" rather than the plan's.
    pub binaural: bool,
    pub requires_preroll: bool,
}

/// The immutable part live edits are validated against, concurrently with the worker.
pub enum Validator {
    None,
    TripleBalance(triple_balance_stream::Source),
    Room(Scope),
}

/// The worker-owned stream.
pub enum Body {
    Speaker(Box<stream::FileStream>),
    TripleBalance(Box<triple_balance_stream::Stream>),
    /// `finishing`: the last pre-roll block is done; the next step only checks cancellation.
    Binaural {
        stream: Box<binaural_stream::FileStream>,
        finishing: bool,
    },
    Room(Box<room_stream::FileStream>),
}

fn invalid(code: i32, message: &str) -> Error {
    Error {
        code,
        message: message.to_owned(),
        location: Location::None,
    }
}

fn cancelled(message: &str) -> Error {
    invalid(5, message)
}

/// The prepared input format the C++ shells compared (the room's own rate for a room).
fn prepared_rate(p: &Prepared) -> u32 {
    p.room.as_ref().map_or(p.sample_rate, |r| r.sample_rate)
}

/// Opens the stream with the C++ `open_stream` checks after the prepared-type check.
pub fn open(p: &Prepared, s: &Settings, host: &dyn Host) -> Result<(Validator, Body, Info)> {
    let bad = || invalid(1, "无效流式渲染参数");
    match p.kind {
        Kind::Vbap | Kind::Hoa | Kind::Ear => {
            if p.kind == Kind::Hoa && (s.channels != p.inputs || s.sample_rate != p.sample_rate) {
                return Err(invalid(1, "HOA prepared/input format mismatch"));
            }
            let kernel = p.kernel.as_ref().ok_or_else(bad)?;
            let stream = stream::FileStream::open(
                s.path,
                kernel,
                Arc::clone(&p.bindings),
                stream::Settings {
                    inputs: p.inputs,
                    outputs: p.outputs,
                    frames: p.frames,
                    sample_rate: p.sample_rate,
                    default_interp_ms: s.default_interp_ms,
                    smoothing_frames: u64::from(s.smoothing_frames),
                },
            )?;
            let info = Info {
                outputs: p.outputs,
                sample_rate: s.sample_rate,
                binaural: false,
                requires_preroll: false,
            };
            Ok((Validator::None, Body::Speaker(Box::new(stream)), info))
        }
        Kind::TripleBalance => {
            let plan = match p.kernel.as_ref() {
                Some(Kernel::TripleBalance(plan)) => plan,
                _ => return Err(bad()),
            };
            let Some((mix, session)) = triple_balance_stream::Source::of(plan) else {
                return Err(invalid(
                    2,
                    "Triple Balance D mode currently supports offline rendering only",
                ));
            };
            if s.channels == 0 || p.outputs == 0 || p.sample_rate != s.sample_rate {
                return Err(invalid(1, "triple-balance: invalid stream format"));
            }
            let data = p.triple_balance.as_ref().ok_or_else(bad)?;
            let source = triple_balance_stream::Source {
                mix,
                session,
                bindings: Arc::clone(&p.bindings),
                size_tracks: Arc::clone(&data.sizes),
                spread_none: data.spread_none,
                inputs: p.inputs,
                outputs: p.outputs,
                frames: p.frames,
                sample_rate: p.sample_rate,
            };
            let stream = triple_balance_stream::Stream::open(
                s.path,
                source.clone(),
                s.default_interp_ms,
                triple_balance_stream::DEFAULT_CHECKPOINT_BUDGET,
            )?;
            let info = Info {
                outputs: p.outputs,
                sample_rate: p.sample_rate,
                binaural: false,
                requires_preroll: source.requires_preroll(),
            };
            Ok((
                Validator::TripleBalance(source),
                Body::TripleBalance(Box::new(stream)),
                info,
            ))
        }
        Kind::Binaural => {
            let plan = p.binaural.as_ref().ok_or_else(bad)?;
            let stream = binaural_stream::FileStream::open(
                Arc::clone(plan),
                s.path,
                binaural_stream::Settings {
                    inputs: s.channels,
                    frames: s.frames,
                    sample_rate: s.sample_rate,
                    smoothing_frames: s.smoothing_frames,
                    spread: s.spread,
                },
            )?;
            let info = Info {
                outputs: 2,
                sample_rate: 48_000,
                binaural: true,
                requires_preroll: true,
            };
            let body = Body::Binaural {
                stream: Box::new(stream),
                finishing: false,
            };
            Ok((Validator::None, body, info))
        }
        Kind::Room => {
            let room = p.room.as_ref().ok_or_else(bad)?;
            if s.frames != p.frames || s.channels != p.inputs || s.sample_rate != prepared_rate(p) {
                return Err(invalid(1, "room-binaural prepared 输入格式不匹配"));
            }
            let scope = host.room_scope();
            let stream = room_stream::FileStream::open(
                s.path,
                Arc::clone(&room.table),
                Arc::clone(&room.resources),
                room.config.clone(),
                s.pose,
                scope,
                host.room_switch(),
            )?;
            let info = Info {
                outputs: 2,
                sample_rate: room.sample_rate,
                binaural: true,
                requires_preroll: true,
            };
            Ok((Validator::Room(scope), Body::Room(Box::new(stream)), info))
        }
    }
}

impl Validator {
    /// `IRenderStream::validate_overrides`: code, message and context (the object id).
    pub fn validate(
        &self,
        overrides: &[LiveOverride],
    ) -> std::result::Result<(), (i32, String, Vec<u8>)> {
        match self {
            Self::None => Ok(()),
            Self::TripleBalance(source) => source.validate_overrides(overrides),
            Self::Room(scope) => room_stream::validate_overrides(overrides, *scope)
                .map_err(|(code, message)| (code, message, Vec::new())),
        }
    }
}

impl Body {
    pub fn process(&mut self, output: &mut [f32], frames: usize) -> Result<usize> {
        match self {
            Self::Speaker(s) => s.process(output, frames),
            Self::TripleBalance(s) => s.process(output, frames),
            Self::Binaural { stream, .. } => stream.process(output, frames),
            Self::Room(s) => s.process(output, frames),
        }
    }

    /// Starts a seek; `cancelled` is the stop token sampled before this call. True while
    /// [`Self::seek_step`] must be called again.
    pub fn seek_begin(&mut self, frame: u64, cancelled_now: bool) -> Result<bool> {
        match self {
            Self::Speaker(s) => {
                // IRenderStream::seek_with_cancel's default check before one seek.
                if cancelled_now {
                    return Err(cancelled("stream seek cancelled"));
                }
                s.seek(frame).map(|()| false)
            }
            Self::TripleBalance(s) => {
                if cancelled_now {
                    return Err(cancelled("Triple Balance seek cancelled"));
                }
                if s.seek_begin(frame)? {
                    return Ok(true);
                }
                s.seek_finish().map(|()| false)
            }
            Self::Binaural { stream, finishing } => {
                if cancelled_now {
                    return Err(cancelled(BINAURAL_CANCELLED));
                }
                *finishing = !stream.seek_begin(frame)?;
                Ok(true)
            }
            Self::Room(s) => s.seek_begin(frame),
        }
    }

    pub fn seek_step(&mut self, cancelled_now: bool) -> Result<bool> {
        match self {
            Self::Speaker(_) => Ok(false),
            Self::TripleBalance(s) => {
                if cancelled_now {
                    return Err(cancelled("Triple Balance seek cancelled"));
                }
                if s.seek_step()? {
                    return Ok(true);
                }
                s.seek_finish().map(|()| false)
            }
            Self::Binaural { stream, finishing } => {
                if cancelled_now {
                    return Err(cancelled(BINAURAL_CANCELLED));
                }
                if *finishing {
                    *finishing = false;
                    return Ok(false);
                }
                *finishing = !stream.seek_step()?;
                Ok(true)
            }
            Self::Room(s) => {
                if cancelled_now {
                    return Err(cancelled("room-binaural 预热已取消"));
                }
                s.seek_step()
            }
        }
    }

    /// Replaces the live edits; the room latches a rejection for the next `process`.
    pub fn set_overrides(&mut self, overrides: &[LiveOverride]) -> Result<()> {
        match self {
            Self::Speaker(s) => {
                let base: Vec<_> = overrides.iter().map(|o| o.base).collect();
                s.set_overrides(&base)
            }
            Self::TripleBalance(s) => s.set_overrides(overrides),
            Self::Binaural { stream, .. } => stream.set_overrides(overrides),
            Self::Room(s) => {
                s.set_overrides(overrides);
                Ok(())
            }
        }
    }

    /// The live listener orientation; loudspeaker and Triple Balance streams ignore it.
    pub fn set_pose(&mut self, pose: [f32; 3]) {
        match self {
            Self::Binaural { stream, .. } => stream.set_pose(pose),
            Self::Room(s) => s.set_pose(pose),
            Self::Speaker(_) | Self::TripleBalance(_) => {}
        }
    }

    /// The tail a room appends after the programme.
    pub fn tail_frames(&self) -> u64 {
        match self {
            Self::Room(s) => s.tail_frames(),
            _ => 0,
        }
    }

    pub fn control(&self) -> Option<Control> {
        match self {
            Self::Room(s) => Some(s.control()),
            _ => None,
        }
    }
}

const BINAURAL_CANCELLED: &str = "双耳文件流式跳转已取消";
