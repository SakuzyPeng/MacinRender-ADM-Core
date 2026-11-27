//! Electrical trims at the inputs of a measured virtual speaker array.
use super::resources::{Array, integer, number};
use crate::{Error, Result};
use serde_json::Value;

#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub enum SpeakerLayout {
    SevenOneFour,
    NineOneSix,
    TwentyTwoTwo,
}

impl SpeakerLayout {
    pub const ALL: [Self; 3] = [Self::SevenOneFour, Self::NineOneSix, Self::TwentyTwoTwo];
    pub fn id(self) -> &'static str {
        match self {
            Self::SevenOneFour => "7.1.4",
            Self::NineOneSix => "9.1.6",
            Self::TwentyTwoTwo => "22.2",
        }
    }
    pub fn channels(self) -> usize {
        match self {
            Self::SevenOneFour => 12,
            Self::NineOneSix => 16,
            Self::TwentyTwoTwo => 24,
        }
    }
}

#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub struct Feed {
    pub layout: SpeakerLayout,
    /// Zero-based channel in mradm's WAVE order, independent of source enumeration.
    pub channel: u8,
}

#[derive(Clone, Copy, Debug)]
pub struct Speaker {
    pub feed: Feed,
    pub nominal: [f32; 3],
    /// Producer or label-derived nominal position routed to the measured speaker.
    pub source_nominal: Option<[f32; 3]>,
}

pub fn same_position(a: [f32; 3], b: [f32; 3]) -> bool {
    a.into_iter().zip(b).all(|(x, y)| (x - y).abs() <= 1e-4)
}

impl Speaker {
    pub fn position(self, authored: [f32; 3]) -> [f32; 3] {
        if self
            .source_nominal
            .is_some_and(|p| same_position(p, authored))
        {
            self.nominal
        } else {
            authored
        }
    }
}

#[derive(Clone, Copy)]
pub struct Trim {
    pub delay: usize,
    pub gain: f64,
}

#[derive(Clone)]
pub struct Calibration {
    layouts: [Vec<Trim>; 3],
    pub lfe: Vec<f32>,
    pub max_delay: usize,
    tails: [usize; 3],
}

impl Calibration {
    pub fn parse(
        value: &Value,
        rate: u32,
        read: &mut impl FnMut(&str, usize) -> Result<Vec<u8>>,
    ) -> Result<Self> {
        let invalid = || Error::InvalidArgument("Invalid room speaker calibration");
        if value["version"] != 1 {
            return Err(invalid());
        }
        let sets = value["layouts"].as_object().ok_or_else(invalid)?;
        if sets.is_empty()
            || sets
                .keys()
                .any(|id| !SpeakerLayout::ALL.iter().any(|l| l.id() == id))
        {
            return Err(invalid());
        }
        let mut layouts: [Vec<Trim>; 3] = Default::default();
        let mut max_delay = 0;
        for layout in SpeakerLayout::ALL {
            let Some(entries) = sets.get(layout.id()) else {
                continue;
            };
            let entries = entries
                .as_array()
                .filter(|v| v.len() == layout.channels())
                .ok_or_else(invalid)?;
            for row in entries {
                let delay = integer(&row["delay_frames"], rate as usize)?;
                let gain = libm::pow(10., number(&row["gain_db"])? / 20.);
                if !(gain as f32).is_finite() || gain <= 0. {
                    return Err(invalid());
                }
                max_delay = max_delay.max(delay);
                layouts[layout as usize].push(Trim { delay, gain });
            }
        }
        let shape = value["lfe_fir"]["shape"]
            .as_array()
            .filter(|s| s.len() == 1)
            .ok_or_else(invalid)?;
        integer(&shape[0], 2 * rate as usize)?;
        let (_, impulse) = Array::f64(&value["lfe_fir"], read)?;
        if impulse.iter().any(|v| !(*v as f32).is_finite()) {
            return Err(invalid());
        }
        let tails = SpeakerLayout::ALL.map(|layout| {
            layouts[layout as usize]
                .iter()
                .enumerate()
                .map(|(i, t)| {
                    let lfe = i == 3 || (layout == SpeakerLayout::TwentyTwoTwo && i == 9);
                    t.delay + if lfe { impulse.len() - 1 } else { 0 }
                })
                .max()
                .unwrap_or(0)
        });
        Ok(Self {
            layouts,
            lfe: impulse.into_iter().map(|v| v as f32).collect(),
            max_delay,
            tails,
        })
    }
    pub fn trim(&self, feed: Feed) -> Option<Trim> {
        self.layouts[feed.layout as usize]
            .get(usize::from(feed.channel))
            .copied()
    }
    pub fn layouts(&self) -> impl Iterator<Item = SpeakerLayout> + '_ {
        SpeakerLayout::ALL
            .into_iter()
            .filter(|l| !self.layouts[*l as usize].is_empty())
    }
    pub fn tail_for_layouts(&self, mask: u8) -> usize {
        SpeakerLayout::ALL
            .into_iter()
            .filter(|l| mask & (1 << *l as usize) != 0)
            .map(|l| self.tails[l as usize])
            .max()
            .unwrap_or(0)
    }
}

