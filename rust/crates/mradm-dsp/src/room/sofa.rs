//! Native SOFA preparation, outside the audio thread. Original FIR gain and
//! support are retained; the two causal guards are explicit in HrirData.
use super::resources::{HrirData, sha256};
use crate::{
    Error, Result,
    geometry::{Vec3, cross, dot, scale, sub},
};
use sofar::hdf::{DataObject, ParsedHdf};
use std::collections::BTreeSet;

const RATE: usize = 48000;
const MAX_BYTES: usize = 256 * 1024 * 1024;
fn invalid(message: &'static str) -> Error {
    Error::InvalidArgument(message)
}
fn attribute<'a>(object: &'a DataObject, name: &str) -> Option<&'a str> {
    object
        .parsed_attributes
        .iter()
        .find(|a| a.name == name)
        .and_then(|a| a.value.as_deref())
}
fn child(file: &ParsedHdf<'_>, name: &str) -> Result<Option<DataObject>> {
    file.get_child(name)
        .map(|v| v.map_err(|_| invalid("SOFA 数据集损坏或 HDF5 编码不受支持")))
        .transpose()
}
fn required(file: &ParsedHdf<'_>, name: &str) -> Result<DataObject> {
    child(file, name)?.ok_or(invalid("SOFA 缺少必要数据集"))
}
fn shape(object: &DataObject) -> Vec<u64> {
    object.ds.dimension_size.to_vec()
}
fn numbers(object: &DataObject) -> Result<Vec<f64>> {
    let count = object
        .ds
        .dimension_size
        .iter()
        .try_fold(1usize, |n, v| {
            usize::try_from(*v).ok().and_then(|v| n.checked_mul(v))
        })
        .filter(|n| *n > 0 && *n <= MAX_BYTES / 8)
        .ok_or(invalid("SOFA 数组超过准备预算"))?;
    let width = object.dt.size as usize;
    let class = object.dt.class_and_version & 15;
    if !matches!(class, 0 | 1)
        || !matches!(width, 1 | 2 | 4 | 8)
        || (class == 1 && !matches!(width, 4 | 8))
        || count.checked_mul(width) != Some(object.data.len())
    {
        return Err(invalid("SOFA 数组必须包含实数，且形状与字节数一致"));
    }
    let big = object.dt.class_bit_field & 1 != 0;
    let signed = object.dt.class_bit_field & 8 != 0;
    let mut result = Vec::with_capacity(count);
    for bytes in object.data.chunks_exact(width) {
        let mut word = 0u64;
        for i in 0..width {
            word |= u64::from(bytes[if big { width - 1 - i } else { i }]) << (8 * i);
        }
        let value = if class == 1 {
            if width == 4 {
                f64::from(f32::from_bits(word as u32))
            } else {
                f64::from_bits(word)
            }
        } else if signed {
            (((word << (64 - width * 8)) as i64) >> (64 - width * 8)) as f64
        } else {
            word as f64
        };
        if !value.is_finite() {
            return Err(invalid("SOFA 包含非有限值"));
        }
        result.push(value);
    }
    Ok(result)
}
fn positions(file: &ParsedHdf<'_>, name: &str, default: Option<&[Vec3]>) -> Result<Vec<Vec3>> {
    let Some(object) = child(file, name)? else {
        return default
            .map(<[Vec3]>::to_vec)
            .ok_or(invalid("SOFA 缺少坐标数据"));
    };
    let dims = shape(&object);
    let valid = if name == "ReceiverPosition" {
        dims == [2, 3, 1] || dims == [2, 3]
    } else {
        dims.len() == 2 && dims[1] == 3
    };
    if !valid {
        return Err(invalid("SOFA 坐标形状不受支持"));
    }
    let values = numbers(&object)?;
    let kind = attribute(&object, "Type")
        .unwrap_or("cartesian")
        .to_ascii_lowercase();
    let units = attribute(&object, "Units")
        .unwrap_or("metre")
        .to_ascii_lowercase();
    let units: Vec<_> = units.split(',').map(str::trim).collect();
    let metres = |s: &str| matches!(s, "metre" | "meter" | "metres" | "meters" | "m");
    let degrees = |s: &str| matches!(s, "degree" | "degrees" | "deg");
    if kind == "cartesian" && matches!(units.len(), 1 | 3) && units.iter().all(|u| metres(u)) {
        return Ok(values.as_chunks::<3>().0.to_vec());
    }
    if kind != "spherical"
        || units.len() != 3
        || !degrees(units[0])
        || !degrees(units[1])
        || !metres(units[2])
    {
        return Err(invalid("SOFA 坐标必须使用米或角度、角度、米"));
    }
    values
        .as_chunks::<3>()
        .0
        .iter()
        .map(|p| {
            if p[2] < 0. {
                return Err(invalid("SOFA 球坐标半径不能为负"));
            }
            let (sa, ca) = libm::sincos(p[0].to_radians());
            let (se, ce) = libm::sincos(p[1].to_radians());
            Ok([ca * ce * p[2], sa * ce * p[2], se * p[2]])
        })
        .collect()
}
fn fixed(values: &[Vec3]) -> Result<Vec3> {
    let first = *values.first().ok_or(invalid("SOFA 监听者坐标为空"))?;
    if values
        .iter()
        .any(|v| (0..3).any(|i| (v[i] - first[i]).abs() > 1e-10))
    {
        return Err(invalid("房间 SOFA 仅支持固定的单个监听点"));
    }
    Ok(first)
}
fn unit(v: Vec3) -> Result<Vec3> {
    let length = libm::sqrt(dot(v, v));
    if length < 1e-9 || !length.is_finite() {
        return Err(invalid("SOFA 坐标或方向无效"));
    }
    Ok(scale(v, 1. / length))
}

