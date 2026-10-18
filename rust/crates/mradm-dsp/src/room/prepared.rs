//! Immutable room preparation and reusable short-response construction.
use super::{
    geometry::{self, Image, Position, Rotation, WeightedPosition},
    hrir::{FRACTIONAL_TAPS, FractionalBank, HrirBank, PHASES},
    partition::LongFilters,
    quality::Quality,
    resources::{HrirData, RoomProfile},
};
use crate::{Complex32, Error, Result, fft::RealFft};
use std::sync::Arc;

pub struct PreparedRoom {
    pub quality: Quality,
    pub profile: Arc<RoomProfile>,
    pub hrir: Arc<HrirBank>,
    pub decoder_hrir: Arc<HrirBank>,
    pub images: Vec<Image>,
    pub fft_length: usize,
    pub short_taps: usize,
    pub path_bucket_stride: usize,
    pub send_taps: usize,
    pub eq: Arc<LongFilters>,
    pub late: Vec<Arc<LongFilters>>,
    material: Vec<Complex32>,
    propagation: FractionalBank,
}

impl PreparedRoom {
    /// Preparation-only stage checksums for room-scoped cross-platform replay.
    pub fn fingerprint(&self) -> serde_json::Value {
        serde_json::json!({"hrir":self.hrir.fingerprint(),"decoder_hrir":self.decoder_hrir.fingerprint(),
            "materials":super::resources::hash_complex(&self.material),
            "propagation":self.propagation.fingerprint(),"eq":self.eq.fingerprint(),
            "late":self.late.iter().map(|v|v.fingerprint()).collect::<Vec<_>>(),
            "short_taps":self.short_taps,"path_bucket_stride":self.path_bucket_stride,"send_taps":self.send_taps})
    }
    pub fn required_fft(profile: &RoomProfile, hrir: &HrirData) -> Result<(usize, usize, usize)> {
        super::validate_sample_rate(profile.sample_rate)?;
        if hrir.sample_rate != profile.sample_rate {
            return Err(Error::InvalidArgument("房间与 HRIR 处理采样率不一致"));
        }
        let farthest = std::array::from_fn::<_, 3, _>(|i| {
            (profile.listener[i] - profile.clearance)
                .max(profile.dimensions[i] - profile.clearance - profile.listener[i])
        });
        let distance = crate::geometry::norm(farthest);
        let arrival =
            (distance / profile.speed + profile.early_fade[1]) * f64::from(profile.sample_rate);
        let short_taps =
            libm::ceil(arrival) as usize + hrir.output_taps + FRACTIONAL_TAPS + profile.band_taps
                - 2
                + profile.alignment_guard;
        let send_taps = libm::ceil(
            (distance / profile.speed + profile.late_predelay) * f64::from(profile.sample_rate),
        ) as usize
            + FRACTIONAL_TAPS
            + profile.alignment_guard
            + profile.band_offset;
        // Synthesize paths in delay buckets. A bucket contains a complete HRIR,
        // propagation/material FIR and at most bucket_stride-1 local delay.
        // Absolute propagation delay is added after the inverse FFT, so its
        // size does not force every HRIR query to use a room-length transform.
        let kernel_taps = hrir.output_taps + FRACTIONAL_TAPS + profile.band_taps - 2;
        let fft_length = (kernel_taps + super::BLOCK - 1).next_power_of_two();
        if fft_length > 32768 {
            return Err(Error::Unsupported(
                "Room short-path support exceeds the v1 resource budget",
            ));
        }
        Ok((fft_length, short_taps, send_taps))
    }

