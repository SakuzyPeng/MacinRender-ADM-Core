//! The hot-swappable HpTF processor shared by a control thread and an audio callback (formerly
//! the C++ `HptfProcessor` and `HptfMailbox`). Control publishes designed targets, the callback
//! consumes them into a [`Processor`] and reports what it applied back.
//!
//! Each [`Mailbox`] is single producer / single consumer with three slots: the producer owns one,
//! the consumer owns one and the third is exchanged through an atomic index. A slot is only ever
//! touched by the side that owns it, so its lock is never contended; the callback side uses
//! `try_lock` and never waits or allocates. A newer publication replaces a pending one.
use super::{Coefficients, Processor, Snapshot, StereoSnapshot};
use crate::Result;
use crate::diagnostics::{self, Words};
use std::sync::atomic::{AtomicU32, Ordering};
use std::sync::{Mutex, MutexGuard, TryLockError};

const DIRTY: u32 = 4;
const INDEX: u32 = 3;

fn owned<T>(slot: &Mutex<T>) -> Option<MutexGuard<'_, T>> {
    match slot.try_lock() {
        Ok(guard) => Some(guard),
        Err(TryLockError::Poisoned(poisoned)) => Some(poisoned.into_inner()),
        Err(TryLockError::WouldBlock) => None,
    }
}

/// Three-slot SPSC hand-off; callers serialize their own side.
pub struct Mailbox<T> {
    slots: [Mutex<T>; 3],
    middle: AtomicU32,
    write: AtomicU32,
    read: AtomicU32,
}

impl<T: Copy> Mailbox<T> {
    pub fn new(initial: T) -> Self {
        Self {
            slots: [
                Mutex::new(initial),
                Mutex::new(initial),
                Mutex::new(initial),
            ],
            middle: AtomicU32::new(2),
            write: AtomicU32::new(0),
            read: AtomicU32::new(1),
        }
    }

    /// Producer side: write the private slot, then exchange it with the middle one.
    pub fn publish(&self, value: T) {
        let slot = self.write.load(Ordering::Relaxed);
        if let Some(mut guard) = owned(&self.slots[slot as usize]) {
            *guard = value;
        }
        let previous = self.middle.swap(slot | DIRTY, Ordering::AcqRel);
        self.write.store(previous & INDEX, Ordering::Relaxed);
    }

    /// Consumer side: take the newest publication, if any arrived since the last consume.
    pub fn consume(&self) -> Option<T> {
        if self.middle.load(Ordering::Acquire) & DIRTY == 0 {
            return None;
        }
        let previous = self.read.load(Ordering::Relaxed);
        let slot = self.middle.swap(previous, Ordering::AcqRel) & INDEX;
        self.read.store(slot, Ordering::Relaxed);
        owned(&self.slots[slot as usize]).map(|guard| *guard)
    }
}

fn snapshot(coefficients: Coefficients, revision: u64) -> StereoSnapshot {
    Snapshot {
        coefficients,
        revision,
    }
    .into()
}

/// `hptf/r{revision}.10-[left-|right-]coefficients` checkpoints of one publication.
fn trace(target: &StereoSnapshot) {
    if !diagnostics::ENABLED {
        return;
    }
    let dump = |c: &Coefficients, side: &str| {
        let mut values = vec![
            c.preamp_gain,
            c.max_response_db,
            c.auto_trim_db,
            c.preamp_db,
        ];
        for s in &c.sections[..c.band_count.min(c.sections.len())] {
            values.extend([s.b0, s.b1, s.b2, s.a1, s.a2]);
        }
        let key = format!("hptf/r{}.10-{side}coefficients", target.revision);
        let _ = diagnostics::process(&format!("{key}.f32"), Words::F32(&values));
        let shape = [c.sample_rate as i32, c.band_count as i32];
        let _ = diagnostics::process(&format!("{key}-shape.i32"), Words::I32(&shape));
    };
    dump(
        &target.coefficients[0],
        if target.independent { "left-" } else { "" },
    );
    if target.independent {
        dump(&target.coefficients[1], "right-");
    }
}

/// A processor with its two mailboxes. Linear blends between targets, see [`Processor`].
pub struct Live {
    channels: usize,
    sample_rate: u32,
    publication: Mutex<()>,
    pending: Mailbox<StereoSnapshot>,
    processor: Mutex<Processor>,
    status: Mailbox<StereoSnapshot>,
    observed: Mutex<StereoSnapshot>,
}

impl Live {
    /// `channels` is the output width; targets must be designed for `sample_rate`.
    pub fn new(channels: usize, sample_rate: u32) -> Result<Self> {
        let processor = Processor::new(channels, sample_rate)?;
        let bypass = snapshot(
            Coefficients {
                sample_rate,
                ..Coefficients::default()
            },
            0,
        );
        let status = Mailbox::new(bypass);
        status.publish(bypass);
        Ok(Self {
            channels,
            sample_rate,
            publication: Mutex::new(()),
            pending: Mailbox::new(bypass),
            processor: Mutex::new(processor),
            status,
            observed: Mutex::new(snapshot(Coefficients::default(), 0)),
        })
    }

    pub fn channels(&self) -> usize {
        self.channels
    }

    pub fn sample_rate(&self) -> u32 {
        self.sample_rate
    }

    /// Control thread: queue a designed target. A target the processor would reject is refused
    /// here, so the callback never meets one.
    pub fn publish(&self, target: StereoSnapshot) -> Result<()> {
        Processor::check_target(self.channels, self.sample_rate, &target)?;
        trace(&target);
        let _serial = self.publication.lock().unwrap_or_else(|e| e.into_inner());
        self.pending.publish(target);
        Ok(())
    }

