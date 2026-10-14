//! Delay-aligned HRIR interpolation on the original spherical triangles.
//! The phase table linearly interpolates 1024 fractional intervals; it does not
//! quantize directions or bulk delays. Full linear-convolution support is kept.
use super::{resources::HrirData, spectral::PlanarBank};
use crate::{
    Complex32, Error, Result,
    fft::{FftPolicy, RealFft},
    geometry::{Vec3, dot, inverse_columns, norm, scale},
};
use std::sync::Arc;

pub const PHASES: usize = 1024;
pub const FRACTIONAL_TAPS: usize = 81;

pub struct FractionalBank {
    pub fft_length: usize,
    spectra: Vec<Complex32>,
    planar: Option<PlanarBank>,
}

impl FractionalBank {
    pub(crate) fn fingerprint(&self) -> String {
        super::resources::hash_complex(&self.spectra)
    }
    pub fn normalized(fft_length: usize) -> Result<Self> {
        Self::normalized_with_policy(fft_length, FftPolicy::Exact)
    }
    pub fn normalized_with_policy(fft_length: usize, policy: FftPolicy) -> Result<Self> {
        let mut impulses = vec![0.; (PHASES + 1) * FRACTIONAL_TAPS];
        for phase in 0..=PHASES {
            let fraction = phase as f64 / PHASES as f64;
            let mut taps = [0.; FRACTIONAL_TAPS];
            for (i, value) in taps.iter_mut().enumerate() {
                let x = i as f64 - 40. - fraction;
                let sinc = if x.abs() < 1e-14 {
                    1.
                } else {
                    libm::sin(std::f64::consts::PI * x) / (std::f64::consts::PI * x)
                };
                let window = 0.5 - 0.5 * libm::cos(2. * std::f64::consts::PI * i as f64 / 80.);
                *value = sinc * window;
            }
            let sum = taps.iter().sum::<f64>();
            for (i, value) in taps.into_iter().enumerate() {
                impulses[phase * FRACTIONAL_TAPS + i] = (value / sum) as f32;
            }
        }
        let mut bank = Self::from_impulses_with_policy(fft_length, &impulses, policy)?;
        bank.planar = Some(PlanarBank::new(&bank.spectra, fft_length / 2 + 1));
        Ok(bank)
    }
    pub fn from_impulses(fft_length: usize, impulses: &[f32]) -> Result<Self> {
        Self::from_impulses_with_policy(fft_length, impulses, FftPolicy::Exact)
    }
    pub fn from_impulses_with_policy(
        fft_length: usize,
        impulses: &[f32],
        policy: FftPolicy,
    ) -> Result<Self> {
        if impulses.len() != (PHASES + 1) * FRACTIONAL_TAPS || fft_length < FRACTIONAL_TAPS {
            return Err(Error::InvalidArgument(
                "Invalid room fractional-delay table",
            ));
        }
        let mut fft = RealFft::with_policy(fft_length, policy)?;
        let bins = fft.bins();
        let mut time = vec![0.; fft_length];
        let mut spectra = vec![Complex32::default(); (PHASES + 1) * bins];
        for phase in 0..=PHASES {
            time[..FRACTIONAL_TAPS]
                .copy_from_slice(&impulses[phase * FRACTIONAL_TAPS..(phase + 1) * FRACTIONAL_TAPS]);
            fft.forward(&time, &mut spectra[phase * bins..(phase + 1) * bins])?;
        }
        Ok(Self {
            fft_length,
            spectra,
            planar: None,
        })
    }
    pub fn rows(&self, fraction: f64) -> (&[Complex32], &[Complex32], f32) {
        let position = fraction.clamp(0., 1.) * PHASES as f64;
        let low = (position as usize).min(PHASES - 1);
        let alpha = (position - low as f64) as f32;
        let bins = self.fft_length / 2 + 1;
        (
            &self.spectra[low * bins..(low + 1) * bins],
            &self.spectra[(low + 1) * bins..(low + 2) * bins],
            alpha,
        )
    }
}

struct Triangle {
    vertices: [usize; 3],
    inverse: [Vec3; 3],
    center: Vec3,
    low: Vec3,
    high: Vec3,
}

struct Node {
    low: Vec3,
    high: Vec3,
    begin: usize,
    end: usize,
    children: Option<[usize; 2]>,
}

