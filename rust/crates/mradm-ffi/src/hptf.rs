pub mod settings;
use mradm_dsp::hptf as dsp;

#[repr(C)]
#[derive(Clone, Copy)]
pub struct Biquad {
    pub b0: f32,
    pub b1: f32,
    pub b2: f32,
    pub a1: f32,
    pub a2: f32,
}
#[repr(C)]
#[derive(Clone, Copy)]
pub struct Coefficients {
    pub sample_rate: u32,
    pub band_count: u32,
    pub preamp_gain: f32,
    pub sections: [Biquad; dsp::MAX_BANDS],
    pub max_response_db: f32,
    pub auto_trim_db: f32,
    pub preamp_db: f32,
}
#[repr(C)]
#[derive(Clone, Copy)]
pub struct Snapshot {
    pub coefficients: Coefficients,
    pub revision: u64,
    pub right: Coefficients,
    pub independent: u32,
}

impl From<dsp::Coefficients> for Coefficients {
    fn from(c: dsp::Coefficients) -> Self {
        Self {
            sample_rate: c.sample_rate,
            band_count: c.band_count as u32,
            preamp_gain: c.preamp_gain,
            sections: c.sections.map(|s| Biquad {
                b0: s.b0,
                b1: s.b1,
                b2: s.b2,
                a1: s.a1,
                a2: s.a2,
            }),
            max_response_db: c.max_response_db,
            auto_trim_db: c.auto_trim_db,
            preamp_db: c.preamp_db,
        }
    }
}
impl From<Coefficients> for dsp::Coefficients {
    fn from(c: Coefficients) -> Self {
        Self {
            sample_rate: c.sample_rate,
            band_count: c.band_count as usize,
            preamp_gain: c.preamp_gain,
            sections: c.sections.map(|s| dsp::Biquad {
                b0: s.b0,
                b1: s.b1,
                b2: s.b2,
                a1: s.a1,
                a2: s.a2,
            }),
            max_response_db: c.max_response_db,
            auto_trim_db: c.auto_trim_db,
            preamp_db: c.preamp_db,
        }
    }
}
impl From<dsp::StereoSnapshot> for Snapshot {
    fn from(s: dsp::StereoSnapshot) -> Self {
        Self {
            coefficients: s.coefficients[0].into(),
            right: s.coefficients[1].into(),
            revision: s.revision,
            independent: u32::from(s.independent),
        }
    }
}
impl From<Snapshot> for dsp::StereoSnapshot {
    fn from(s: Snapshot) -> Self {
        Self {
            coefficients: [
                s.coefficients.into(),
                if s.independent != 0 {
                    s.right
                } else {
                    s.coefficients
                }
                .into(),
            ],
            revision: s.revision,
            independent: s.independent != 0,
        }
    }
}
