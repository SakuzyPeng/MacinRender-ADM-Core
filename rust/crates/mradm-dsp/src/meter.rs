//! Project-owned loudness/peak boundary. Full-history integration stays on the
//! worker: it may grow after long playback. No histogram quantization or history
//! truncation (ebur128 0.1.10's set_max_history is defective) is exposed here.
use crate::{Error, Result};
use ebur128::{Channel, EbuR128, Mode};

#[derive(Clone, Copy, Debug)]
#[repr(u32)]
pub enum MeterMode {
    Integrated = 0,
    TruePeak = 1,
    IntegratedTruePeak = 2,
    Monitor = 3,
}

impl TryFrom<u32> for MeterMode {
    type Error = Error;
    fn try_from(value: u32) -> Result<Self> {
        match value {
            0 => Ok(Self::Integrated),
            1 => Ok(Self::TruePeak),
            2 => Ok(Self::IntegratedTruePeak),
            3 => Ok(Self::Monitor),
            _ => Err(Error::InvalidArgument("Invalid meter mode")),
        }
    }
}

#[derive(Clone, Copy, Debug)]
#[repr(u32)]
pub enum MeterChannel {
    Unused = 0,
    Left = 1,
    Right = 2,
    Center = 3,
    LeftSurround = 4,
    RightSurround = 5,
    SideLeft = 6,
    SideRight = 7,
    RearLeft = 8,
    RearRight = 9,
    TopFrontLeft = 10,
    TopFrontRight = 11,
    TopRearLeft = 12,
    TopRearRight = 13,
}

impl TryFrom<u32> for MeterChannel {
    type Error = Error;
    fn try_from(value: u32) -> Result<Self> {
        match value {
            0 => Ok(Self::Unused),
            1 => Ok(Self::Left),
            2 => Ok(Self::Right),
            3 => Ok(Self::Center),
            4 => Ok(Self::LeftSurround),
            5 => Ok(Self::RightSurround),
            6 => Ok(Self::SideLeft),
            7 => Ok(Self::SideRight),
            8 => Ok(Self::RearLeft),
            9 => Ok(Self::RearRight),
            10 => Ok(Self::TopFrontLeft),
            11 => Ok(Self::TopFrontRight),
            12 => Ok(Self::TopRearLeft),
            13 => Ok(Self::TopRearRight),
            _ => Err(Error::InvalidArgument("Invalid meter channel position")),
        }
    }
}

impl From<MeterChannel> for Channel {
    fn from(value: MeterChannel) -> Self {
        match value {
            MeterChannel::Unused => Self::Unused,
            MeterChannel::Left => Self::Left,
            MeterChannel::Right => Self::Right,
            MeterChannel::Center => Self::Center,
            MeterChannel::LeftSurround => Self::LeftSurround,
            MeterChannel::RightSurround => Self::RightSurround,
            MeterChannel::SideLeft => Self::Mp090,
            MeterChannel::SideRight => Self::Mm090,
            MeterChannel::RearLeft => Self::Mp135,
            MeterChannel::RearRight => Self::Mm135,
            MeterChannel::TopFrontLeft => Self::Up045,
            MeterChannel::TopFrontRight => Self::Um045,
            MeterChannel::TopRearLeft => Self::Up135,
            MeterChannel::TopRearRight => Self::Um135,
        }
    }
}

fn translate(error: ebur128::Error) -> Error {
    match error {
        ebur128::Error::InvalidMode => {
            Error::InvalidArgument("Meter query is unavailable in this mode")
        }
        ebur128::Error::InvalidChannelIndex => {
            Error::InvalidArgument("Invalid meter channel index")
        }
        ebur128::Error::NoMem => Error::RenderFailed("Unable to prepare loudness meter"),
    }
}

pub struct Meter {
    inner: EbuR128,
}

impl Meter {
    pub fn new(channels: u32, rate: u32, mode: MeterMode, map: &[MeterChannel]) -> Result<Self> {
        // Validate before calling the crate: its NoMem also represents invalid
        // dimensions, which must not be reported as an allocation failure.
        if !(1..=64).contains(&channels) || !(16..=2_822_400).contains(&rate) {
            return Err(Error::InvalidArgument(
                "Invalid meter channel count or sample rate",
            ));
        }
        if !map.is_empty() && map.len() != channels as usize {
            return Err(Error::InvalidArgument("Meter channel map length mismatch"));
        }
        let mode = match mode {
            MeterMode::Integrated => Mode::I,
            MeterMode::TruePeak => Mode::TRUE_PEAK,
            MeterMode::IntegratedTruePeak => Mode::I | Mode::TRUE_PEAK,
            MeterMode::Monitor => Mode::S | Mode::I,
        };
        let mut inner = EbuR128::new(channels, rate, mode).map_err(translate)?;
        // Empty map explicitly requests the established libebur128 defaults:
        // quad and five-channel special cases, otherwise L/R/C/LFE/Ls/Rs, with
        // further channels unused. Layout policy changes belong to a separate
        // migration, not to the choice of implementation language.
        for (index, position) in map.iter().enumerate() {
            inner
                .set_channel(index as u32, (*position).into())
                .map_err(translate)?;
        }
        Ok(Self { inner })
    }

    pub fn channels(&self) -> usize {
        self.inner.channels() as usize
    }