    pub fn new(profile: Arc<RoomProfile>, hrir: Arc<HrirBank>) -> Result<Self> {
        Self::with_quality(profile, hrir, Quality::Exact)
    }
    pub fn with_quality(
        profile: Arc<RoomProfile>,
        hrir: Arc<HrirBank>,
        quality: Quality,
    ) -> Result<Self> {
        if hrir.policy != quality.fft() {
            return Err(Error::InvalidArgument(
                "Room and HRIR quality policies differ",
            ));
        }
        let (fft_length, short_taps, send_taps) = Self::required_fft(&profile, &hrir.data)?;
        if hrir.fft_length != fft_length {
            return Err(Error::InvalidArgument(
                "Room and HRIR prepared FFT sizes differ",
            ));
        }
        let images = geometry::images(profile.reflection_order);
        let bins = fft_length / 2 + 1;
        let mut material = vec![Complex32::default(); images.len() * bins];
        let mut fft = RealFft::with_policy(fft_length, quality.fft())?;
        let mut time = vec![0.; fft_length];
        for (index, image) in images.iter().enumerate() {
            let mut band_gain = [1f32; 8];
            for (wall, count) in image.walls.iter().enumerate() {
                for (band, gain) in band_gain.iter_mut().enumerate() {
                    let reflection = (1. - profile.absorption[wall][band]).sqrt() as f32;
                    for _ in 0..*count {
                        *gain *= reflection;
                    }
                }
            }
            time.fill(0.);
            for (sample, destination) in time[..profile.band_taps].iter_mut().enumerate() {
                let mut value = 0.;
                for (band, gain) in band_gain.iter().enumerate() {
                    value += profile.bands[band * profile.band_taps + sample] * f64::from(*gain);
                }
                *destination = value as f32;
            }
            fft.forward(&time, &mut material[index * bins..(index + 1) * bins])?;
        }
        let propagation = FractionalBank::from_impulses_with_policy(
            fft_length,
            &profile.propagation_phases,
            quality.fft(),
        )?;
        let eq = LongFilters::with_policy(1, &profile.eq, quality.fft())?;
        let late = profile
            .late
            .chunks_exact(profile.late_taps)
            .map(|direction| LongFilters::with_policy(1, direction, quality.fft()))
            .collect::<Result<Vec<_>>>()?;
        let kernel_taps = hrir.data.output_taps + FRACTIONAL_TAPS + profile.band_taps - 2;
        let path_bucket_stride = (fft_length - kernel_taps + 1) / super::BLOCK * super::BLOCK;
        Ok(Self {
            quality,
            profile,
            decoder_hrir: hrir.short.clone().unwrap_or_else(|| hrir.clone()),
            hrir,
            images,
            fft_length,
            short_taps,
            path_bucket_stride,
            send_taps,
            eq,
            late,
            material,
            propagation,
        })
    }

    pub fn history_frames(&self) -> usize {
        self.eq.taps - 1
            + (self.short_taps - 1).max(
                self.send_taps - 1 + self.profile.late_taps - 1 + self.hrir.data.output_taps - 1,
            )
    }
    /// Exact preroll covers the stored FFT windows, not just mathematically
    /// nonzero FIR taps. Large-tail partition alignment is 2048 samples.
    pub fn preroll_frames(&self) -> usize {
        let short = self.short_taps.div_ceil(super::BLOCK) * super::BLOCK;
        let send = self.send_taps.div_ceil(super::BLOCK) * super::BLOCK;
        let hrir = self.hrir.data.output_taps.div_ceil(super::BLOCK) * super::BLOCK;
        super::filter_fade(self.profile.sample_rate)
            + self
                .quality
                .reflection_history_frames_at(self.profile.sample_rate)
            + self.eq.history_frames()
            + short.max(send + self.late[0].history_frames() + hrir)
            + super::BLOCK
    }

    /// The frozen PRA propagation table is intentionally separate from the
    /// normalized Hann-sinc HRIR table. Their historical contracts differ.
    pub fn propagation_kernel(
        &self,
        fraction: f64,
        normalize: bool,
        output: &mut [f32; FRACTIONAL_TAPS],
    ) {
        let position = fraction.clamp(0., 1.) * PHASES as f64;
        let low = (position as usize).min(PHASES - 1);
        let alpha = (position - low as f64) as f32;
        let table = &self.profile.propagation_phases;
        for (i, value) in output.iter_mut().enumerate() {
            let a = table[low * FRACTIONAL_TAPS + i];
            let b = table[(low + 1) * FRACTIONAL_TAPS + i];
            *value = a + (b - a) * alpha;
        }
        if normalize {
            let sum = output.iter().sum::<f32>();
            for value in output {
                *value /= sum;
            }
        }
    }