pub struct HrirBank {
    pub policy: FftPolicy,
    pub data: Arc<HrirData>,
    pub fft_length: usize,
    fractions: PlanarBank,
    spectra: PlanarBank,
    radial_spectra: Vec<Option<PlanarBank>>,
    triangles: Vec<Triangle>,
    integer_phases: Vec<Complex32>,
    hrir_integer_min: i64,
    hrir_rotations: PlanarBank,
    order: Vec<usize>,
    nodes: Vec<Node>,
    pub short: Option<Arc<HrirBank>>,
}

/// Prepared per-source workspace used only during a two-room transition. An
/// exact direction/FFT key reuses the same HRIR evaluation across room models;
/// a miss computes the full response. There is no angular quantization.
pub struct QueryCache {
    identity: String,
    policy: FftPolicy,
    fft_length: usize,
    stride: usize,
    keys: Vec<Option<[u64; 4]>>,
    values: Vec<f32>,
    next: usize,
}
impl QueryCache {
    pub fn new(bank: &HrirBank, count: usize) -> Self {
        let count = count.min(32);
        let mut result = Self {
            identity: bank.data.identity.clone(),
            policy: bank.policy,
            fft_length: bank.fft_length,
            stride: bank.stride() * 4,
            keys: vec![None; count],
            values: vec![0.; count * bank.stride() * 4],
            next: 0,
        };
        super::touch_pages(&mut result.values);
        result
    }
    pub fn query(
        &mut self,
        bank: &HrirBank,
        _index: usize,
        direction: Vec3,
        distance: Option<f64>,
        face: &mut usize,
        out: &mut [f32],
    ) -> Result<()> {
        if self.identity != bank.data.identity
            || self.policy != bank.policy
            || self.fft_length != bank.fft_length
            || self.keys.is_empty()
        {
            return bank.query_distance(direction, distance, face, out);
        }
        let key = [
            direction[0].to_bits(),
            direction[1].to_bits(),
            direction[2].to_bits(),
            distance.map_or(u64::MAX, f64::to_bits),
        ];
        if let Some(index) = self.keys.iter().position(|stored| *stored == Some(key)) {
            out.copy_from_slice(&self.values[index * self.stride..(index + 1) * self.stride]);
            return Ok(());
        }
        bank.query_distance(direction, distance, face, out)?;
        let index = self.next;
        self.values[index * self.stride..(index + 1) * self.stride].copy_from_slice(out);
        self.keys[index] = Some(key);
        self.next = (index + 1) % self.keys.len();
        Ok(())
    }
}

pub struct TimeQuery {
    bank: Arc<HrirBank>,
    fft: RealFft,
    spectrum: Vec<Complex32>,
    planar: Vec<f32>,
    time: Vec<f32>,
}
impl TimeQuery {
    pub fn new(bank: Arc<HrirBank>) -> Result<Self> {
        Ok(Self {
            fft: RealFft::with_policy(bank.fft_length, bank.policy)?,
            spectrum: vec![Complex32::default(); bank.fft_length + 2],
            planar: vec![0.; bank.spectra.stride() * 4],
            time: vec![0.; bank.fft_length],
            bank,
        })
    }
    pub fn query(&mut self, direction: Vec3, face: &mut usize, output: &mut [f32]) -> Result<()> {
        self.query_distance(direction, None, face, output)
    }
    pub fn query_distance(
        &mut self,
        direction: Vec3,
        distance: Option<f64>,
        face: &mut usize,
        output: &mut [f32],
    ) -> Result<()> {
        let taps = self.bank.data.output_taps;
        let bins = self.bank.fft_length / 2 + 1;
        if output.len() != 2 * taps {
            return Err(Error::InvalidArgument("Invalid HRIR output"));
        }
        self.bank
            .query_distance(direction, distance, face, &mut self.planar)?;
        let stride = self.bank.spectra.stride();
        for ear in 0..2 {
            for bin in 0..bins {
                self.spectrum[ear * bins + bin] = Complex32::new(
                    self.planar[ear * stride * 2 + bin],
                    self.planar[ear * stride * 2 + stride + bin],
                );
            }
        }
        for ear in 0..2 {
            self.fft
                .inverse(&self.spectrum[ear * bins..(ear + 1) * bins], &mut self.time)?;
            output[ear * taps..(ear + 1) * taps].copy_from_slice(&self.time[..taps]);
        }
        Ok(())
    }
}