    /// Control thread: the unit-gain, zero-band target; enabling, disabling and switching
    /// profiles are the same blend.
    pub fn publish_bypass(&self, revision: u64) -> Result<()> {
        self.publish(snapshot(
            Coefficients {
                sample_rate: self.sample_rate,
                ..Coefficients::default()
            },
            revision,
        ))
    }

    /// Only while the callback is parked: adopt the newest target, then clear the histories.
    pub fn reset_state(&self) {
        let mut processor = self.processor.lock().unwrap_or_else(|e| e.into_inner());
        let target = self.pending.consume();
        if let Ok(update) = processor.reset_stereo(target) {
            self.status.publish(update.snapshot);
        }
    }

    /// Audio callback: process interleaved PCM in place; never waits or allocates.
    pub fn process(&self, samples: &mut [f32]) {
        // A buffer the processor would reject must not consume the pending target.
        if samples.is_empty() || self.channels == 0 || !samples.len().is_multiple_of(self.channels)
        {
            return;
        }
        let Some(mut processor) = owned(&self.processor) else {
            return;
        };
        let target = self.pending.consume();
        if let Ok(update) = processor.process_stereo(samples, target)
            && update.applied
        {
            self.status.publish(update.snapshot);
        }
    }

    /// Control thread: the target the callback reported last.
    pub fn active_snapshot(&self) -> StereoSnapshot {
        let mut observed = self.observed.lock().unwrap_or_else(|e| e.into_inner());
        if let Some(latest) = self.status.consume() {
            *observed = latest;
        }
        *observed
    }

    pub fn applied_revision(&self) -> u64 {
        self.active_snapshot().revision
    }
}

#[cfg(test)]
mod tests {
    use super::*;
    use crate::hptf::{BLEND_FRAMES, Biquad};

    fn gain(rate: u32, preamp_gain: f32) -> Coefficients {
        Coefficients {
            sample_rate: rate,
            preamp_gain,
            ..Coefficients::default()
        }
    }

    #[test]
    fn mailbox_keeps_the_newest_publication() {
        let mailbox = Mailbox::new(0u32);
        assert_eq!(mailbox.consume(), None);
        mailbox.publish(1);
        mailbox.publish(2);
        assert_eq!(mailbox.consume(), Some(2));
        assert_eq!(mailbox.consume(), None);
        for value in 3..10 {
            mailbox.publish(value);
            assert_eq!(mailbox.consume(), Some(value));
        }
    }

    #[test]
    fn mailbox_hands_off_across_threads() {
        let mailbox = std::sync::Arc::new(Mailbox::new([0u64; 8]));
        let producer = {
            let mailbox = mailbox.clone();
            std::thread::spawn(move || {
                for i in 1..=20_000u64 {
                    mailbox.publish([i; 8]);
                }
            })
        };
        let mut last = 0;
        let deadline = std::time::Instant::now() + std::time::Duration::from_secs(10);
        while last < 20_000 && std::time::Instant::now() < deadline {
            if let Some(value) = mailbox.consume() {
                assert!(value.iter().all(|&v| v == value[0]), "torn slot");
                assert!(value[0] > last, "stale or repeated slot");
                last = value[0];
            }
        }
        producer.join().unwrap();
        assert_eq!(last, 20_000, "the final publication arrives");
    }

    #[test]
    fn targets_blend_report_and_reset() {
        let live = Live::new(2, 48_000).unwrap();
        assert_eq!(live.applied_revision(), 0);
        assert_eq!(live.active_snapshot().coefficients[0].sample_rate, 48_000);
        live.publish(snapshot(gain(48_000, 0.5), 1)).unwrap();
        let mut pcm = vec![1.0f32; 2 * (BLEND_FRAMES - 1)];
        live.process(&mut pcm);
        assert_eq!(live.applied_revision(), 0, "the fade is still running");
        // Queued behind the active fade, then adopted by a reset.
        live.publish(snapshot(gain(48_000, 0.25), 2)).unwrap();
        live.reset_state();
        assert_eq!(live.applied_revision(), 2);
        let mut pcm = vec![1.0f32; 8];
        live.process(&mut pcm);
        assert!(pcm.iter().all(|&s| s == 0.25));
        live.publish_bypass(3).unwrap();
        let mut pcm = vec![1.0f32; 2 * BLEND_FRAMES];
        live.process(&mut pcm);
        assert_eq!(live.applied_revision(), 3);
        assert!(live.active_snapshot().coefficients[0].is_bypass());
    }

    #[test]
    fn invalid_targets_are_refused_on_publication() {
        let live = Live::new(2, 48_000).unwrap();
        assert!(live.publish(snapshot(gain(44_100, 0.5), 1)).is_err());
        let mut bad = gain(48_000, 1.0);
        bad.band_count = 1;
        bad.sections[0] = Biquad {
            b0: f32::NAN,
            ..Biquad::default()
        };
        assert!(live.publish(snapshot(bad, 2)).is_err());
        let mut pcm = vec![1.0f32; 2 * BLEND_FRAMES];
        live.process(&mut pcm);
        assert_eq!(live.applied_revision(), 0);
        assert!(pcm.iter().all(|&s| s == 1.0));
    }

    #[test]
    fn a_malformed_buffer_keeps_the_pending_target() {
        let live = Live::new(2, 48_000).unwrap();
        live.publish(snapshot(gain(48_000, 0.5), 1)).unwrap();
        let mut odd = vec![1.0f32; 3];
        live.process(&mut odd);
        assert_eq!(odd, [1.0; 3]);
        live.reset_state();
        assert_eq!(live.applied_revision(), 1, "the target survived");
    }
}
