//! Session-fixed room processing policy. Scheduling uses absolute sample time.
use crate::{Error, Result, fft::FftPolicy};

#[derive(Clone, Copy, Debug, Default, PartialEq, Eq, PartialOrd, Ord)]
#[repr(u32)]
pub enum Quality {
    #[default]
    Exact = 0,
    Realtime = 1,
}

impl TryFrom<u32> for Quality {
    type Error = Error;
    fn try_from(value: u32) -> Result<Self> {
        match value {
            0 => Ok(Self::Exact),
            1 => Ok(Self::Realtime),
            _ => Err(Error::InvalidArgument("Invalid room quality")),
        }
    }
}

impl std::str::FromStr for Quality {
    type Err = Error;
    fn from_str(value: &str) -> Result<Self> {
        match value {
            "exact" => Ok(Self::Exact),
            "realtime" => Ok(Self::Realtime),
            _ => Err(Error::InvalidArgument(
                "Room quality must be exact or realtime",
            )),
        }
    }
}

impl Quality {
    pub fn name(self) -> &'static str {
        match self {
            Self::Exact => "exact",
            Self::Realtime => "realtime",
        }
    }
    pub fn policy(self) -> &'static str {
        match self {
            Self::Exact => "room-exact-v1",
            Self::Realtime => "room-realtime-v2",
        }
    }
    pub fn fft(self) -> FftPolicy {
        match self {
            Self::Exact => FftPolicy::Exact,
            Self::Realtime => FftPolicy::Native,
        }
    }
    pub fn reflection_frames(self) -> usize {
        match self {
            Self::Exact => 0,
            Self::Realtime => 1024,
        }
    }
    pub(crate) fn reflection_due(self, sample: u64, id: u64) -> bool {
        Self::periodic_due(sample, id, self.reflection_frames())
    }
    /// Maximum held reflection-target pose error, before the existing filter fade.
    pub fn reflection_pose_degrees(self) -> f64 {
        match self {
            Self::Exact => 0.,
            Self::Realtime => 0.5,
        }
    }
    pub fn reflection_pose_frames(self) -> usize {
        match self {
            Self::Exact => 0,
            Self::Realtime => 1024,
        }
    }
    pub(crate) fn reflection_pose_due(self, sample: u64, id: u64) -> bool {
        Self::periodic_due(sample, id, self.reflection_pose_frames())
    }
    pub fn reflection_history_frames(self) -> usize {
        self.reflection_frames().max(self.reflection_pose_frames())
    }
    fn periodic_due(sample: u64, id: u64, period: usize) -> bool {
        let period = period as u64;
        if period == 0 {
            return true;
        }
        // Stable source identity, independent of source order, generation,
        // packetization, worker timing and which other sources are present.
        let mut hash = id.wrapping_add(0x9e37_79b9_7f4a_7c15);
        hash = (hash ^ (hash >> 30)).wrapping_mul(0xbf58_476d_1ce4_e5b9);
        hash = (hash ^ (hash >> 27)).wrapping_mul(0x94d0_49bb_1331_11eb);
        hash ^= hash >> 31;
        let phase = hash % (period / super::BLOCK as u64) * super::BLOCK as u64;
        sample % period == phase
    }
}