pub fn prepare(bytes: &[u8]) -> Result<HrirData> {
    prepare_at_rate(bytes, RATE as u32)
}

pub fn prepare_at_rate(bytes: &[u8], target_rate: u32) -> Result<HrirData> {
    prepare_inner(bytes, target_rate, None)
}

pub(super) fn prepare_authenticated(
    bytes: &[u8],
    target_rate: u32,
    hash: String,
) -> Result<HrirData> {
    prepare_inner(bytes, target_rate, Some(hash))
}

fn prepare_inner(
    bytes: &[u8],
    target_rate: u32,
    authenticated_hash: Option<String>,
) -> Result<HrirData> {
    super::validate_sample_rate(target_rate)?;
    if bytes.len() > MAX_BYTES {
        return Err(invalid("SOFA 文件超过 256 MiB"));
    }
    let file =
        sofar::hdf::parse_with_children(bytes).map_err(|_| invalid("无法解析 SOFA/HDF5 文件"))?;
    let convention = attribute(&file.root, "SOFAConventions");
    if attribute(&file.root, "Conventions") != Some("SOFA")
        || !matches!(convention, Some("SimpleFreeFieldHRIR" | "GeneralFIR"))
        || attribute(&file.root, "DataType") != Some("FIR")
    {
        return Err(Error::Unsupported(
            "房间仅支持 SimpleFreeFieldHRIR／适用的 GeneralFIR SOFA",
        ));
    }
    let object = required(&file, "Data.IR")?;
    let dims = shape(&object);
    if dims.len() != 3
        || !(4..=65536).contains(&dims[0])
        || dims[1] != 2
        || !(1..=4096).contains(&dims[2])
    {
        return Err(invalid("SOFA Data.IR 必须为 M×2×N 双耳 FIR"));
    }
    let count = dims[0] as usize;
    let taps = dims[2] as usize;
    let ir = numbers(&object)?;
    drop(object);
    let rate = required(&file, "Data.SamplingRate")?;
    if !matches!(
        attribute(&rate, "Units")
            .unwrap_or("hertz")
            .to_ascii_lowercase()
            .as_str(),
        "hertz" | "hz"
    ) {
        return Err(invalid("SOFA 采样率必须使用 Hz"));
    }
    let rates = numbers(&rate)?;
    let rate = rates[0];
    if !(1. ..=384000.).contains(&rate)
        || rate != libm::floor(rate)
        || rates.iter().any(|r| *r != rate)
    {
        return Err(invalid("SOFA 必须提供一个一致的整数采样率"));
    }
    if let Some(emitter) = child(&file, "EmitterPosition")? {
        let values = numbers(&emitter)?;
        if values.len() != 3 || values.iter().any(|v| *v != 0.) {
            return Err(Error::Unsupported(
                "房间 SOFA 仅支持 SourcePosition 上的单个发射点",
            ));
        }
    }
    let listener = fixed(&positions(
        &file,
        "ListenerPosition",
        Some(&[[0., 0., 0.]]),
    )?)?;
    let forward = unit(fixed(&positions(
        &file,
        "ListenerView",
        Some(&[[1., 0., 0.]]),
    )?)?)?;
    let up = unit(fixed(&positions(
        &file,
        "ListenerUp",
        Some(&[[0., 0., 1.]]),
    )?)?)?;
    if dot(forward, up).abs() > 1e-6 {
        return Err(invalid("SOFA ListenerView 与 ListenerUp 必须正交"));
    }
    let left = unit(cross(up, forward))?;
    let up = cross(forward, left);
    let sources = positions(&file, "SourcePosition", None)?;
    if sources.len() != count {
        return Err(invalid("SOFA 测量数与 SourcePosition 不一致"));
    }
    let mut radius = 0.;
    let mut directions = Vec::with_capacity(count);
    let mut unique = BTreeSet::new();
    for source in sources {
        let relative = sub(source, listener);
        let point = [
            dot(relative, forward),
            dot(relative, left),
            dot(relative, up),
        ];
        let distance = libm::sqrt(dot(point, point));
        if directions.is_empty() {
            radius = distance;
        }
        if distance <= 1e-6
            || !distance.is_finite()
            || (distance - radius).abs() > 1e-6 + 1e-5 * radius
        {
            return Err(Error::Unsupported("房间 SOFA 仅支持一个非零测量半径"));
        }
        let point = scale(point, 1. / distance);
        if !unique.insert(point.map(|v| libm::round(v * 1e11) as i64)) {
            return Err(invalid("SOFA 存在重复方向"));
        }
        directions.push(point);
    }
    let default_ears = [[0., 0.09, 0.], [0., -0.09, 0.]];
    let ears = positions(
        &file,
        "ReceiverPosition",
        if convention == Some("SimpleFreeFieldHRIR") {
            Some(&default_ears)
        } else {
            None
        },
    )?;
    let order = if ears[0][1] > 1e-6 && ears[1][1] < -1e-6 {
        [0, 1]
    } else if ears[1][1] > 1e-6 && ears[0][1] < -1e-6 {
        [1, 0]
    } else {
        return Err(invalid("SOFA ReceiverPosition 无法明确识别左右耳"));
    };
    let delay = if let Some(object) = child(&file, "Data.Delay")? {
        let factor = match attribute(&object, "Units")
            .unwrap_or("samples")
            .to_ascii_lowercase()
            .as_str()
        {
            "sample" | "samples" => 1.,
            "second" | "seconds" | "s" => rate,
            _ => return Err(invalid("SOFA Data.Delay 必须使用样本或秒")),
        };
        if ![vec![2], vec![1, 2], vec![count as u64, 2]].contains(&shape(&object)) {
            return Err(invalid("SOFA Data.Delay 必须为 I×R 或 M×R"));
        }
        numbers(&object)?
            .into_iter()
            .map(|v| v * factor)
            .collect::<Vec<_>>()
    } else {
        vec![0.; 2]
    };
    let delays: Vec<_> = (0..count)
        .flat_map(|m| {
            order.map(|ear| {
                delay[(if delay.len() == 2 { 0 } else { m * 2 }) + ear] * f64::from(target_rate)
                    / rate
            })
        })
        .collect();
    let mut ordered = Vec::with_capacity(ir.len());
    for m in 0..count {
        for ear in order {
            ordered.extend_from_slice(&ir[(m * 2 + ear) * taps..(m * 2 + ear + 1) * taps]);
        }
    }
    let (resampled, taps, resample_guard) =
        resample(&ordered, taps, rate as usize, target_rate as usize)?;
    let (impulses, taps, delay_guard) = apply_delays(&resampled, taps, &delays)?;
    let faces = mesh(&directions)?;
    let delays = estimate_delays(&impulses, taps, target_rate as usize)?;
    let lo = delays.iter().copied().fold(f64::INFINITY, f64::min);
    let hi = delays.iter().copied().fold(f64::NEG_INFINITY, f64::max);
    let spread = libm::ceil(hi - lo) as usize;
    let guard = 96.max(40 + spread);
    let output_taps = (taps + 40 + guard + spread).div_ceil(64) * 64;
    if guard > 4096 || output_taps > 8192 {
        return Err(invalid("SOFA 完整插值响应超过 v1 支持预算"));
    }
    let source_hash = authenticated_hash.unwrap_or_else(|| sha256(bytes));
    Ok(HrirData {
        sample_rate: target_rate,
        source_sample_rate: Some(rate as u32),
        collection: None,
        source_geometry: Some(super::collection::SourceGeometry {
            radius,
            sample_rate: rate as u32,
            receivers: [ears[0], ears[1]],
            order,
            origins: point_origins(&file, count)?,
        }),
        identity: if target_rate == RATE as u32 {
            sha256(format!("mradm-native-sofa-v1:{source_hash}").as_bytes())
        } else {
            sha256(format!("mradm-native-sofa-v2:{target_rate}:{source_hash}").as_bytes())
        },
        source_sofa_sha256: Some(source_hash),
        impulses,
        taps,
        directions,
        faces,
        delays: delays.as_chunks::<2>().0.to_vec(),
        guard,
        preprocessing_guard: resample_guard + delay_guard,
        output_taps,
    })
}