impl HrirBank {
    pub fn fingerprint(&self) -> serde_json::Value {
        use super::resources::{hash_complex, hash_f64};
        let mut value = serde_json::json!({"fft_length":self.fft_length,"spectra":self.spectra.fingerprint(),
            "fractional":self.fractions.fingerprint(),"phase_rows":self.hrir_rotations.fingerprint(),
            "integer_phases":hash_complex(&self.integer_phases),
            "mesh_inverse":hash_f64(self.triangles.iter().flat_map(|t|t.inverse.into_iter().flatten())),
            "bvh":hash_f64(self.nodes.iter().flat_map(|n|n.low.into_iter().chain(n.high)))});
        if let Some(collection) = &self.data.collection {
            value["distance_layers"] = serde_json::json!({"identity":self.data.identity,
                "radii":collection.layers.iter().map(|l|l.radius).collect::<Vec<_>>(),
                "spectra":self.radial_spectra.iter().map(|s|s.as_ref().unwrap_or(&self.spectra).fingerprint()).collect::<Vec<_>>()});
        }
        value
    }
    pub fn new(data: Arc<HrirData>, fft_length: usize) -> Result<Self> {
        Self::with_policy(data, fft_length, FftPolicy::Exact)
    }
    pub fn with_policy(data: Arc<HrirData>, fft_length: usize, policy: FftPolicy) -> Result<Self> {
        if fft_length < data.output_taps || fft_length > 32768 {
            return Err(Error::InvalidArgument("Invalid room HRIR FFT support"));
        }
        if let Some(collection) = &data.collection {
            let bins = fft_length / 2 + 1;
            let stride = bins.next_multiple_of(16);
            let count = data.directions.len();
            let raw: usize = (0..collection.layers.len())
                .map(|i| {
                    let layer = data.layer(i);
                    layer.impulses.len() * 4
                        + layer.delays.len() * 16
                        + layer.directions.len() * 24
                        + layer.faces.len() * 12
                })
                .sum();
            let (low, high) = data.delay_bounds();
            let rotations = 2 * (libm::ceil(high - low) as usize + 1) + 1;
            // All layers are resident; include the largest transient complex
            // spectrum, shared phase tables and geometry in the preparation cap.
            let budget = raw as u128
                + (collection.layers.len() as u128 * count as u128 * 2 * stride as u128 * 8)
                + (count as u128 * 2 * bins as u128 * 8)
                + ((PHASES + 1 + rotations) as u128 * stride as u128 * 16)
                + data.faces.len() as u128 * 256
                + fft_length as u128 * 32;
            if budget > super::collection::MAX_BYTES as u128 {
                return Err(Error::Unsupported("距离 HRTF 准备资源超过 1 GiB 预算"));
            }
        }
        if data.faces.len() != 2 * data.directions.len() - 4 {
            return Err(Error::InvalidArgument(
                "HRIR mesh is not a closed spherical triangulation",
            ));
        }
        let mut faces = std::collections::BTreeSet::new();
        let mut edges = std::collections::BTreeMap::<(u32, u32), (usize, i32)>::new();
        let mut used = vec![false; data.directions.len()];
        for face in &data.faces {
            let mut key = *face;
            key.sort_unstable();
            if key[0] == key[1] || key[1] == key[2] || !faces.insert(key) {
                return Err(Error::InvalidArgument("Repeated/degenerate HRIR face"));
            }
            let p = face.map(|i| data.directions[i as usize]);
            let sign = if dot(p[0], crate::geometry::cross(p[1], p[2])) > 0. {
                1
            } else {
                -1
            };
            for (a, b) in [(face[0], face[1]), (face[1], face[2]), (face[2], face[0])] {
                used[a as usize] = true;
                let edge = edges.entry((a.min(b), a.max(b))).or_default();
                edge.0 += 1;
                edge.1 += if a < b { sign } else { -sign };
            }
        }
        if used.iter().any(|v| !*v) || edges.values().any(|v| *v != (2, 0)) {
            return Err(Error::InvalidArgument(
                "HRIR mesh has a hole or inconsistent spherical faces",
            ));
        }
        // Queries consume only the planar rows. Drop the duplicate complex table
        // after preparation; PlanarBank fingerprints retain its original ordering.
        let fractions = FractionalBank::normalized_with_policy(fft_length, policy)?
            .planar
            .unwrap();
        let mut fft = RealFft::with_policy(fft_length, policy)?;
        let bins = fft.bins();
        let length = data
            .directions
            .len()
            .checked_mul(2)
            .and_then(|n| n.checked_mul(bins))
            .filter(|n| *n <= 1 << 26)
            .ok_or(Error::InvalidArgument(
                "Prepared room HRIR bank exceeds memory budget",
            ))?;
        let mut spectra = vec![Complex32::default(); length];
        let mut time = vec![0.; fft_length];
        for (index, impulse) in data.impulses.chunks_exact(data.taps).enumerate() {
            time[..data.taps].copy_from_slice(impulse);
            fft.forward(&time, &mut spectra[index * bins..(index + 1) * bins])?;
        }
        let mut triangles = Vec::with_capacity(data.faces.len());
        for face in &data.faces {
            let vertices = face.map(|i| i as usize);
            let points = vertices.map(|i| data.directions[i]);
            let inverse = inverse_columns(points[0], points[1], points[2])
                .ok_or(Error::InvalidArgument("Degenerate room HRIR triangle"))?;
            let mean = std::array::from_fn(|i| points[0][i] + points[1][i] + points[2][i]);
            let center = scale(mean, 1. / norm(mean));
            let minimum = points.iter().map(|v| dot(center, *v)).fold(1., f64::min);
            if !minimum.is_finite() || minimum <= 0. {
                return Err(Error::InvalidArgument(
                    "HRIR triangle is not contained in a hemisphere",
                ));
            }
            let radius = (2. - 2. * minimum).max(0.).sqrt() + 1e-12;
            triangles.push(Triangle {
                vertices,
                inverse,
                center,
                low: center.map(|v| v - radius),
                high: center.map(|v| v + radius),
            });
        }
        if triangles.len() < 4 {
            return Err(Error::InvalidArgument("Incomplete room HRIR mesh"));
        }
        let count = triangles.len();
        let integer_phases: Vec<_> = (0..fft_length)
            .map(|index| {
                let (sin, cos) =
                    libm::sincos(-2. * std::f64::consts::PI * index as f64 / fft_length as f64);
                Complex32::new(cos as f32, sin as f32)
            })
            .collect();
        let (minimum, maximum) = data.delay_bounds();
        let extent = libm::ceil(maximum - minimum) as i64 + 1;
        let hrir_integer_min = data.guard as i64 - 40 - extent;
        let rotation_count = (2 * extent + 1) as usize;
        let rotation_length = rotation_count
            .checked_mul(bins)
            .filter(|v| *v <= 1 << 26)
            .ok_or(Error::InvalidArgument(
                "Room HRIR phase workspace exceeds budget",
            ))?;
        let mut hrir_rotations = vec![Complex32::default(); rotation_length];
        for (index, row) in hrir_rotations.chunks_exact_mut(bins).enumerate() {
            let shift = (hrir_integer_min + index as i64).rem_euclid(fft_length as i64) as usize;
            for (band, value) in row.iter_mut().enumerate() {
                *value = integer_phases[(band * shift) & (fft_length - 1)];
            }
        }
        let short_length = data.output_taps.next_power_of_two();
        let short = if short_length < fft_length && data.collection.is_none() {
            Some(Arc::new(Self::with_policy(
                data.clone(),
                short_length,
                policy,
            )?))
        } else {
            None
        };
        let planar = PlanarBank::new(&spectra, bins);
        drop(spectra);
        let spectra = planar;
        let mut radial_spectra = Vec::new();
        if let Some(collection) = &data.collection {
            for layer in &collection.layers {
                let Some(layer) = &layer.data else {
                    radial_spectra.push(None);
                    continue;
                };
                let mut values = vec![Complex32::default(); length];
                time.fill(0.);
                for (index, impulse) in layer.impulses.chunks_exact(layer.taps).enumerate() {
                    time[..layer.taps].copy_from_slice(impulse);
                    fft.forward(&time, &mut values[index * bins..(index + 1) * bins])?;
                }
                radial_spectra.push(Some(PlanarBank::new(&values, bins)));
            }
        }
        let mut result = Self {
            policy,
            data,
            fft_length,
            fractions,
            spectra,
            radial_spectra,
            triangles,
            integer_phases,
            hrir_integer_min,
            hrir_rotations: PlanarBank::new(&hrir_rotations, bins),
            order: (0..count).collect(),
            nodes: Vec::new(),
            short,
        };
        result.build(0, count);
        Ok(result)
    }

