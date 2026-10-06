//! Project-owned spatial DSP. No ADM model, C ABI, device I/O or native BLAS.
#![forbid(unsafe_code)]

pub mod convolution;
pub mod data;
pub mod dataset;
pub mod decorrelator;
pub mod diffuse;
pub mod ear_post;
pub mod fft;
pub mod filterbank;
pub mod gain;
pub mod geometry;
pub mod hoa;
pub mod hptf;
pub mod hrtf;
pub mod hrtf_filters;
pub mod live_vbap;
pub mod meter;
pub mod mixing;
pub mod monitor;
pub mod pcm_mix;
pub mod peak_guard;
pub mod resampler;
pub mod rng;
pub mod scene_math;
pub mod scene_transition;
pub mod spreader;
pub mod triple_balance;
pub mod vbap;

pub use rustfft::num_complex::Complex32;

#[derive(Debug)]
pub enum Error {
    InvalidArgument(&'static str),
    Unsupported(&'static str),
    RenderFailed(&'static str),
    Io(&'static str),
}

impl std::fmt::Display for Error {
    fn fmt(&self, formatter: &mut std::fmt::Formatter<'_>) -> std::fmt::Result {
        match self {
            Self::InvalidArgument(message)
            | Self::Unsupported(message)
            | Self::RenderFailed(message)
            | Self::Io(message) => formatter.write_str(message),
        }
    }
}

impl std::error::Error for Error {}
pub type Result<T> = std::result::Result<T, Error>;