/// Schedule at the time of emission. A change of assignment never retimes
/// samples already in flight. The ring is allocated in instance preparation.
pub(super) struct FeedDelay {
    pending: Vec<f32>,
    cursor: usize,
    remaining: usize,
}
impl FeedDelay {
    pub fn new(maximum: usize) -> Self {
        Self {
            pending: vec![0.; maximum],
            cursor: 0,
            remaining: 0,
        }
    }
    pub fn reset(&mut self) {
        self.pending.fill(0.);
        self.cursor = 0;
        self.remaining = 0;
    }
    pub fn is_empty(&self) -> bool {
        self.remaining == 0
    }
    pub fn next(&mut self, sample: f32, trim: Option<Trim>) -> f32 {
        let scaled = trim.map_or(0., |t| (f64::from(sample) * t.gain) as f32);
        let delay = trim.map_or(0, |t| t.delay);
        if self.pending.is_empty() {
            return scaled;
        }
        self.remaining = self.remaining.saturating_sub(1);
        if scaled != 0. {
            self.remaining = self.remaining.max(delay);
        }
        let due = std::mem::take(&mut self.pending[self.cursor]);
        let out = if delay == 0 {
            due + scaled
        } else {
            let mut index = self.cursor + delay;
            if index >= self.pending.len() {
                index -= self.pending.len();
            }
            self.pending[index] += scaled;
            due
        };
        self.cursor += 1;
        if self.cursor == self.pending.len() {
            self.cursor = 0;
        }
        out
    }
}

#[cfg(test)]
mod tests {
    use super::*;
    #[test]
    fn reassignment_drains_samples_at_their_original_delays() {
        let mut delay = FeedDelay::new(3);
        let trim = |delay, gain| Some(Trim { delay, gain });
        assert_eq!(delay.next(1., trim(3, 2.)), 0.);
        assert_eq!(delay.next(4., trim(2, 0.5)), 0.);
        assert_eq!(delay.next(9., None), 0.);
        assert_eq!(delay.next(8., trim(0, 0.25)), 6.);
        assert!(delay.is_empty());
        delay.next(1., trim(3, 1.));
        delay.reset();
        assert!((0..5).all(|_| delay.next(0., None) == 0.));
    }
    #[test]
    fn rejects_bad_tables_and_firs_before_loading_audio() {
        let base: Value = serde_json::from_str(include_str!(
            "../../../../../tests/fixtures/room/synthetic/speaker-room.json"
        ))
        .unwrap();
        for (key, value) in [
            ("version", serde_json::json!(2)),
            ("layouts", serde_json::json!({"5.1":[]})),
            ("layouts", serde_json::json!({"7.1.4":[]})),
            ("lfe_fir", serde_json::json!({"shape":[96001]})),
        ] {
            let mut v = base["speaker_calibration"].clone();
            v[key] = value;
            assert!(Calibration::parse(&v, 48000, &mut |_, _| panic!("unexpected read")).is_err());
        }
    }
}
