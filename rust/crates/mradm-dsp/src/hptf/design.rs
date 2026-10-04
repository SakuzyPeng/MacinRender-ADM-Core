use super::{Band, BandKind, Biquad, Coefficients, MAX_BANDS, PreampMode};
use crate::{Error, Result};
use rustfft::num_complex::Complex64;
use std::{
    cmp::Ordering,
    collections::BinaryHeap,
    f64::consts::{PI, TAU},
};

const LOW: f64 = 20.0;
const HIGH: f64 = 20000.0;
const TOLERANCE_DB: f64 = 0.001;
const REFINEMENTS: usize = 4096;

fn design_band(band: &Band, rate: f64) -> Result<Biquad> {
    let a = 10.0_f64.powf(band.gain_db / 40.0);
    let w = TAU * band.frequency / rate;
    let (cs, sn) = (w.cos(), w.sin());
    let alpha = sn / (2.0 * band.q);
    let (b0, b1, b2, a0, a1, a2) = match band.kind {
        BandKind::Peaking => (
            1.0 + alpha * a,
            -2.0 * cs,
            1.0 - alpha * a,
            1.0 + alpha / a,
            -2.0 * cs,
            1.0 - alpha / a,
        ),
        BandKind::LowShelf => {
            let t = 2.0 * a.sqrt() * alpha;
            (
                a * ((a + 1.0) - (a - 1.0) * cs + t),
                2.0 * a * ((a - 1.0) - (a + 1.0) * cs),
                a * ((a + 1.0) - (a - 1.0) * cs - t),
                (a + 1.0) + (a - 1.0) * cs + t,
                -2.0 * ((a - 1.0) + (a + 1.0) * cs),
                (a + 1.0) + (a - 1.0) * cs - t,
            )
        }
        BandKind::HighShelf => {
            let t = 2.0 * a.sqrt() * alpha;
            (
                a * ((a + 1.0) + (a - 1.0) * cs + t),
                -2.0 * a * ((a - 1.0) + (a + 1.0) * cs),
                a * ((a + 1.0) + (a - 1.0) * cs - t),
                (a + 1.0) - (a - 1.0) * cs + t,
                2.0 * ((a - 1.0) - (a + 1.0) * cs),
                (a + 1.0) - (a - 1.0) * cs - t,
            )
        }
        BandKind::LowPass => (
            (1.0 - cs) / 2.0,
            1.0 - cs,
            (1.0 - cs) / 2.0,
            1.0 + alpha,
            -2.0 * cs,
            1.0 - alpha,
        ),
        BandKind::HighPass => (
            (1.0 + cs) / 2.0,
            -(1.0 + cs),
            (1.0 + cs) / 2.0,
            1.0 + alpha,
            -2.0 * cs,
            1.0 - alpha,
        ),
        BandKind::BandPass => (alpha, 0.0, -alpha, 1.0 + alpha, -2.0 * cs, 1.0 - alpha),
        BandKind::Notch => (1.0, -2.0 * cs, 1.0, 1.0 + alpha, -2.0 * cs, 1.0 - alpha),
    };
    let values = [b0 / a0, b1 / a0, b2 / a0, a1 / a0, a2 / a0];
    if values
        .iter()
        .any(|x| !x.is_finite() || x.abs() > f32::MAX as f64)
    {
        return Err(Error::InvalidArgument("HpTF:滤波器系数超出可表示范围"));
    }
    let result = Biquad {
        b0: values[0] as f32,
        b1: values[1] as f32,
        b2: values[2] as f32,
        a1: values[3] as f32,
        a2: values[4] as f32,
    };
    result.validate()?;
    Ok(result)
}

