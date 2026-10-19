//! Private research entry points shared by the existing benchmark/replay.
#[cfg(windows)]
use mradm_dsp::Result;
use mradm_dsp::room::{
    gpu_experiment::{Factory, ReferenceFactory},
    session::PreparedSet,
};
use std::{path::PathBuf, sync::Arc};
#[cfg(windows)]
#[path = "gpu_native.rs"]
mod native;
pub struct Options {
    reference: bool,
    library: Option<PathBuf>,
    fixture: Option<PathBuf>,
    kernels: Option<PathBuf>,
}
impl Options {
    pub fn parse(args: &mut Vec<String>) -> std::result::Result<Self, Box<dyn std::error::Error>> {
        let reference = args.iter().any(|a| a == "--gpu-reference");
        args.retain(|a| a != "--gpu-reference");
        let mut take =
            |name: &str| -> std::result::Result<Option<PathBuf>, Box<dyn std::error::Error>> {
                if let Some(i) = args.iter().position(|a| a == name) {
                    let value = args.get(i + 1).ok_or("Missing GPU option value")?.clone();
                    args.drain(i..i + 2);
                    Ok(Some(value.into()))
                } else {
                    Ok(None)
                }
            };
        let result = Self {
            reference,
            library: take("--gpu-dll")?,
            fixture: take("--gpu-fixture")?,
            kernels: take("--gpu-kernels")?,
        };
        if result.library.is_some()
            && (reference || result.fixture.is_none() || result.kernels.is_none())
        {
            return Err(
                "GPU DLL needs fixture/kernels and cannot combine with CPU reference".into(),
            );
        }
        Ok(result)
    }
    pub fn prepare(
        &self,
        prepared: Arc<PreparedSet>,
    ) -> std::result::Result<Arc<PreparedSet>, Box<dyn std::error::Error>> {
        let factory: Option<Arc<dyn Factory>> = if self.reference {
            Some(Arc::new(ReferenceFactory))
        } else if let Some(library) = &self.library {
            #[cfg(windows)]
            {
                Some(Arc::new(native::NativeFactory::new(
                    library,
                    self.fixture.as_ref().unwrap(),
                    self.kernels.as_ref().unwrap(),
                )?))
            }
            #[cfg(not(windows))]
            {
                let _ = (library, &self.fixture, &self.kernels);
                return Err("Native GPU experiment requires Windows CUDA".into());
            }
        } else {
            None
        };
        if let Some(factory) = factory {
            Ok(Arc::new(prepared.with_gpu_experiment(factory)?))
        } else {
            Ok(prepared)
        }
    }
    pub fn report(&self) -> serde_json::Value {
        if self.reference || self.library.is_some() {
            serde_json::json!({"kind":"room-gpu-session","cpu_reference":self.reference,"control":"production Session","finite_fade":true})
        } else {
            serde_json::Value::Null
        }
    }
}
#[cfg(windows)]
fn tables(room: &mradm_dsp::room::prepared::PreparedRoom) -> Result<(Vec<f32>, Vec<f32>)> {
    use mradm_dsp::{
        Complex32 as C,
        fft::RealFft,
        room::hrir::{FractionalBank, PHASES},
    };
    let n = room.fft_length;
    let bins = n / 2 + 1;
    let mut fft = RealFft::with_policy(n, room.quality.fft())?;
    let mut time = vec![0.; n];
    let mut spectrum = vec![C::default(); bins];
    let mut materials = Vec::with_capacity(room.images.len() * bins * 2);
    for image in &room.images {
        let mut gain = [1f32; 8];
        for (wall, count) in image.walls.iter().enumerate() {
            for (band, gain) in gain.iter_mut().enumerate() {
                let r = (1. - room.profile.absorption[wall][band]).sqrt() as f32;
                for _ in 0..*count {
                    *gain *= r;
                }
            }
        }
        time.fill(0.);
        for (t, out) in time[..room.profile.band_taps].iter_mut().enumerate() {
            let mut sum = 0.;
            for (b, g) in gain.iter().enumerate() {
                sum += room.profile.bands[b * room.profile.band_taps + t] * f64::from(*g);
            }
            *out = sum as f32;
        }
        fft.forward(&time, &mut spectrum)?;
        for v in &spectrum {
            materials.extend([v.re, v.im]);
        }
    }
    let table = FractionalBank::from_impulses_with_policy(
        n,
        &room.profile.propagation_phases,
        room.quality.fft(),
    )?;
    let mut propagation = Vec::with_capacity((PHASES + 1) * bins * 2);
    for phase in 0..=PHASES {
        let (a, b, _) = table.rows(phase as f64 / PHASES as f64);
        for v in if phase == PHASES { b } else { a } {
            propagation.extend([v.re, v.im]);
        }
    }
    Ok((materials, propagation))
}
