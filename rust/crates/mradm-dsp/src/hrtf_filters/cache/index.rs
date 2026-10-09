//! Fixed-capacity, half-full open addressing. Backshift deletion leaves no
//! tombstones to accumulate during motion; lookups and eviction never allocate.
use super::EMPTY_SLOT;

#[derive(Clone, Copy)]
struct Bucket {
    key: [u32; 2],
    slot: u16,
}

impl Bucket {
    const EMPTY: Self = Self {
        key: [0; 2],
        slot: EMPTY_SLOT,
    };
}

pub(super) struct QueryIndex {
    buckets: Box<[Bucket]>,
}

impl QueryIndex {
    // Callers cap the spectrum slots at 512 before computing index storage.
    const fn bucket_count(capacity: usize) -> usize {
        if capacity == 0 {
            0
        } else {
            (capacity * 2).next_power_of_two()
        }
    }

    pub(super) const fn storage_for(capacity: usize) -> usize {
        size_of::<Self>() + Self::bucket_count(capacity) * size_of::<Bucket>()
    }

    pub(super) fn new(capacity: usize) -> Self {
        Self {
            buckets: vec![Bucket::EMPTY; Self::bucket_count(capacity)].into_boxed_slice(),
        }
    }

    fn hash(key: [u32; 2]) -> usize {
        let mut value = (u64::from(key[0]) << 32) | u64::from(key[1]);
        value ^= value >> 30;
        value = value.wrapping_mul(0xbf58_476d_1ce4_e5b9);
        value ^= value >> 27;
        value = value.wrapping_mul(0x94d0_49bb_1331_11eb);
        (value ^ (value >> 31)) as usize
    }

    fn bucket(&self, key: [u32; 2]) -> Option<usize> {
        if self.buckets.is_empty() {
            return None;
        }
        let mask = self.buckets.len() - 1;
        let mut at = Self::hash(key) & mask;
        for _ in 0..self.buckets.len() {
            let bucket = self.buckets[at];
            if bucket.slot == EMPTY_SLOT || bucket.key == key {
                return Some(at);
            }
            at = (at + 1) & mask;
        }
        unreachable!("query index is at most half full")
    }

    pub(super) fn find(&self, key: [u32; 2]) -> Option<usize> {
        let slot = self.buckets[self.bucket(key)?].slot;
        (slot != EMPTY_SLOT).then_some(usize::from(slot))
    }

    pub(super) fn insert(&mut self, key: [u32; 2], slot: usize) {
        let at = self.bucket(key).expect("nonempty query index");
        self.buckets[at] = Bucket {
            key,
            slot: u16::try_from(slot).expect("bounded query slot"),
        };
    }

    pub(super) fn remove(&mut self, key: [u32; 2]) {
        let mut hole = self.bucket(key).expect("nonempty query index");
        assert_ne!(self.buckets[hole].slot, EMPTY_SLOT, "indexed query victim");
        let mask = self.buckets.len() - 1;
        let mut at = (hole + 1) & mask;
        while self.buckets[at].slot != EMPTY_SLOT {
            let home = Self::hash(self.buckets[at].key) & mask;
            if (hole.wrapping_sub(home) & mask) < (at.wrapping_sub(home) & mask) {
                self.buckets[hole] = self.buckets[at];
                hole = at;
            }
            at = (at + 1) & mask;
        }
        self.buckets[hole] = Bucket::EMPTY;
    }

    pub(super) fn clear(&mut self) {
        self.buckets.fill(Bucket::EMPTY);
    }

    #[cfg(test)]
    pub(super) fn storage_bytes(&self) -> usize {
        size_of::<Self>() + size_of_val(&*self.buckets)
    }

    #[cfg(test)]
    pub(super) fn collision_keys(&self, home: usize, count: usize) -> Vec<[u32; 2]> {
        (0u32..)
            .map(|n| [n, n.wrapping_mul(137)])
            .filter(|key| Self::hash(*key) & (self.buckets.len() - 1) == home)
            .take(count)
            .collect()
    }
}