    fn build(&mut self, begin: usize, end: usize) -> usize {
        let mut low = [f64::INFINITY; 3];
        let mut high = [f64::NEG_INFINITY; 3];
        for index in &self.order[begin..end] {
            for axis in 0..3 {
                low[axis] = low[axis].min(self.triangles[*index].low[axis]);
                high[axis] = high[axis].max(self.triangles[*index].high[axis]);
            }
        }
        let index = self.nodes.len();
        self.nodes.push(Node {
            low,
            high,
            begin,
            end,
            children: None,
        });
        if end - begin > 8 {
            let mut axis = 0;
            for next in 1..3 {
                if high[next] - low[next] > high[axis] - low[axis] {
                    axis = next;
                }
            }
            self.order[begin..end].sort_unstable_by(|a, b| {
                self.triangles[*a].center[axis]
                    .total_cmp(&self.triangles[*b].center[axis])
                    .then(a.cmp(b))
            });
            let middle = begin + (end - begin) / 2;
            let left = self.build(begin, middle);
            let right = self.build(middle, end);
            self.nodes[index].children = Some([left, right]);
        }
        index
    }

    fn coefficients(&self, face: usize, direction: Vec3) -> ([f64; 3], f64) {
        let w = self.triangles[face].inverse.map(|row| dot(row, direction));
        (w, w[0].min(w[1]).min(w[2]))
    }