    pub fn add_frames(&mut self, samples: &[f32]) -> Result<()> {
        if !samples.len().is_multiple_of(self.channels()) {
            return Err(Error::InvalidArgument(
                "Meter input must contain complete interleaved frames",
            ));
        }
        if samples.iter().any(|sample| !sample.is_finite()) {
            return Err(Error::InvalidArgument(
                "Meter input contains non-finite samples",
            ));
        }
        self.inner.add_frames_f32(samples).map_err(translate)
    }

    pub fn reset(&mut self) {
        self.inner.reset();
    }

    pub fn integrated(&self) -> Result<f64> {
        self.inner.loudness_global().map_err(translate)
    }

    pub fn momentary(&self) -> Result<f64> {
        self.inner.loudness_momentary().map_err(translate)
    }

    pub fn shortterm(&self) -> Result<f64> {
        self.inner.loudness_shortterm().map_err(translate)
    }

    pub fn true_peak(&self, channel: u32) -> Result<f64> {
        self.inner.true_peak(channel).map_err(translate)
    }

    pub fn max_true_peak(&self) -> Result<f64> {
        let mut peak: f64 = 0.0;
        for channel in 0..self.inner.channels() {
            peak = peak.max(self.true_peak(channel)?);
        }
        Ok(peak)
    }
}

#[cfg(test)]
mod tests {
    use super::*;

    fn tone(frames: usize) -> Vec<f32> {
        (0..frames)
            .map(|n| (0.1 * (std::f64::consts::TAU * 1000.0 * n as f64 / 48_000.0).sin()) as f32)
            .collect()
    }

    #[test]
    fn independent_level_window_and_intersample_peak() {
        let input = tone(240_000);
        let mut meter = Meter::new(1, 48_000, MeterMode::IntegratedTruePeak, &[]).unwrap();
        meter.add_frames(&input[..19_199]).unwrap();
        assert_eq!(meter.integrated().unwrap(), f64::NEG_INFINITY);
        meter.add_frames(&input[19_199..19_200]).unwrap();
        assert!(meter.integrated().unwrap().is_finite());
        meter.add_frames(&input[19_200..]).unwrap();
        assert!((meter.integrated().unwrap() + 23.0).abs() < 0.01);
        assert!((20.0 * meter.true_peak(0).unwrap().log10() + 20.0).abs() < 0.1);

        meter.reset();
        let input: Vec<f32> = (0..48_000)
            .map(|n| {
                (0.9 * (std::f64::consts::FRAC_PI_2 * f64::from(n) + std::f64::consts::FRAC_PI_4)
                    .sin()) as f32
            })
            .collect();
        meter.add_frames(&input).unwrap();
        let sample_peak = input.iter().copied().map(f32::abs).fold(0.0, f32::max);
        assert!(meter.max_true_peak().unwrap() > f64::from(sample_peak) * 1.3);
    }

    #[test]
    fn explicit_lfe_map_reset_and_chunk_boundaries() {
        let input = tone(240_000);
        let map = [MeterChannel::Unused];
        let mut lfe = Meter::new(1, 48_000, MeterMode::IntegratedTruePeak, &map).unwrap();
        lfe.add_frames(&input).unwrap();
        assert_eq!(lfe.integrated().unwrap(), f64::NEG_INFINITY);
        assert!(lfe.max_true_peak().unwrap() > 0.09);
        lfe.reset();
        assert_eq!(lfe.max_true_peak().unwrap(), 0.0);
        lfe.add_frames(&input).unwrap();
        assert_eq!(lfe.integrated().unwrap(), f64::NEG_INFINITY);

        let mut full = Meter::new(1, 48_000, MeterMode::Monitor, &[]).unwrap();
        full.add_frames(&input).unwrap();
        let expected = [
            full.integrated().unwrap(),
            full.momentary().unwrap(),
            full.shortterm().unwrap(),
        ];
        full.reset();
        assert_eq!(full.integrated().unwrap(), f64::NEG_INFINITY);
        for chunk in input.chunks(127) {
            full.add_frames(chunk).unwrap();
        }
        for (actual, expected) in [
            full.integrated().unwrap(),
            full.momentary().unwrap(),
            full.shortterm().unwrap(),
        ]
        .into_iter()
        .zip(expected)
        {
            assert!((actual - expected).abs() < 1e-10);
        }
    }

    #[test]
    fn invalid_input_is_rejected_without_poisoning_state() {
        for (channels, rate) in [(0, 48_000), (65, 48_000), (2, 0), (1, 2_822_401)] {
            assert!(matches!(
                Meter::new(channels, rate, MeterMode::Monitor, &[]),
                Err(Error::InvalidArgument(_))
            ));
        }
        assert!(Meter::new(2, 48_000, MeterMode::Monitor, &[MeterChannel::Left]).is_err());
        let mut meter = Meter::new(2, 48_000, MeterMode::Monitor, &[]).unwrap();
        assert!(meter.add_frames(&[0.0]).is_err());
        assert!(meter.add_frames(&[f32::NAN, 0.0]).is_err());
        assert!(meter.add_frames(&[f32::INFINITY, 0.0]).is_err());
        assert_eq!(meter.momentary().unwrap(), f64::NEG_INFINITY);
        assert!(meter.true_peak(0).is_err());
        meter.add_frames(&[]).unwrap();
        meter.add_frames(&[0.0; 960]).unwrap();
        assert_eq!(meter.integrated().unwrap(), f64::NEG_INFINITY);
    }
}