fn point_origins(file: &ParsedHdf<'_>, count: usize) -> Result<serde_json::Value> {
    let mut result = serde_json::Map::new();
    for (name, band) in [
        ("MRADMPointOrigin", "low_band"),
        ("MRADMPointOriginHighBand", "high_band"),
    ] {
        if let Some(object) = child(file, name)? {
            if shape(&object) != [count as u64] {
                return Err(invalid("SOFA 来源标记与方向数不一致"));
            }
            let mut counts = std::collections::BTreeMap::<String, usize>::new();
            for code in numbers(&object)? {
                *counts.entry(code.to_string()).or_default() += 1;
            }
            result.insert(band.into(), serde_json::json!(counts));
        }
    }
    Ok(result.into())
}

fn mesh(directions: &[Vec3]) -> Result<Vec<[u32; 3]>> {
    let faces = crate::geometry::hull(directions)?;
    let mut result = Vec::with_capacity(faces.len());
    for f in faces {
        let [a, b, c] = f.map(|i| directions[i]);
        let normal = cross(sub(b, a), sub(c, a));
        // Test the original geometry, not the deterministic tie-breaking jitter.
        if dot(normal, a) / libm::sqrt(dot(normal, normal)) <= 1e-8
            || dot(a, cross(b, c)).abs() < 1e-12
        {
            return Err(invalid("SOFA 方向覆盖不足或网格退化"));
        }
        let mut face = f.map(|i| i as u32);
        face.sort_unstable();
        result.push(face);
    }
    result.sort_unstable();
    if result.len() != 2 * directions.len() - 4 {
        return Err(invalid("SOFA 无法生成完整闭合网格"));
    }
    Ok(result)
}
fn sinc(x: f64) -> f64 {
    if x == 0. {
        1.
    } else {
        let p = std::f64::consts::PI * x;
        libm::sin(p) / p
    }
}
fn i0(x: f64) -> f64 {
    let mut term = 1.;
    let mut sum = 1.;
    for k in 1..100 {
        term *= x * x / (4. * f64::from(k * k));
        sum += term;
        if term < sum * 1e-16 {
            break;
        }
    }
    sum
}
fn resample(
    ir: &[f64],
    taps: usize,
    rate: usize,
    target: usize,
) -> Result<(Vec<f64>, usize, usize)> {
    if rate == target {
        return Ok((ir.to_vec(), taps, 0));
    }
    let mut a = target;
    let mut b = rate;
    while b != 0 {
        let r = a % b;
        a = b;
        b = r;
    }
    let up = target / a;
    let down = rate / a;
    let half = (10 * up.max(down)).div_ceil(down) * down;
    if 2 * half + 1 > 1_000_001 {
        return Err(invalid("SOFA 重采样比率超过预算"));
    }
    let length = ((taps - 1) * up + 2 * half) / down + 1;
    let rows = ir.len() / taps;
    if length > 4096 || rows * length > MAX_BYTES / 8 {
        return Err(invalid("SOFA 完整重采样响应超过预算"));
    }
    let cutoff = 1. / up.max(down) as f64;
    let mut filter: Vec<_> = (0..=2 * half)
        .map(|i| {
            let t = (i as f64 - half as f64) / half as f64;
            cutoff
                * sinc(cutoff * (i as f64 - half as f64))
                * i0(8.6 * libm::sqrt((1. - t * t).max(0.)))
                / i0(8.6)
        })
        .collect();
    let sum: f64 = filter.iter().sum();
    for x in &mut filter {
        *x = *x / sum * down as f64;
    }
    let mut out = vec![0.; rows * length];
    for (input, output) in ir.chunks_exact(taps).zip(out.chunks_exact_mut(length)) {
        for (j, y) in output.iter_mut().enumerate() {
            let time = j * down;
            let start = time.saturating_sub(2 * half).div_ceil(up);
            let end = (time / up + 1).min(taps);
            for i in start..end {
                *y += input[i] * filter[time - i * up];
            }
        }
    }
    Ok((out, length, half / down))
}
fn apply_delays(ir: &[f64], taps: usize, delays: &[f64]) -> Result<(Vec<f32>, usize, usize)> {
    let delays: Vec<_> = delays
        .iter()
        .map(|&d| {
            let n = libm::round(d);
            if (d - n).abs() < 1e-10 { n } else { d }
        })
        .collect();
    let low = delays.iter().copied().fold(f64::INFINITY, f64::min);
    let high = delays.iter().copied().fold(f64::NEG_INFINITY, f64::max);
    if low.abs().max(high.abs()) > 4096. {
        return Err(invalid("SOFA Data.Delay 超过完整响应预算"));
    }
    let fractional = delays.iter().any(|v| *v != libm::floor(*v));
    let extra = if fractional { 40 } else { 0 };
    let guard = extra + (-libm::floor(low)).max(0.) as usize;
    let length = (libm::ceil(high) as i64 + guard as i64 + taps as i64 + extra as i64) as usize;
    if length == 0 || length > 4096 || delays.len() * length > MAX_BYTES / 8 {
        return Err(invalid("SOFA 完整时延响应超过预算"));
    }
    let mut out = vec![0f32; delays.len() * length];
    let mut work = vec![0.; length];
    for ((input, output), delay) in ir
        .chunks_exact(taps)
        .zip(out.chunks_exact_mut(length))
        .zip(delays)
    {
        work.fill(0.);
        let integer = libm::floor(delay);
        let fraction = delay - integer;
        if fraction == 0. {
            let start = (integer as i64 + guard as i64) as usize;
            work[start..start + taps].copy_from_slice(input);
        } else {
            let mut kernel = [0.; 81];
            for (i, k) in kernel.iter_mut().enumerate() {
                *k = sinc(i as f64 - 40. - fraction)
                    * (0.5 - 0.5 * libm::cos(std::f64::consts::TAU * i as f64 / 80.));
            }
            let sum: f64 = kernel.iter().sum();
            for k in &mut kernel {
                *k /= sum;
            }
            let start = (integer as i64 + guard as i64 - 40) as usize;
            for (i, x) in input.iter().enumerate() {
                for (j, k) in kernel.iter().enumerate() {
                    work[start + i + j] += x * k;
                }
            }
        }
        for (o, x) in output.iter_mut().zip(&work) {
            *o = *x as f32;
            if !o.is_finite() {
                return Err(invalid("SOFA 幅度超出 float32 范围"));
            }
        }
    }
    Ok((out, length, guard))
}