    pub fn weights(
        &self,
        direction: Vec3,
        previous_face: &mut usize,
    ) -> Result<([usize; 3], [f64; 3])> {
        let length = norm(direction);
        if !length.is_finite() || length < 1e-15 {
            return Err(Error::InvalidArgument("Invalid room HRIR query direction"));
        }
        let direction = scale(direction, 1. / length);
        let mut selected = *previous_face;
        let mut weights = [0.; 3];
        let mut best = f64::NEG_INFINITY;
        if selected < self.triangles.len() {
            (weights, best) = self.coefficients(selected, direction);
        }
        if best <= 1e-10 {
            // Shared edges/vertices must pick the same face after seek or cache
            // eviction. A previous face is only a hint in the strict interior.
            best = f64::NEG_INFINITY;
            selected = usize::MAX;
            let mut stack = [0usize; 128];
            let mut length = 1;
            while length != 0 {
                length -= 1;
                let node = &self.nodes[stack[length]];
                if (0..3).any(|axis| {
                    direction[axis] < node.low[axis] || direction[axis] > node.high[axis]
                }) {
                    continue;
                }
                if let Some(children) = node.children {
                    stack[length] = children[1];
                    stack[length + 1] = children[0];
                    length += 2;
                } else {
                    for face in &self.order[node.begin..node.end] {
                        let (w, score) = self.coefficients(*face, direction);
                        if score > best || (score == best && *face < selected) {
                            selected = *face;
                            weights = w;
                            best = score;
                        }
                    }
                }
            }
        }
        if best < -1e-9 || selected >= self.triangles.len() {
            return Err(Error::InvalidArgument(
                "Query is outside the room HRIR spherical mesh",
            ));
        }
        weights = weights.map(|v| v.max(0.));
        let sum = weights.iter().sum::<f64>();
        weights = weights.map(|v| v / sum);
        *previous_face = selected;
        Ok((self.triangles[selected].vertices, weights))
    }

    /// Channel-major L/R spectrum, with the declared common interpolation guard.
    pub fn query(
        &self,
        direction: Vec3,
        previous_face: &mut usize,
        output: &mut [f32],
    ) -> Result<()> {
        let _profile = super::profile::Guard::new(super::profile::Stage::HrirQuery);
        let bins = self.fft_length / 2 + 1;
        let stride = self.spectra.stride();
        if output.len() != 4 * stride {
            return Err(Error::InvalidArgument("Invalid room HRIR spectrum buffer"));
        }
        let (vertices, weights) = self.weights(direction, previous_face)?;
        output.fill(0.);
        for ear in 0..2 {
            let mut delay = 0.;
            for vertex in 0..3 {
                delay += self.data.delays[vertices[vertex]][ear] * weights[vertex];
            }
            let destination = &mut output[ear * stride * 2..(ear + 1) * stride * 2];
            for vertex in 0..3 {
                if weights[vertex] <= 0. {
                    continue;
                }
                let shift = delay - self.data.delays[vertices[vertex]][ear];
                let integer = libm::floor(shift);
                let fraction = shift - integer;
                let position = fraction.clamp(0., 1.) * PHASES as f64;
                let lo = (position as usize).min(PHASES - 1);
                let alpha = (position - lo as f64) as f32;
                let fractions = &self.fractions;
                let low = fractions.row(lo);
                let high = fractions.row(lo + 1);
                let start = self.data.guard as f64 + integer - 40.;
                let source = self.spectra.row(vertices[vertex] * 2 + ear);
                let weight = weights[vertex] as f32;
                let rotation = self
                    .hrir_rotations
                    .row((start as i64 - self.hrir_integer_min) as usize);
                super::spectral::aligned_add(
                    destination.split_at_mut(stride),
                    source,
                    low,
                    high,
                    rotation,
                    alpha,
                    weight,
                );
            }
            destination[stride] = 0.;
            destination[stride + bins - 1] = 0.;
        }
        Ok(())
    }

