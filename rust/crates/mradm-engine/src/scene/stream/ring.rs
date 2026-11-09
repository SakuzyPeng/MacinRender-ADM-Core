//! The worker-to-player output ring (formerly the C++ `FloatRingBuffer` behind
//! `PlayerOutputEngine`): single producer, single consumer, wait-free on both sides. Each side owns
//! one index and reads the other's; one slot stays free so full and empty differ. Samples are
//! stored as f32 bits; the indices publish them with release / acquire.
use std::sync::atomic::{AtomicU32, AtomicUsize, Ordering};

pub(super) struct Ring {
    slots: Box<[AtomicU32]>,
    write: AtomicUsize,
    read: AtomicUsize,
}

impl Ring {
    /// A ring holding `capacity` floats at once.
    pub(super) fn new(capacity: usize) -> Self {
        Self {
            slots: (0..=capacity).map(|_| AtomicU32::new(0)).collect(),
            write: AtomicUsize::new(0),
            read: AtomicUsize::new(0),
        }
    }

    /// Consumer-side count of floats available; from the producer side only a status metric.
    pub(super) fn available_read(&self) -> usize {
        let w = self.write.load(Ordering::Acquire);
        let r = self.read.load(Ordering::Relaxed);
        (w + self.slots.len() - r) % self.slots.len()
    }

    /// Writes up to `source.len()` floats; returns how many fit.
    pub(super) fn push(&self, source: &[f32]) -> usize {
        let size = self.slots.len();
        let mut w = self.write.load(Ordering::Relaxed);
        let r = self.read.load(Ordering::Acquire);
        let free = (r + size - w - 1) % size;
        let count = source.len().min(free);
        for &value in &source[..count] {
            self.slots[w].store(value.to_bits(), Ordering::Relaxed);
            w = (w + 1) % size;
        }
        self.write.store(w, Ordering::Release);
        count
    }

    /// Reads up to `destination.len()` floats; returns how many were available.
    pub(super) fn pop(&self, destination: &mut [f32]) -> usize {
        let size = self.slots.len();
        let w = self.write.load(Ordering::Acquire);
        let mut r = self.read.load(Ordering::Relaxed);
        let available = (w + size - r) % size;
        let count = destination.len().min(available);
        for value in &mut destination[..count] {
            *value = f32::from_bits(self.slots[r].load(Ordering::Relaxed));
            r = (r + 1) % size;
        }
        self.read.store(r, Ordering::Release);
        count
    }

    /// Empties the ring; only while neither side runs (the epoch gate holds the consumer off).
    pub(super) fn clear(&self) {
        self.write.store(0, Ordering::Relaxed);
        self.read.store(0, Ordering::Relaxed);
    }
}

#[cfg(test)]
mod tests {
    use super::Ring;

    #[test]
    fn the_ring_keeps_one_slot_free_and_wraps() {
        let ring = Ring::new(4);
        assert_eq!(ring.push(&[1., 2., 3., 4., 5.]), 4);
        let mut out = [0f32; 3];
        assert_eq!(ring.pop(&mut out), 3);
        assert_eq!(out, [1., 2., 3.]);
        assert_eq!(ring.push(&[6., 7., 8.]), 3);
        assert_eq!(ring.available_read(), 4);
        let mut rest = [0f32; 8];
        assert_eq!(ring.pop(&mut rest), 4);
        assert_eq!(&rest[..4], &[4., 6., 7., 8.]);
        ring.push(&[-0.]);
        ring.clear();
        assert_eq!(ring.available_read(), 0);
    }
}