// Preparation-only f64 radix-2 FFT. Portable libm twiddles avoid platform C
// trigonometry in the excess-phase estimate; runtime FFT plans are unchanged.
struct Fft {
    twiddles: Vec<[f64; 2]>,
    reverse: Vec<usize>,
}
impl Fft {
    fn new(n: usize) -> Self {
        let twiddles = (0..n / 2)
            .map(|k| {
                let (s, c) = libm::sincos(-std::f64::consts::TAU * k as f64 / n as f64);
                [c, s]
            })
            .collect();
        let reverse = (0..n)
            .map(|i| i.reverse_bits() >> (usize::BITS - n.trailing_zeros()))
            .collect();
        Self { twiddles, reverse }
    }
    fn run(&self, data: &mut [[f64; 2]], inverse: bool) {
        let n = data.len();
        for (i, &j) in self.reverse.iter().enumerate() {
            if i < j {
                data.swap(i, j);
            }
        }
        let mut width = 2;
        while width <= n {
            let half = width / 2;
            let step = n / width;
            for block in data.chunks_exact_mut(width) {
                let (a, b) = block.split_at_mut(half);
                for (k, (a, b)) in a.iter_mut().zip(b).enumerate() {
                    let [wr, mut wi] = self.twiddles[k * step];
                    if inverse {
                        wi = -wi;
                    }
                    let re = b[0] * wr - b[1] * wi;
                    let im = b[0] * wi + b[1] * wr;
                    *b = [a[0] - re, a[1] - im];
                    *a = [a[0] + re, a[1] + im];
                }
            }
            width *= 2;
        }
        if inverse {
            for v in data {
                v[0] /= n as f64;
                v[1] /= n as f64;
            }
        }
    }
}
struct DelayPlan {
    fft: Fft,
    n: usize,
    first: usize,
    last: usize,
    x: Vec<f64>,
    mean: f64,
    denominator: f64,
}
impl DelayPlan {
    fn new(taps: usize, target: usize) -> Self {
        let n = (4 * taps).next_power_of_two().max(2048);
        let first = (300 * n).div_ceil(target);
        let last = 1500 * n / target;
        let x: Vec<_> = (first..=last)
            .map(|k| std::f64::consts::TAU * k as f64 / n as f64)
            .collect();
        let mean = x.iter().sum::<f64>() / x.len() as f64;
        let denominator = x.iter().map(|v| (v - mean) * (v - mean)).sum::<f64>();
        Self {
            fft: Fft::new(n),
            n,
            first,
            last,
            x,
            mean,
            denominator,
        }
    }
    fn estimate(&self, ir: &[f32], taps: usize, delays: &mut [f64]) -> Result<()> {
        let Self {
            fft,
            n,
            first,
            last,
            x,
            mean,
            denominator,
        } = self;
        let (n, first, last, mean, denominator) = (*n, *first, *last, *mean, *denominator);
        let mut original = vec![[0.; 2]; n];
        let mut work = original.clone();
        for (row, destination) in ir.chunks_exact(taps).zip(delays) {
            original.fill([0.; 2]);
            for (v, x) in original.iter_mut().zip(row) {
                v[0] = f64::from(*x);
            }
            fft.run(&mut original, false);
            let peak = original[..=n / 2]
                .iter()
                .map(|v| libm::hypot(v[0], v[1]))
                .fold(0., f64::max);
            if peak == 0. {
                return Err(invalid("SOFA 每个方向的每只耳都必须有非零响应"));
            }
            for k in 0..=n / 2 {
                let value =
                    libm::log(libm::hypot(original[k][0], original[k][1]).max(peak * 1e-12));
                work[k] = [value, 0.];
                if k > 0 && k < n / 2 {
                    work[n - k] = [value, 0.];
                }
            }
            fft.run(&mut work, true);
            for (k, v) in work.iter_mut().enumerate() {
                *v = [
                    v[0] * if k == 0 || k == n / 2 {
                        1.
                    } else if k < n / 2 {
                        2.
                    } else {
                        0.
                    },
                    0.,
                ];
            }
            fft.run(&mut work, false);
            let mut previous = 0.;
            let mut offset = 0.;
            let mut numerator = 0.;
            for k in 0..=last {
                let phase = libm::atan2(original[k][1], original[k][0]) - work[k][1];
                if k > 0 {
                    let diff = phase - previous;
                    if diff.abs() >= std::f64::consts::PI {
                        let mut wrapped = diff
                            - std::f64::consts::TAU
                                * libm::floor(
                                    (diff + std::f64::consts::PI) / std::f64::consts::TAU,
                                );
                        if wrapped == -std::f64::consts::PI && diff > 0. {
                            wrapped = std::f64::consts::PI;
                        }
                        offset += wrapped - diff;
                    }
                }
                previous = phase;
                if k >= first {
                    numerator += (phase + offset) * (x[k - first] - mean);
                }
            }
            let delay = -numerator / denominator;
            if !delay.is_finite() || delay.abs() > taps as f64 {
                return Err(invalid("SOFA 超额相位时延超出完整响应长度"));
            }
            *destination = delay;
        }
        Ok(())
    }
}