    pub fn late_send(&self, position: Position, output: &mut [f32]) -> Result<()> {
        self.late_send_many(
            &[WeightedPosition {
                position,
                weight: 1.,
            }],
            output,
        )
    }
    pub fn late_send_many(&self, sources: &[WeightedPosition], output: &mut [f32]) -> Result<()> {
        if output.len() != self.send_taps {
            return Err(Error::InvalidArgument("Invalid late-send FIR buffer"));
        }
        output.fill(0.);
        for source in sources {
            let position = source.position;
            let time = (position.distance / self.profile.speed + self.profile.late_predelay)
                * f64::from(self.profile.sample_rate);
            let integer = libm::floor(time) as usize;
            let mut kernel = [0.; FRACTIONAL_TAPS];
            self.propagation_kernel(f64::from((time - integer as f64) as f32), true, &mut kernel);
            let gain = (self.profile.late_energy / self.profile.late_directions.len() as f64).sqrt()
                as f32;
            let start = integer + self.profile.alignment_guard + self.profile.band_offset;
            for (value, kernel) in output[start..start + FRACTIONAL_TAPS]
                .iter_mut()
                .zip(kernel)
            {
                *value += kernel * (gain * source.weight);
            }
        }
        Ok(())
    }
}

pub struct ResponseWork {
    fft: RealFft,
    query: Vec<f32>,
    direct: Vec<f32>,
    early: Vec<f32>,
    spectrum: Vec<Complex32>,
    direct_buckets: Vec<bool>,
    early_buckets: Vec<bool>,
    time: Vec<f32>,
    faces: Vec<usize>,
    pub impulse: Vec<f32>,
    pub direct_support: usize,
    pub early_support: usize,
    pub query_cache: Option<Arc<std::sync::Mutex<super::hrir::QueryCache>>>,
    decoder: super::hrir::TimeQuery,
    // Path and transfer caches per branch: a direct-only update (ADR 0020)
    // leaves the reflection entries for the next full update.
    cached_direct: Option<WeightedPosition>,
    cached_early: Option<WeightedPosition>,
    cached_paths: Vec<geometry::Path>,
    cached_transfers: Vec<f32>,
}

impl ResponseWork {
    // Only buckets marked below ever receive spectral accumulation. Clear those
    // buckets before reusing a branch; inactive branches keep their own scratch.
    fn clear_buckets(spectrum: &mut [f32], used: &mut [bool], stride: usize) {
        for (bucket, active) in used.iter_mut().enumerate() {
            if *active {
                spectrum[bucket * stride..(bucket + 1) * stride].fill(0.);
                *active = false;
            }
        }
    }

    pub fn new(room: &PreparedRoom) -> Result<Self> {
        let bins = room.fft_length / 2 + 1;
        let mut result = Self {
            fft: RealFft::with_policy(room.fft_length, room.quality.fft())?,
            query: vec![0.; room.hrir.stride() * 4],
            direct: vec![
                0.;
                room.hrir.stride() * 4 * room.short_taps.div_ceil(room.path_bucket_stride)
            ],
            early: vec![
                0.;
                room.hrir.stride() * 4 * room.short_taps.div_ceil(room.path_bucket_stride)
            ],
            direct_buckets: vec![false; room.short_taps.div_ceil(room.path_bucket_stride)],
            early_buckets: vec![false; room.short_taps.div_ceil(room.path_bucket_stride)],
            spectrum: vec![Complex32::default(); bins],
            time: vec![0.; room.fft_length],
            faces: vec![usize::MAX; room.images.len()],
            impulse: vec![0.; room.short_taps * 4],
            direct_support: 1,
            early_support: 1,
            query_cache: None,
            decoder: super::hrir::TimeQuery::new(room.decoder_hrir.clone())?,
            cached_direct: None,
            cached_early: None,
            cached_paths: vec![
                geometry::Path {
                    direction: [1., 0., 0.],
                    distance: 0.,
                    samples: 0.,
                    gain: 0.
                };
                room.images.len()
            ],
            cached_transfers: vec![0.; room.images.len() * room.hrir.stride() * 2],
        };
        super::touch_pages(&mut result.query);
        super::touch_pages(&mut result.direct);
        super::touch_pages(&mut result.early);
        super::touch_pages(&mut result.time);
        super::touch_pages(&mut result.impulse);
        super::touch_pages(&mut result.cached_transfers);
        Ok(result)
    }