fn response_at_z(section: &Biquad, z: Complex64, z2: Complex64) -> f64 {
    let numerator = section.b0 as f64 + (section.b1 as f64 * z) + (section.b2 as f64 * z2);
    let denominator = 1.0 + (section.a1 as f64 * z) + (section.a2 as f64 * z2);
    20.0 * (numerator.norm() / denominator.norm().max(1e-30))
        .max(1e-30)
        .log10()
}
fn section_response(section: &Biquad, hz: f64, rate: f64) -> f64 {
    let w = TAU * hz / rate;
    let z = Complex64::new((-w).cos(), (-w).sin());
    response_at_z(section, z, z * z)
}
pub(super) fn cascade_response(coefficients: &Coefficients, hz: f64) -> f64 {
    if coefficients.sample_rate == 0 {
        return 0.0;
    }
    let w = TAU * hz / coefficients.sample_rate as f64;
    let z = Complex64::new((-w).cos(), (-w).sin());
    let z2 = z * z;
    let mut db = 20.0 * (coefficients.preamp_gain as f64).log10();
    for section in &coefficients.sections[..coefficients.band_count] {
        db += response_at_z(section, z, z2);
    }
    db
}

#[derive(Clone, Copy)]
struct Extrema {
    section: Biquad,
    frequencies: [f64; 3],
}
fn extrema(section: Biquad, fc: f64, rate: f64) -> Extrema {
    // The squared response is a ratio of quadratics in sin(w/2)^2. This
    // shifted basis avoids subtracting nearly equal O(1) terms near DC.
    let polynomial = |b0: f64, b1: f64, b2: f64| {
        let sum = b0 + b1 + b2;
        let difference = b0 - b2;
        [
            sum * sum,
            4.0 * ((difference * difference) - (sum * (b0 + b2))),
            16.0 * b0 * b2,
        ]
    };
    let n = polynomial(section.b0 as f64, section.b1 as f64, section.b2 as f64);
    let d = polynomial(1.0, section.a1 as f64, section.a2 as f64);
    let a = n[2] * d[1] - n[1] * d[2];
    let b = 2.0 * (n[2] * d[0] - n[0] * d[2]);
    let c = n[1] * d[0] - n[0] * d[1];
    let mut result = Extrema {
        section,
        frequencies: [fc, -1.0, -1.0],
    };
    let mut store = |root: f64, index: usize| {
        if (0.0..=1.0).contains(&root) {
            result.frequencies[index] = root.sqrt().asin() * rate / PI;
        }
    };
    if a == 0.0 {
        if b != 0.0 {
            store(-c / b, 1);
        }
    } else {
        let discriminant = b * b - 4.0 * a * c;
        if discriminant >= 0.0 {
            let q = -0.5 * (b + discriminant.sqrt().copysign(b));
            store(q / a, 1);
            if q != 0.0 {
                store(c / q, 2);
            }
        }
    }
    result
}

#[derive(Clone, Copy)]
struct Interval {
    low: f64,
    high: f64,
    upper: f64,
}
impl PartialEq for Interval {
    fn eq(&self, other: &Self) -> bool {
        self.cmp(other) == Ordering::Equal
    }
}
impl Eq for Interval {}
impl PartialOrd for Interval {
    fn partial_cmp(&self, other: &Self) -> Option<Ordering> {
        Some(self.cmp(other))
    }
}
impl Ord for Interval {
    fn cmp(&self, other: &Self) -> Ordering {
        self.upper.total_cmp(&other.upper)
    }
}