fn estimate_delays_with_workers(
    ir: &[f32],
    taps: usize,
    target: usize,
    workers: usize,
) -> Result<Vec<f64>> {
    let plan = DelayPlan::new(taps, target);
    let count = ir.len() / taps;
    let mut delays = vec![0.; count];
    let workers = workers.clamp(1, 4).min(count.max(1));
    if workers == 1 {
        plan.estimate(ir, taps, &mut delays)?;
    } else {
        use rayon::prelude::*;
        // One private pool bounds preparation concurrency across simultaneous
        // loads as well as within a layer. Audio workers never enter this pool.
        // Each task owns its scratch; indexed collection keeps first-row errors.
        static POOL: std::sync::Mutex<Option<std::sync::Arc<rayon::ThreadPool>>> =
            std::sync::Mutex::new(None);
        let pool = {
            let mut slot = POOL
                .lock()
                .map_err(|_| Error::RenderFailed("HRIR preparation pool failed"))?;
            if let Some(pool) = &*slot {
                pool.clone()
            } else {
                // A transient thread-creation error must not become a cached
                // failure for every subsequent load in this process.
                let pool = rayon::ThreadPoolBuilder::new()
                    .num_threads(
                        std::thread::available_parallelism()
                            .map_or(1, usize::from)
                            .min(4),
                    )
                    .thread_name(|i| format!("mradm-hrir-prepare-{i}"))
                    .build()
                    .map_err(|_| Error::RenderFailed("Cannot start HRIR preparation workers"))?;
                let pool = std::sync::Arc::new(pool);
                *slot = Some(pool.clone());
                pool
            }
        };
        let chunk = count.div_ceil(workers);
        let results: Vec<Result<()>> = pool.install(|| {
            ir.par_chunks(chunk * taps)
                .zip(delays.par_chunks_mut(chunk))
                .map(|(input, output)| plan.estimate(input, taps, output))
                .collect()
        });
        for result in results {
            result?;
        }
    }
    Ok(delays)
}

