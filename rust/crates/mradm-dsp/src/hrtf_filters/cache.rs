//! Bounded memoization owned by one live session, never by the shared bank.
//! A failed preview may warm these pure queries without changing signal state.
use super::{Filters, Lookup, continuous_cells, coordinates};
use crate::{Error, Result};
use std::sync::Arc;

const CORNER_BYTES: usize = 6 * 1024 * 1024;
const QUERY_BYTES: usize = 2 * 1024 * 1024;
const NONE: usize = usize::MAX;

#[derive(Clone, Copy)]
struct Entry<K> {
    key: Option<K>,
    older: usize,
    newer: usize,
}

/// Slabs and LRU links are allocated once. A bounded linear key scan avoids
/// hashing float keys and any allocation/rehashing on cold or evicting queries.
struct Spectra<K> {
    entries: Box<[Entry<K>]>,
    values: Box<[f32]>,
    width: usize,
    used: usize,
    oldest: usize,
    newest: usize,
}

impl<K: Copy + Eq> Spectra<K> {
    fn new(width: usize, limit: usize, budget: usize) -> Self {
        let count = width
            .checked_mul(size_of::<f32>())
            .and_then(|n| n.checked_add(size_of::<Entry<K>>()))
            .map_or(0, |bytes| {
                (budget.saturating_sub(size_of::<Self>()) / bytes).min(limit)
            });
        Self {
            entries: vec![
                Entry {
                    key: None,
                    older: NONE,
                    newer: NONE
                };
                count
            ]
            .into_boxed_slice(),
            values: vec![0.; count * width].into_boxed_slice(),
            width,
            used: 0,
            oldest: NONE,
            newest: NONE,
        }
    }

    fn clear(&mut self) {
        self.entries.fill(Entry {
            key: None,
            older: NONE,
            newer: NONE,
        });
        self.used = 0;
        self.oldest = NONE;
        self.newest = NONE;
    }

    fn touch(&mut self, slot: usize) {
        if self.newest == slot {
            return;
        }
        let Entry { older, newer, .. } = self.entries[slot];
        if older != NONE {
            self.entries[older].newer = newer;
        } else if self.oldest == slot {
            self.oldest = newer;
        }
        if newer != NONE {
            self.entries[newer].older = older;
        }
        self.entries[slot].older = self.newest;
        self.entries[slot].newer = NONE;
        if self.newest != NONE {
            self.entries[self.newest].newer = slot;
        } else {
            self.oldest = slot;
        }
        self.newest = slot;
    }

    fn find(&mut self, key: K) -> Option<usize> {
        let slot = self.entries[..self.used]
            .iter()
            .position(|e| e.key == Some(key))?;
        self.touch(slot);
        Some(slot)
    }

    fn insert(&mut self, key: K) -> Option<usize> {
        if self.entries.is_empty() {
            return None;
        }
        let slot = if self.used < self.entries.len() {
            let slot = self.used;
            self.used += 1;
            slot
        } else {
            self.oldest
        };
        self.entries[slot].key = Some(key);
        self.touch(slot);
        Some(slot)
    }

    fn get(&self, slot: usize) -> &[f32] {
        &self.values[slot * self.width..(slot + 1) * self.width]
    }

    fn get_mut(&mut self, slot: usize) -> &mut [f32] {
        &mut self.values[slot * self.width..(slot + 1) * self.width]
    }

    #[cfg(test)]
    fn storage_bytes(&self) -> usize {
        size_of::<Self>() + size_of_val(&*self.entries) + size_of_val(&*self.values)
    }
}

pub(crate) struct CachedQueries {
    bank: Arc<Filters>,
    corners: Spectra<usize>,
    queries: Spectra<[u32; 2]>,
}

impl CachedQueries {
    pub(crate) fn new(bank: Arc<Filters>) -> Self {
        Self::with_limits(bank, 512, 128)
    }

    // Also allows tests to force eviction and compare against the original
    // continuous path. No public option or C ABI surface is needed.
    pub(crate) fn with_limits(bank: Arc<Filters>, corners: usize, queries: usize) -> Self {
        let width = bank.output_len();
        Self {
            bank,
            corners: Spectra::new(width, corners, CORNER_BYTES - size_of::<Arc<Filters>>()),
            queries: Spectra::new(width, queries, QUERY_BYTES),
        }
    }

    pub(crate) fn clear(&mut self) {
        self.corners.clear();
        self.queries.clear();
    }

    pub(crate) fn query(&mut self, azimuth: f32, elevation: f32, output: &mut [f32]) -> Result<()> {
        let (az, el) = coordinates(azimuth, elevation)?;
        if output.len() != self.bank.output_len() {
            return Err(Error::InvalidArgument("Invalid HRTF query buffers"));
        }
        let key = [az.to_bits(), el.to_bits()];
        if let Some(slot) = self.queries.find(key) {
            output.copy_from_slice(self.queries.get(slot));
            return Ok(());
        }
        if self.corners.entries.is_empty() {
            self.bank
                .query(azimuth, elevation, Lookup::Continuous, output, None)?;
        } else {
            let (corners, weights) = continuous_cells(az, el);
            output.fill(0.);
            for (grid, weight) in corners.into_iter().zip(weights) {
                if weight == 0. {
                    continue;
                }
                let slot = if let Some(slot) = self.corners.find(grid) {
                    slot
                } else {
                    let slot = self.corners.insert(grid).expect("nonempty corner cache");
                    let values = self.corners.get_mut(slot);
                    for bin in 0..self.bank.bins {
                        for ear in 0..2 {
                            let h = self.bank.grid_bin(grid, bin, ear).value;
                            let index = (bin * 2 + ear) * 2;
                            values[index] = h.re;
                            values[index + 1] = h.im;
                        }
                    }
                    slot
                };
                // Retain the reference's corner order and multiply/add order,
                // including signed zero, zero weights and the phase fallback.
                for (out, h) in output.iter_mut().zip(self.corners.get(slot)) {
                    *out += weight * h;
                }
            }
        }
        if let Some(slot) = self.queries.insert(key) {
            self.queries.get_mut(slot).copy_from_slice(output);
        }
        Ok(())
    }
}

#[cfg(test)]
mod tests;
