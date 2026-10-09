//! Bounded memoization owned by one live session, never by the shared bank.
//! A failed preview may warm these pure queries without changing signal state.
use super::{Filters, Lookup, continuous_cells, coordinates};
use crate::{Error, Result, hrtf::GRID_POINTS};
use std::sync::Arc;

mod index;
use index::QueryIndex;

const CORNER_LIMIT: usize = 1536;
const QUERY_LIMIT: usize = 512;
// Guarantee the slot counts at the normal 2048-point HRTF FFT, including keys,
// links and indexes. Longer spectra reduce capacity within the same byte caps.
const TARGET_WIDTH: usize = (2048 / 2 + 1) * 4;
const CORNER_INDEX_BYTES: usize = size_of::<Box<[u16]>>() + GRID_POINTS * size_of::<u16>();
const CORNER_BYTES: usize = size_of::<Arc<Filters>>()
    + CORNER_INDEX_BYTES
    + size_of::<Spectra<usize>>()
    + CORNER_LIMIT * (TARGET_WIDTH * size_of::<f32>() + size_of::<Entry<usize>>());
const QUERY_BYTES: usize = QueryIndex::storage_for(QUERY_LIMIT)
    + size_of::<Spectra<[u32; 2]>>()
    + QUERY_LIMIT * (TARGET_WIDTH * size_of::<f32>() + size_of::<Entry<[u32; 2]>>());
const NONE: usize = usize::MAX;
const EMPTY_SLOT: u16 = u16::MAX;

#[derive(Clone, Copy)]
struct Entry<K> {
    key: Option<K>,
    older: usize,
    newer: usize,
}

/// Fixed spectrum slabs and LRU links. Separate indexes locate the slots.
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

    #[cfg(test)]
    fn find(&mut self, key: K) -> Option<usize> {
        let slot = self.entries[..self.used]
            .iter()
            .position(|e| e.key == Some(key))?;
        self.touch(slot);
        Some(slot)
    }

    fn insert(&mut self, key: K) -> Option<(usize, Option<K>)> {
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
        let previous = self.entries[slot].key.replace(key);
        self.touch(slot);
        Some((slot, previous))
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
    corner_index: Box<[u16]>,
    query_index: QueryIndex,
}

impl CachedQueries {
    pub(crate) fn new(bank: Arc<Filters>) -> Self {
        Self::with_limits(bank, CORNER_LIMIT, QUERY_LIMIT)
    }

    // Also allows tests to force eviction and compare against the original
    // continuous path. No public option or C ABI surface is needed.
    pub(crate) fn with_limits(bank: Arc<Filters>, corners: usize, queries: usize) -> Self {
        let width = bank.output_len();
        let corners = Spectra::new(
            width,
            corners.min(CORNER_LIMIT),
            CORNER_BYTES - size_of::<Arc<Filters>>() - CORNER_INDEX_BYTES,
        );
        let queries = Spectra::new(
            width,
            queries.min(QUERY_LIMIT),
            QUERY_BYTES - QueryIndex::storage_for(QUERY_LIMIT),
        );
        let corner_index = if corners.entries.is_empty() {
            Box::default()
        } else {
            vec![EMPTY_SLOT; GRID_POINTS].into_boxed_slice()
        };
        let query_index = QueryIndex::new(queries.entries.len());
        Self {
            bank,
            corners,
            queries,
            corner_index,
            query_index,
        }
    }

    pub(crate) fn clear(&mut self) {
        self.corners.clear();
        self.queries.clear();
        self.corner_index.fill(EMPTY_SLOT);
        self.query_index.clear();
    }

    pub(crate) fn query(&mut self, azimuth: f32, elevation: f32, output: &mut [f32]) -> Result<()> {
        let (az, el) = coordinates(azimuth, elevation)?;
        if output.len() != self.bank.output_len() {
            return Err(Error::InvalidArgument("Invalid HRTF query buffers"));
        }
        let key = [az.to_bits(), el.to_bits()];
        if let Some(slot) = self.query_index.find(key) {
            self.queries.touch(slot);
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
                let indexed = self.corner_index[grid];
                let slot = if indexed != EMPTY_SLOT {
                    let slot = usize::from(indexed);
                    self.corners.touch(slot);
                    slot
                } else {
                    let (slot, previous) =
                        self.corners.insert(grid).expect("nonempty corner cache");
                    if let Some(previous) = previous {
                        self.corner_index[previous] = EMPTY_SLOT;
                    }
                    self.corner_index[grid] = u16::try_from(slot).expect("bounded corner slot");
                    self.bank.grid_spectrum(grid, self.corners.get_mut(slot));
                    slot
                };
                // Retain the reference's corner order and multiply/add order,
                // including signed zero, zero weights and the phase fallback.
                for (out, h) in output.iter_mut().zip(self.corners.get(slot)) {
                    *out += weight * h;
                }
            }
        }
        if let Some((slot, previous)) = self.queries.insert(key) {
            if let Some(previous) = previous {
                self.query_index.remove(previous);
            }
            self.queries.get_mut(slot).copy_from_slice(output);
            self.query_index.insert(key, slot);
        }
        Ok(())
    }

    #[cfg(test)]
    fn storage_bytes(&self) -> usize {
        size_of::<Arc<Filters>>()
            + self.corners.storage_bytes()
            + self.queries.storage_bytes()
            + size_of::<Box<[u16]>>()
            + size_of_val(&*self.corner_index)
            + self.query_index.storage_bytes()
    }
}

#[cfg(test)]
mod tests;
