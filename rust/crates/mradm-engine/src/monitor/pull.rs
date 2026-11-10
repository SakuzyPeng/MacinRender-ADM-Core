//! The monitor's device callback (`MonitorEngine::pull`): never waits or allocates.
use super::{LEVEL_CHANNELS, Pulled, Shared, State, store_f32};
use mradm_dsp::monitor;
use std::sync::atomic::Ordering;

pub(super) fn pull(s: &Shared, output: &mut [f32], frames: usize) -> Pulled {
    let ch = s.channels;
    if frames.checked_mul(ch).is_none_or(|n| n > output.len()) {
        // Device callback contract (C++ std::terminate before consuming the ring).
        std::process::abort();
    }
    let floats = frames * ch;
    let output = &mut output[..floats];
    s.in_pop.store(true, Ordering::SeqCst);
    let active = s.state() == State::Playing && !s.flushing.load(Ordering::SeqCst);
    let mut produced = 0;
    if !active {
        output.fill(0.);
    } else if s.output_stage {
        produced = output_stage(s, output, frames);
    } else {
        let got = s.ring.pop(output);
        if got < floats {
            output[got..].fill(0.);
            // A short read is an underrun only while the producer should still be feeding us.
            let done = s.ended.load(Ordering::Relaxed) || s.failed.load(Ordering::Relaxed);
            if !done && s.realtime {
                s.underruns.fetch_add(1, Ordering::Relaxed);
            }
        }
        s.frames_played
            .fetch_add((got / ch) as u64, Ordering::Relaxed);
        produced = got / ch;
    }
    let mark_end =
        !s.output_stage && s.ended.load(Ordering::Relaxed) && s.ring.available_read() == 0;
    // HpTF after the LUFS taps, before the seek transition and the levels.
    if active {
        let hptf_frames = if s.realtime {
            frames
        } else {
            frames.min(produced)
        };
        s.hptf.process(&mut output[..hptf_frames * ch]);
    }
    let mut peaks = [0f32; LEVEL_CHANNELS];
    let mut rms = [0f32; LEVEL_CHANNELS];
    // Only the device thread locks the transition history.
    let Ok(mut cb) = s.callback.try_lock() else {
        std::process::abort();
    };
    if cb
        .output
        .process(
            output,
            monitor::Callback {
                frames,
                produced_frames: produced,
                active,
                generation: s.seek_generation.load(Ordering::Acquire),
            },
            &mut peaks,
            &mut rms,
        )
        .is_err()
    {
        std::process::abort();
    }
    drop(cb);
    for c in 0..ch.min(LEVEL_CHANNELS) {
        store_f32(&s.peak[c], peaks[c]);
        store_f32(&s.rms[c], rms[c]);
    }
    s.in_pop.store(false, Ordering::SeqCst);
    Pulled {
        frames: produced,
        mark_end,
    }
}

/// Pops the buffered intermediate and renders it with the current head pose; taps the output
/// for the worker's LUFS meter.
fn output_stage(s: &Shared, output: &mut [f32], frames: usize) -> usize {
    let ch = s.channels;
    let rc = s.ring_channels;
    let pose = s.orientation_snapshot();
    // While active the worker is not parking: it never holds the stage then.
    let Ok(mut st) = s.stage.try_lock() else {
        output.fill(0.);
        return 0;
    };
    let st = &mut *st;
    let max_chunk = st.scratch.len() / rc;
    let loop_end = s.loop_end.load(Ordering::Relaxed);
    let loop_start = s.loop_start.load(Ordering::Relaxed);
    let looping = loop_end > loop_start;
    let Some(stage) = st.renderer.as_deref() else {
        output.fill(0.);
        return 0;
    };
    let mut done = 0;
    while done < frames {
        let mut want = (frames - done).min(max_chunk);
        let pos = s.consumer_src_pos.load(Ordering::Relaxed);
        if looping && pos < loop_end {
            want = want.min((loop_end - pos) as usize);
        }
        let got = s.ring.pop(&mut st.scratch[..want * rc]) / rc;
        if got == 0 {
            break;
        }
        let out = &mut output[done * ch..(done + got) * ch];
        let rendered = stage.render_output(&st.scratch[..got * rc], out, got, pose);
        s.meter_ring.push(&out[..rendered.min(got) * ch]);
        done += got;
        let pos = s.consumer_src_pos.load(Ordering::Relaxed) + got as u64;
        if looping && pos >= loop_end {
            stage.loop_render_reset(loop_start);
            s.consumer_src_pos.store(loop_start, Ordering::Relaxed);
        } else {
            s.consumer_src_pos.store(pos, Ordering::Relaxed);
        }
        if got < want {
            break;
        }
    }
    if done < frames {
        output[done * ch..].fill(0.);
        let ended = s.ended.load(Ordering::Relaxed) || s.failed.load(Ordering::Relaxed);
        if !ended && s.realtime {
            s.underruns.fetch_add(1, Ordering::Relaxed);
        }
    }
    s.frames_played.fetch_add(done as u64, Ordering::Relaxed);
    done
}
