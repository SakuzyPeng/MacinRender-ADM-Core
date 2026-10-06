//! Project-used EAR algorithms, derived from EBU libear (Apache-2.0).
//! Scene semantics and streaming DSP are owned by the caller. See PROVENANCE.json.
#![forbid(unsafe_code)]

mod data;
pub mod decorrelate;
#[path = "../../mradm-dsp/src/diagnostics.rs"]
pub mod diagnostics;
mod direct;
mod extent;
mod geom;
mod hoa;
mod panner;

pub use direct::DirectSpeakers;
pub use hoa::Normalization;

#[derive(Debug, Clone, Copy, PartialEq, Eq)]
pub enum Error {
    InvalidArgument(&'static str),
    Unsupported(&'static str),
    RenderFailed(&'static str),
}
impl std::fmt::Display for Error {
    fn fmt(&self, f: &mut std::fmt::Formatter<'_>) -> std::fmt::Result {
        match self {
            Self::InvalidArgument(s) | Self::Unsupported(s) | Self::RenderFailed(s) => {
                f.write_str(s)
            }
        }
    }
}
impl std::error::Error for Error {}
pub type Result<T> = std::result::Result<T, Error>;

#[derive(Clone, Debug)]
pub struct Channel {
    pub name: String,
    /// ADM azimuth/elevation in degrees, distance in metres.
    pub real: [f64; 3],
    pub nominal: [f64; 3],
    pub azimuth_range: [f64; 2],
    pub elevation_range: [f64; 2],
    pub lfe: bool,
}
#[derive(Clone, Debug)]
pub struct Layout {
    pub name: String,
    pub channels: Vec<Channel>,
}
pub fn standard_layout(name: &str) -> Result<Layout> {
    data::layout(name)
}
impl Layout {
    pub fn validate(&self) -> Result<()> {
        if self.channels.is_empty() || self.channels.len() > 256 {
            return Err(Error::InvalidArgument("Invalid EAR channel count"));
        }
        for (i, ch) in self.channels.iter().enumerate() {
            if ch.name.is_empty()
                || self.channels[..i].iter().any(|c| c.name == ch.name)
                || ch
                    .real
                    .iter()
                    .chain(&ch.nominal)
                    .chain(&ch.azimuth_range)
                    .chain(&ch.elevation_range)
                    .any(|v| !v.is_finite())
                || ch.real[2] <= 0.0
                || ch.nominal[2] <= 0.0
            {
                return Err(Error::InvalidArgument("Invalid EAR channel description"));
            }
        }
        Ok(())
    }
}

#[derive(Clone, Copy, Debug)]
pub struct Object {
    pub position: [f64; 3],
    pub width: f64,
    pub height: f64,
    pub depth: f64,
    pub gain: f64,
    pub diffuse: f64,
}
impl Default for Object {
    fn default() -> Self {
        Self {
            position: [0.0, 0.0, 1.0],
            width: 0.0,
            height: 0.0,
            depth: 0.0,
            gain: 1.0,
            diffuse: 0.0,
        }
    }
}

/// Preparation-only calculator. Lazy tables are owned by this instance.
pub struct Calculator {
    layout: Layout,
    panner: panner::Panner,
    extent: Option<extent::Extent>,
    hoa: Option<hoa::Decoder>,
}
impl Calculator {
    pub fn new(layout: Layout) -> Result<Self> {
        layout.validate()?;
        let panner = panner::Panner::new(&layout)?;
        Ok(Self {
            layout,
            panner,
            extent: None,
            hoa: None,
        })
    }
    pub fn layout(&self) -> &Layout {
        &self.layout
    }
    pub fn objects(&mut self, meta: Object) -> Result<(Vec<f64>, Vec<f64>)> {
        if meta
            .position
            .iter()
            .chain([meta.width, meta.height, meta.depth, meta.gain, meta.diffuse].iter())
            .any(|v| !v.is_finite())
            || meta.position[2] < 0.0
            || meta.width < 0.0
            || meta.height < 0.0
            || meta.depth < 0.0
            || !(0.0..=1.0).contains(&meta.diffuse)
        {
            return Err(Error::InvalidArgument("Invalid EAR object metadata"));
        }
        if self.extent.is_none() {
            self.extent = Some(extent::Extent::new(&self.panner)?);
        }
        let gains = self.extent.as_ref().unwrap().gains(
            &self.panner,
            geom::cart(meta.position),
            meta.width,
            meta.height,
            meta.depth,
        )?;
        let mut direct = vec![0.0; self.layout.channels.len()];
        let mut diffuse = direct.clone();
        let mut i = 0;
        for (j, ch) in self.layout.channels.iter().enumerate() {
            if !ch.lfe {
                let g = gains[i] * meta.gain;
                direct[j] = g * (1.0 - meta.diffuse).sqrt();
                diffuse[j] = g * meta.diffuse.sqrt();
                i += 1;
            }
        }
        Ok((direct, diffuse))
    }
    pub fn direct_speakers(&self, meta: &DirectSpeakers) -> Result<Vec<f64>> {
        direct::calculate(&self.layout, &self.panner, meta)
    }
    /// Row-major input-channel × output-channel matrix; sparse/mixed orders are retained.
    pub fn hoa(
        &mut self,
        orders: &[i32],
        degrees: &[i32],
        norm: Normalization,
    ) -> Result<Vec<f64>> {
        hoa::validate(orders, degrees, norm)?;
        if self.hoa.is_none() {
            self.hoa = Some(hoa::Decoder::new(&self.panner)?);
        }
        self.hoa
            .as_ref()
            .unwrap()
            .calculate(&self.layout, orders, degrees, norm)
    }
    pub fn filters(&self) -> Vec<f32> {
        decorrelate::filters(&self.layout)
    }
}