    /// None selects the fixed reference for reflections/late decoding. A finite
    /// distance selects up to six samples using the same angular mesh and guard.
    pub fn query_distance(
        &self,
        direction: Vec3,
        distance: Option<f64>,
        previous_face: &mut usize,
        output: &mut [f32],
    ) -> Result<()> {
        let (Some(collection), Some(radius)) = (&self.data.collection, distance) else {
            return self.query(direction, previous_face, output);
        };
        if !radius.is_finite() || radius < 0. || output.len() != 4 * self.stride() {
            return Err(Error::InvalidArgument("无效的 HRTF 距离或输出缓冲"));
        }
        let _profile = super::profile::Guard::new(super::profile::Stage::HrirQuery);
        let (vertices, weights) = self.weights(direction, previous_face)?;
        let (low, high, alpha) = collection.bracket(radius);
        let layers = [
            (low, 1. - alpha),
            (high, if low == high { 0. } else { alpha }),
        ];
        let stride = self.stride();
        let bins = self.fft_length / 2 + 1;
        output.fill(0.);
        for ear in 0..2 {
            let mut delay = 0.;
            for (index, radial) in layers {
                if radial == 0. {
                    continue;
                }
                let data = self.data.layer(index);
                for vertex in 0..3 {
                    delay += data.delays[vertices[vertex]][ear] * (weights[vertex] * radial);
                }
            }
            let destination = &mut output[ear * stride * 2..(ear + 1) * stride * 2];
            for (index, radial) in layers {
                if radial == 0. {
                    continue;
                }
                let data = self.data.layer(index);
                let spectra = self.radial_spectra[index].as_ref().unwrap_or(&self.spectra);
                for vertex in 0..3 {
                    let weight = weights[vertex] * radial;
                    if weight <= 0. {
                        continue;
                    }
                    let shift = delay - data.delays[vertices[vertex]][ear];
                    let integer = libm::floor(shift);
                    let fraction = shift - integer;
                    let position = fraction.clamp(0., 1.) * PHASES as f64;
                    let lo = (position as usize).min(PHASES - 1);
                    let alpha = (position - lo as f64) as f32;
                    let start = self.data.guard as f64 + integer - 40.;
                    super::spectral::aligned_add(
                        destination.split_at_mut(stride),
                        spectra.row(vertices[vertex] * 2 + ear),
                        self.fractions.row(lo),
                        self.fractions.row(lo + 1),
                        self.hrir_rotations
                            .row((start as i64 - self.hrir_integer_min) as usize),
                        alpha,
                        weight as f32,
                    );
                }
            }
            destination[stride] = 0.;
            destination[stride + bins - 1] = 0.;
        }
        Ok(())
    }

    pub fn stride(&self) -> usize {
        self.spectra.stride()
    }
    pub fn storage_bytes(&self) -> usize {
        self.spectra.storage_bytes()
            + self.hrir_rotations.storage_bytes()
            + self.integer_phases.len() * size_of::<Complex32>()
            + self.fractions.storage_bytes()
            + self.data.impulses.len() * size_of::<f32>()
            + self
                .radial_spectra
                .iter()
                .flatten()
                .map(PlanarBank::storage_bytes)
                .sum::<usize>()
            + self.data.collection.as_ref().map_or(0, |c| {
                c.layers
                    .iter()
                    .filter_map(|l| l.data.as_ref())
                    .map(|d| d.impulses.len() * size_of::<f32>())
                    .sum::<usize>()
            })
    }

    pub fn integer_phase(&self, shift: i64, band: usize) -> Complex32 {
        let shift = shift.rem_euclid(self.fft_length as i64) as usize;
        self.integer_phases[(band * shift) & (self.fft_length - 1)]
    }
}