    /// Write channel-major [direct L, direct R, early L, early R]. EQ is an
    /// independent source filter; including it here would make pose updates long.
    pub fn build(
        &mut self,
        room: &PreparedRoom,
        source: Position,
        rotation: &Rotation,
    ) -> Result<()> {
        self.build_many(
            room,
            &[WeightedPosition {
                position: source,
                weight: 1.,
            }],
            rotation,
        )
    }
    pub fn build_many(
        &mut self,
        room: &PreparedRoom,
        sources: &[WeightedPosition],
        rotation: &Rotation,
    ) -> Result<()> {
        self.build_branches(room, sources, rotation, true, true)
    }
    pub fn build_branches(
        &mut self,
        room: &PreparedRoom,
        sources: &[WeightedPosition],
        rotation: &Rotation,
        direct: bool,
        early: bool,
    ) -> Result<()> {
        let _profile = super::profile::Guard::new(super::profile::Stage::SourceResponse);
        let bins = room.fft_length / 2 + 1;
        let stride = room.hrir.stride();
        if direct {
            Self::clear_buckets(&mut self.direct, &mut self.direct_buckets, stride * 4);
        }
        if early {
            Self::clear_buckets(&mut self.early, &mut self.early_buckets, stride * 4);
        }
        // Preserve the public output contract: unrequested planes are zero.
        self.impulse.fill(0.);
        self.direct_support = 1;
        self.early_support = 1;
        // geometry::images sorts by order, with the sole direct path first.
        // A direct-only update therefore visits one image, not every reflection.
        let images = match (direct, early) {
            (true, true) => 0..room.images.len(),
            (true, false) => 0..1,
            (false, true) => 1..room.images.len(),
            (false, false) => 0..0,
        };
        let cache = sources.len() == 1;
        let stale = |last: Option<WeightedPosition>| {
            last.is_none_or(|last| {
                last.position.point != sources[0].position.point
                    || last.position.direction != sources[0].position.direction
                    || last.weight != sources[0].weight
            })
        };
        let fill_direct = cache && direct && stale(self.cached_direct);
        let fill_early = cache && early && stale(self.cached_early);
        if fill_direct || fill_early {
            let source = sources[0];
            for index in images.clone() {
                let image = room.images[index];
                if (image.order == 0 && !fill_direct) || (image.order != 0 && !fill_early) {
                    continue;
                }
                let path = geometry::path(&room.profile, source.position, image);
                self.cached_paths[index] = path;
                if path.gain == 0. {
                    continue;
                }
                let integer = libm::floor(path.samples);
                let fraction = f64::from((path.samples - integer) as f32);
                let (low, high, alpha) = room.propagation.rows(fraction);
                let absolute = integer as usize + room.profile.alignment_guard;
                let start = (absolute % room.path_bucket_stride) as f64;
                let gain = path.gain as f32 * source.weight;
                let material = &room.material[index * bins..(index + 1) * bins];
                for band in 0..bins {
                    let fractional = low[band] + (high[band] - low[band]) * alpha;
                    let phase = room.hrir.integer_phase(start as i64, band);
                    let value = ((fractional * phase) * material[band]) * gain;
                    self.cached_transfers[index * stride * 2 + band] = value.re;
                    self.cached_transfers[index * stride * 2 + stride + band] = value.im;
                }
            }
            if fill_direct {
                self.cached_direct = Some(source);
            }
            if fill_early {
                self.cached_early = Some(source);
            }
        }
        for source in sources {
            for index in images.clone() {
                let image = room.images[index];
                let path = if cache {
                    self.cached_paths[index]
                } else {
                    geometry::path(&room.profile, source.position, image)
                };
                if path.gain == 0. {
                    continue;
                }
                let direction = rotation.apply(path.direction);
                // Preserve the direct radius arithmetic. Reflections use their
                // complete virtual-source distance, not the last wall segment.
                let distance = if image.order == 0 {
                    Some(source.position.distance)
                } else {
                    room.hrir.data.collection.as_ref().map(|_| path.distance)
                };
                if let Some(cache) = self.query_cache.as_ref().filter(|_| sources.len() == 1) {
                    cache.lock().unwrap().query(
                        &room.hrir,
                        index,
                        direction,
                        distance,
                        &mut self.faces[index],
                        &mut self.query,
                    )?;
                } else {
                    room.hrir.query_distance(
                        direction,
                        distance,
                        &mut self.faces[index],
                        &mut self.query,
                    )?;
                }
                let bucket = (libm::floor(path.samples) as usize + room.profile.alignment_guard)
                    / room.path_bucket_stride;
                if image.order == 0 {
                    self.direct_buckets[bucket] = true;
                } else {
                    self.early_buckets[bucket] = true;
                }
                let support = libm::floor(path.samples) as usize
                    + room.profile.alignment_guard
                    + room.hrir.data.output_taps
                    + FRACTIONAL_TAPS
                    + room.profile.band_taps
                    - 2;
                if image.order == 0 {
                    self.direct_support = self.direct_support.max(support);
                } else {
                    self.early_support = self.early_support.max(support);
                }
                if cache {
                    let destination = if image.order == 0 {
                        &mut self.direct
                    } else {
                        &mut self.early
                    };
                    let transfer = self.cached_transfers
                        [index * stride * 2..(index + 1) * stride * 2]
                        .split_at(stride);
                    for ear in 0..2 {
                        super::spectral::product_add(
                            destination[(bucket * 2 + ear) * stride * 2
                                ..(bucket * 2 + ear + 1) * stride * 2]
                                .split_at_mut(stride),
                            self.query[ear * stride * 2..(ear + 1) * stride * 2].split_at(stride),
                            transfer,
                        );
                    }
                    continue;
                }
                let integer = libm::floor(path.samples);
                let fraction = f64::from((path.samples - integer) as f32);
                let (low, high, alpha) = room.propagation.rows(fraction);
                let absolute = integer as usize + room.profile.alignment_guard;
                let start = (absolute % room.path_bucket_stride) as f64;
                let gain = path.gain as f32 * source.weight;
                let material = &room.material[index * bins..(index + 1) * bins];
                let destination = if image.order == 0 {
                    &mut self.direct
                } else {
                    &mut self.early
                };
                for band in 0..bins {
                    let fractional = low[band] + (high[band] - low[band]) * alpha;
                    let rotation = room.hrir.integer_phase(start as i64, band);
                    let transfer = ((fractional * rotation) * material[band]) * gain;
                    for ear in 0..2 {
                        let base = ear * stride * 2;
                        let value = Complex32::new(
                            self.query[base + band],
                            self.query[base + stride + band],
                        ) * transfer;
                        let target = (bucket * 2 + ear) * stride * 2;
                        destination[target + band] += value.re;
                        destination[target + stride + band] += value.im;
                    }
                }
            }
        }
        for (branch, (spectrum, used)) in [
            (&self.direct, &self.direct_buckets),
            (&self.early, &self.early_buckets),
        ]
        .into_iter()
        .enumerate()
        {
            if (branch == 0 && !direct) || (branch == 1 && !early) {
                continue;
            }
            // Bucket index is the declared, platform-independent accumulation
            // order. Skip only buckets containing no participating image path.
            for (bucket, active) in used.iter().enumerate() {
                if !active {
                    continue;
                }
                let offset = bucket * room.path_bucket_stride;
                let count = room.fft_length.min(room.short_taps - offset);
                for ear in 0..2 {
                    let base = (bucket * 2 + ear) * stride * 2;
                    for (band, value) in self.spectrum.iter_mut().enumerate() {
                        *value =
                            Complex32::new(spectrum[base + band], spectrum[base + stride + band]);
                    }
                    self.fft.inverse(&self.spectrum, &mut self.time)?;
                    let start = (branch * 2 + ear) * room.short_taps + offset;
                    for (destination, value) in self.impulse[start..start + count]
                        .iter_mut()
                        .zip(&self.time[..count])
                    {
                        *destination += *value;
                    }
                }
            }
        }
        Ok(())
    }

    pub fn hrir(
        &mut self,
        room: &PreparedRoom,
        direction: [f64; 3],
        previous_face: &mut usize,
        output: &mut [f32],
    ) -> Result<()> {
        if room.hrir.data.output_taps * 2 != output.len() {
            return Err(Error::InvalidArgument("Invalid HRIR time buffer"));
        }
        self.decoder.query(direction, previous_face, output)
    }

    pub fn direct_hrir(
        &mut self,
        room: &PreparedRoom,
        direction: [f64; 3],
        distance: f64,
        previous_face: &mut usize,
        output: &mut [f32],
    ) -> Result<()> {
        if room.hrir.data.output_taps * 2 != output.len() {
            return Err(Error::InvalidArgument("Invalid HRIR time buffer"));
        }
        self.decoder
            .query_distance(direction, Some(distance), previous_face, output)
    }
}