fn peak_response(coefficients: &Coefficients, bands: &[Extrema]) -> f64 {
    let high = HIGH.min(coefficients.sample_rate as f64 * 0.49);
    let preamp_db = 20.0 * (coefficients.preamp_gain as f64).log10();
    if high <= LOW || bands.is_empty() {
        return preamp_db;
    }
    let bound = |low: f64, upper: f64| {
        let mut result = preamp_db;
        for band in bands {
            let mut peak =
                section_response(&band.section, low, coefficients.sample_rate as f64).max(
                    section_response(&band.section, upper, coefficients.sample_rate as f64),
                );
            for &hz in &band.frequencies {
                if hz >= low && hz <= upper {
                    peak = peak.max(section_response(
                        &band.section,
                        hz,
                        coefficients.sample_rate as f64,
                    ));
                }
            }
            result += peak;
        }
        result
    };
    let mut measured =
        cascade_response(coefficients, LOW).max(cascade_response(coefficients, high));
    for band in bands {
        for &hz in &band.frequencies {
            if hz >= LOW && hz <= high {
                measured = measured.max(cascade_response(coefficients, hz));
            }
        }
    }
    let mut pending = BinaryHeap::new();
    pending.push(Interval {
        low: LOW,
        high,
        upper: bound(LOW, high),
    });
    for _ in 0..REFINEMENTS {
        if pending.peek().unwrap().upper <= measured + TOLERANCE_DB {
            break;
        }
        let interval = pending.pop().unwrap();
        let middle = (interval.low * interval.high).sqrt();
        measured = measured.max(cascade_response(coefficients, middle));
        pending.push(Interval {
            low: interval.low,
            high: middle,
            upper: bound(interval.low, middle),
        });
        pending.push(Interval {
            low: middle,
            high: interval.high,
            upper: bound(middle, interval.high),
        });
    }
    let result = measured.max(pending.peek().unwrap().upper);
    if result > 0.0 {
        result + TOLERANCE_DB
    } else {
        result
    }
}

pub fn design(
    bands: &[Band],
    preamp_db: f64,
    sample_rate: u32,
    mode: PreampMode,
) -> Result<Coefficients> {
    if sample_rate == 0 || !preamp_db.is_finite() {
        return Err(Error::InvalidArgument(
            "HpTF:采样率必须为正,前级增益必须是有限数",
        ));
    }
    let mut enabled = 0;
    for band in bands {
        if !band.frequency.is_finite()
            || band.frequency <= 0.0
            || !band.q.is_finite()
            || band.q <= 0.0
            || !band.gain_db.is_finite()
        {
            return Err(Error::InvalidArgument("HpTF:参数必须为有限数且 Fc/Q 为正"));
        }
        enabled += usize::from(band.enabled);
        if enabled > MAX_BANDS {
            return Err(Error::InvalidArgument("HpTF:启用的滤波器段数超过上限 32"));
        }
    }
    let gain = 10.0_f64.powf(preamp_db / 20.0);
    if !gain.is_finite() || gain > f32::MAX as f64 || gain < (f32::MIN_POSITIVE as f64) {
        return Err(Error::InvalidArgument("HpTF:Preamp 超出可表示范围"));
    }
    let mut out = Coefficients {
        sample_rate,
        preamp_gain: gain as f32,
        preamp_db: preamp_db as f32,
        ..Coefficients::default()
    };
    let mut peaks = Vec::with_capacity(enabled);
    for band in bands {
        if !band.enabled || band.frequency >= sample_rate as f64 * 0.5 {
            continue;
        }
        let section = design_band(band, sample_rate as f64)?;
        out.sections[out.band_count] = section;
        out.band_count += 1;
        peaks.push(extrema(section, band.frequency, sample_rate as f64));
    }
    let mut peak = peak_response(&out, &peaks);
    if !peak.is_finite() || peak > 20.0 * (f32::MAX as f64).log10() {
        return Err(Error::InvalidArgument("HpTF:级联响应超出可表示范围"));
    }
    if matches!(mode, PreampMode::AutoTrim) && peak > 0.0 {
        let trimmed = out.preamp_gain as f64 * 10.0_f64.powf(-peak / 20.0);
        if !trimmed.is_finite() || trimmed < (f32::MIN_POSITIVE as f64) {
            return Err(Error::InvalidArgument(
                "HpTF:自动衰减后的增益超出可表示范围",
            ));
        }
        let gain = (trimmed as f32).next_down();
        let adjustment = 20.0 * (gain as f64 / out.preamp_gain as f64).log10();
        out.auto_trim_db = adjustment as f32;
        out.preamp_gain = gain;
        peak += adjustment;
    }
    out.max_response_db = peak as f32;
    Ok(out)
}