fn estimate_delays(ir: &[f32], taps: usize, target: usize) -> Result<Vec<f64>> {
    let workers = if ir.len() / taps >= 128 {
        std::thread::available_parallelism()
            .map_or(1, usize::from)
            .min(4)
    } else {
        1
    };
    estimate_delays_with_workers(ir, taps, target, workers)
}

#[cfg(test)]
mod tests {
    use super::*;
    #[test]
    fn preparation_workers_preserve_every_delay_bit_and_first_error() {
        let taps = 128;
        let mut ir = vec![0.; taps * 133];
        for (index, row) in ir.chunks_exact_mut(taps).enumerate() {
            row[3 + index % 47] = 0.8;
            row[70 + index % 31] = -0.03;
        }
        for rate in [48000, 96000] {
            let expected = estimate_delays_with_workers(&ir, taps, rate, 1).unwrap();
            for workers in [2, 3, 4] {
                let actual = estimate_delays_with_workers(&ir, taps, rate, workers).unwrap();
                assert!(
                    expected
                        .iter()
                        .zip(actual)
                        .all(|(a, b)| a.to_bits() == b.to_bits())
                );
            }
        }
        ir[taps * 11..taps * 12].fill(0.);
        ir[taps * 100..taps * 101].fill(0.);
        let expected = estimate_delays_with_workers(&ir, taps, 96000, 1)
            .unwrap_err()
            .to_string();
        for workers in [2, 3, 4] {
            assert_eq!(
                estimate_delays_with_workers(&ir, taps, 96000, workers)
                    .unwrap_err()
                    .to_string(),
                expected
            );
        }
    }
    #[test]
    fn complete_resampling_and_fractional_delay_keep_gain_and_time() {
        for target in [48000, 96000] {
            for rate in [44100, 48000, 96000] {
                let mut ir = vec![0.; 64];
                ir[2] = 0.8;
                ir[32 + 4] = 0.6;
                let (resampled, n, resample_guard) = resample(&ir, 32, rate, target).unwrap();
                let d = [
                    -0.25 * target as f64 / rate as f64,
                    2.5 * target as f64 / rate as f64,
                ];
                let (output, n, delay_guard) = apply_delays(&resampled, n, &d).unwrap();
                for ear in 0..2 {
                    let row = &output[ear * n..(ear + 1) * n];
                    let sum = row.iter().map(|v| f64::from(*v)).sum::<f64>();
                    let moment = row
                        .iter()
                        .enumerate()
                        .map(|(i, v)| i as f64 * f64::from(*v))
                        .sum::<f64>()
                        / sum;
                    assert!((sum - [0.8, 0.6][ear]).abs() < 1e-4);
                    let expected = ([2., 4.][ear] + [-0.25, 2.5][ear]) * target as f64
                        / rate as f64
                        + (resample_guard + delay_guard) as f64;
                    assert!(
                        (moment - expected).abs() < 0.002,
                        "{rate}: {moment} != {expected}"
                    );
                }
            }
        }
    }
    #[test]
    fn portable_cepstrum_recovers_unscaled_integer_delay() {
        let mut ir = vec![0.; 2 * 128];
        ir[35] = 0.8;
        ir[128 + 51] = 0.6;
        let delays = estimate_delays(&ir, 128, RATE).unwrap();
        assert!((delays[0] - 35.).abs() < 1e-10);
        assert!((delays[1] - 51.).abs() < 1e-10);
    }
    #[test]
    fn hdf_filters_preserve_samples_coordinates_and_rate() {
        let original = prepare(include_bytes!(
            "../../../../../tests/fixtures/sofa/simple.sofa"
        ))
        .unwrap();
        let expected: Vec<_> = original.impulses.iter().map(|v| v + 0.125).collect();
        for (name, bytes) in [
            (
                "chunked",
                &include_bytes!("../../../../../tests/fixtures/sofa/filters-chunked.sofa")[..],
            ),
            (
                "shuffle",
                &include_bytes!("../../../../../tests/fixtures/sofa/filters-shuffle.sofa")[..],
            ),
            (
                "deflate",
                &include_bytes!("../../../../../tests/fixtures/sofa/filters-deflate.sofa")[..],
            ),
            (
                "shuffle-deflate",
                &include_bytes!("../../../../../tests/fixtures/sofa/filters-shuffle-deflate.sofa")
                    [..],
            ),
            (
                "skipped",
                &include_bytes!("../../../../../tests/fixtures/sofa/filters-skipped.sofa")[..],
            ),
        ] {
            let data = prepare(bytes).unwrap_or_else(|error| panic!("{name}: {error}"));
            assert_eq!(data.impulses, expected, "{name}");
            assert_eq!(data.directions, original.directions, "{name}");
            assert_eq!(data.taps, original.taps, "{name}");
            assert_eq!(data.preprocessing_guard, 0, "{name}");
        }
        assert!(
            prepare(include_bytes!(
                "../../../../../tests/fixtures/sofa/filters-unsupported.sofa"
            ))
            .is_err()
        );
    }
    #[test]
    fn native_loader_works_without_optional_binaural_sofa_feature() {
        let data = prepare(include_bytes!(
            "../../../../../tests/fixtures/sofa/simple.sofa"
        ))
        .unwrap();
        assert_eq!(data.directions.len(), 6);
        assert_eq!(data.taps, 16);
        assert_eq!(data.preprocessing_guard, 0);
        for node in 0..6 {
            assert_eq!(data.impulses[node * 32 + 2], (node + 1) as f32 / 8.);
            assert_eq!(data.impulses[node * 32 + 16 + 4], (6 - node) as f32 / 8.);
        }
        let bank = std::sync::Arc::new(
            super::super::hrir::HrirBank::new(std::sync::Arc::new(data), 256).unwrap(),
        );
        let mut query = super::super::hrir::TimeQuery::new(bank.clone()).unwrap();
        let mut output = vec![0.; bank.data.output_taps * 2];
        for (node, d) in bank.data.directions.iter().enumerate() {
            let mut face = usize::MAX;
            query.query(*d, &mut face, &mut output).unwrap();
            assert!((output[bank.data.guard + 2] - (node + 1) as f32 / 8.).abs() < 2e-5);
        }
        let converted = prepare(include_bytes!(
            "../../../../../tests/fixtures/sofa/room-delayed.sofa"
        ))
        .unwrap();
        assert_eq!(converted.directions.len(), 6);
        assert_eq!(converted.preprocessing_guard, 52);
        assert!(converted.taps > 16);
        let seconds = prepare(include_bytes!(
            "../../../../../tests/fixtures/sofa/room-seconds.sofa"
        ))
        .unwrap();
        assert_eq!(seconds.preprocessing_guard, converted.preprocessing_guard);
        assert_eq!(seconds.impulses, converted.impulses);
        let high_rate = prepare(include_bytes!(
            "../../../../../tests/fixtures/sofa/room-96k.sofa"
        ))
        .unwrap();
        assert_eq!(high_rate.preprocessing_guard, 51);
        for (node, direction) in converted.directions.iter().enumerate() {
            let original = [
                [1., 0., 0.],
                [0., 1., 0.],
                [-1., 0., 0.],
                [0., -1., 0.],
                [0., 0., 1.],
                [0., 0., -1.],
            ][node];
            assert_eq!(*direction, original);
            for ear in 0..2 {
                let row = &converted.impulses
                    [(node * 2 + ear) * converted.taps..(node * 2 + ear + 1) * converted.taps];
                let gain = if ear == 0 {
                    (node + 1) as f64 / 8.
                } else {
                    (6 - node) as f64 / 8.
                };
                let energy = row.iter().map(|x| f64::from(*x)).sum::<f64>();
                assert!((energy - gain - 0.015625).abs() < 1e-4);
                let centre = row
                    .iter()
                    .enumerate()
                    .map(|(i, x)| i as f64 * f64::from(*x))
                    .sum::<f64>()
                    / energy;
                let expected = (([2., 4.][ear] * gain + 15. * 0.015625) / (gain + 0.015625)
                    + [-0.25, 2.5][ear])
                    * 48000.
                    / 44100.
                    + 52.;
                assert!((centre - expected).abs() < 0.002);
            }
        }
    }
    #[test]
    fn invalid_files_and_uncovered_sphere_are_rejected() {
        assert!(prepare(b"not a SOFA").is_err());
        let points = [[1., 0., 0.], [0., 1., 0.], [-1., 0., 0.], [0., 0., 1.]];
        assert!(mesh(&points).is_err());
        assert!(apply_delays(&[1., 1.], 1, &[-5000., 0.]).is_err());
        assert!(estimate_delays(&[0.; 32], 16, RATE).is_err());
    }
}
