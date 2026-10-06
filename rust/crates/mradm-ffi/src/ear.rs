//! Private preparation-only EAR boundary. All descriptors are copied before output writes.
use crate::{boundary, checked, ptr};
use mradm_dsp::{Error, Result};
use mradm_ear::{Calculator, Channel, DirectSpeakers, Layout, Normalization, Object};

#[repr(C)]
#[derive(Clone, Copy)]
pub struct MradmEarChannel {
    pub name: [u8; 32],
    pub position: [f64; 3],
    pub nominal: [f64; 3],
    pub azimuth_range: [f64; 2],
    pub elevation_range: [f64; 2],
    pub lfe: u32,
}
#[repr(C)]
#[derive(Clone, Copy)]
pub struct MradmEarObject {
    pub position: [f64; 3],
    pub width: f64,
    pub height: f64,
    pub depth: f64,
    pub gain: f64,
    pub diffuse: f64,
}
#[repr(C)]
#[derive(Clone, Copy)]
pub struct MradmEarLabel {
    pub value: *const u8,
    pub length: usize,
}
#[repr(C)]
#[derive(Clone, Copy)]
pub struct MradmEarDirect {
    pub position: [f64; 3],
    pub bounds: [f64; 6],
    pub low_pass: f64,
    pub high_pass: f64,
    /// Bits 0..5: bounds; 6: low-pass; 7: high-pass; 8: pack ID present.
    pub present: u32,
    pub pack: MradmEarLabel,
}
fn convert<T>(r: mradm_ear::Result<T>) -> Result<T> {
    r.map_err(|e| match e {
        mradm_ear::Error::InvalidArgument(s) => Error::InvalidArgument(s),
        mradm_ear::Error::Unsupported(s) => Error::Unsupported(s),
        mradm_ear::Error::RenderFailed(s) => Error::RenderFailed(s),
    })
}
fn string(b: &[u8]) -> Result<String> {
    let n = b
        .iter()
        .position(|v| *v == 0)
        .ok_or(Error::InvalidArgument("Unterminated EAR string"))?;
    Ok(std::str::from_utf8(&b[..n])
        .map_err(|_| Error::InvalidArgument("Invalid EAR UTF-8"))?
        .into())
}
unsafe fn text(p: *const u8, n: usize) -> Result<String> {
    Ok(std::str::from_utf8(unsafe { checked::input(p, n)? })
        .map_err(|_| Error::InvalidArgument("Invalid EAR UTF-8"))?
        .into())
}
fn channel(c: &MradmEarChannel) -> Result<Channel> {
    Ok(Channel {
        name: string(&c.name)?,
        real: c.position,
        nominal: c.nominal,
        azimuth_range: c.azimuth_range,
        elevation_range: c.elevation_range,
        lfe: checked::flag(c.lfe)?,
    })
}
#[unsafe(no_mangle)]
pub unsafe extern "C" fn mradm_ear_layout(
    name: *const u8,
    name_len: usize,
    channels: *mut MradmEarChannel,
    capacity: usize,
    count: *mut usize,
    message: *mut u8,
    message_len: usize,
) -> i32 {
    boundary(message, message_len, || unsafe {
        checked::range(count, 1)?;
        checked::separate(channels, capacity, count, 1)?;
        let layout = convert(mradm_ear::standard_layout(&text(name, name_len)?))?;
        if capacity < layout.channels.len() {
            return Err(Error::InvalidArgument("EAR layout capacity too small"));
        }
        let output = checked::output(channels, capacity)?;
        for (dst, src) in output.iter_mut().zip(&layout.channels) {
            let mut name = [0; 32];
            name[..src.name.len()].copy_from_slice(src.name.as_bytes());
            *dst = MradmEarChannel {
                name,
                position: src.real,
                nominal: src.nominal,
                azimuth_range: src.azimuth_range,
                elevation_range: src.elevation_range,
                lfe: src.lfe as u32,
            };
        }
        *count = layout.channels.len();
        Ok(())
    })
}
#[unsafe(no_mangle)]
pub unsafe extern "C" fn mradm_ear_create(
    name: *const u8,
    name_len: usize,
    channels: *const MradmEarChannel,
    count: usize,
    output: *mut *mut Calculator,
    message: *mut u8,
    message_len: usize,
) -> i32 {
    boundary(message, message_len, || unsafe {
        checked::range(output, 1)?;
        checked::separate(output, 1, channels, count)?;
        checked::separate(output, 1, name, name_len)?;
        *output = ptr::null_mut();
        let name = text(name, name_len)?;
        let channels = checked::input(channels, count)?
            .iter()
            .map(channel)
            .collect::<Result<Vec<_>>>()?;
        let calc = convert(Calculator::new(Layout { name, channels }))?;
        *output = Box::into_raw(Box::new(calc));
        Ok(())
    })
}
#[unsafe(no_mangle)]
pub unsafe extern "C" fn mradm_ear_destroy(h: *mut Calculator) {
    if !h.is_null() {
        unsafe {
            drop(Box::from_raw(h));
        }
    }
}
#[unsafe(no_mangle)]
pub unsafe extern "C" fn mradm_ear_objects(
    h: *mut Calculator,
    metadata: *const MradmEarObject,
    direct: *mut f64,
    diffuse: *mut f64,
    count: usize,
    message: *mut u8,
    message_len: usize,
) -> i32 {
    boundary(message, message_len, || unsafe {
        checked::separate(h, 1, metadata, 1)?;
        checked::separate(h, 1, direct, count)?;
        checked::separate(h, 1, diffuse, count)?;
        checked::separate(direct, count, diffuse, count)?;
        let m = *checked::input(metadata, 1)?.first().unwrap();
        let calc = h
            .as_mut()
            .ok_or(Error::InvalidArgument("Null EAR calculator"))?;
        checked::range(direct, count)?;
        checked::range(diffuse, count)?;
        if count != calc.layout().channels.len() {
            return Err(Error::InvalidArgument("EAR gain dimensions"));
        }
        let (d, f) = convert(calc.objects(Object {
            position: m.position,
            width: m.width,
            height: m.height,
            depth: m.depth,
            gain: m.gain,
            diffuse: m.diffuse,
        }))?;
        checked::output(direct, count)?.copy_from_slice(&d);
        checked::output(diffuse, count)?.copy_from_slice(&f);
        Ok(())
    })
}
#[unsafe(no_mangle)]
pub unsafe extern "C" fn mradm_ear_direct(
    h: *mut Calculator,
    metadata: *const MradmEarDirect,
    labels: *const MradmEarLabel,
    label_count: usize,
    gains: *mut f64,
    count: usize,
    message: *mut u8,
    message_len: usize,
) -> i32 {
    boundary(message, message_len, || unsafe {
        checked::separate(h, 1, metadata, 1)?;
        checked::separate(h, 1, labels, label_count)?;
        checked::separate(h, 1, gains, count)?;
        let m = *checked::input(metadata, 1)?.first().unwrap();
        if m.present & !511 != 0 {
            return Err(Error::InvalidArgument("Unknown EAR metadata flags"));
        }
        let labels = checked::input(labels, label_count)?
            .iter()
            .map(|v| text(v.value, v.length))
            .collect::<Result<Vec<_>>>()?;
        let meta = DirectSpeakers {
            labels,
            pack: if m.present & 256 != 0 {
                Some(text(m.pack.value, m.pack.length)?)
            } else {
                None
            },
            position: m.position,
            bounds: std::array::from_fn(|i| {
                if m.present & (1 << i) != 0 {
                    Some(m.bounds[i])
                } else {
                    None
                }
            }),
            low_pass: if m.present & 64 != 0 {
                Some(m.low_pass)
            } else {
                None
            },
            high_pass: if m.present & 128 != 0 {
                Some(m.high_pass)
            } else {
                None
            },
        };
        checked::range(gains, count)?;
        let calc = h
            .as_mut()
            .ok_or(Error::InvalidArgument("Null EAR calculator"))?;
        if count != calc.layout().channels.len() {
            return Err(Error::InvalidArgument("EAR gain dimensions"));
        }
        let result = convert(calc.direct_speakers(&meta))?;
        checked::output(gains, count)?.copy_from_slice(&result);
        Ok(())
    })
}
#[unsafe(no_mangle)]
pub unsafe extern "C" fn mradm_ear_hoa(
    h: *mut Calculator,
    orders: *const i32,
    degrees: *const i32,
    inputs: usize,
    normalization: u32,
    matrix: *mut f64,
    length: usize,
    message: *mut u8,
    message_len: usize,
) -> i32 {
    boundary(message, message_len, || unsafe {
        checked::separate(h, 1, orders, inputs)?;
        checked::separate(h, 1, degrees, inputs)?;
        checked::separate(h, 1, matrix, length)?;
        let orders = checked::input(orders, inputs)?.to_vec();
        let degrees = checked::input(degrees, inputs)?.to_vec();
        let norm = match normalization {
            0 => Normalization::N3d,
            1 => Normalization::Sn3d,
            2 => Normalization::Fuma,
            _ => return Err(Error::Unsupported("Unknown HOA normalization")),
        };
        checked::range(matrix, length)?;
        let calc = h
            .as_mut()
            .ok_or(Error::InvalidArgument("Null EAR calculator"))?;
        if Some(length) != inputs.checked_mul(calc.layout().channels.len()) {
            return Err(Error::InvalidArgument("EAR HOA matrix dimensions"));
        }
        let result = convert(calc.hoa(&orders, &degrees, norm))?;
        checked::output(matrix, length)?.copy_from_slice(&result);
        Ok(())
    })
}
#[unsafe(no_mangle)]
pub unsafe extern "C" fn mradm_ear_filters(
    h: *const Calculator,
    filters: *mut f32,
    length: usize,
    message: *mut u8,
    message_len: usize,
) -> i32 {
    boundary(message, message_len, || unsafe {
        checked::separate(h, 1, filters, length)?;
        checked::range(filters, length)?;
        let calc = h
            .as_ref()
            .ok_or(Error::InvalidArgument("Null EAR calculator"))?;
        if Some(length)
            != calc
                .layout()
                .channels
                .len()
                .checked_mul(mradm_ear::decorrelate::TAPS)
        {
            return Err(Error::InvalidArgument("EAR FIR dimensions"));
        }
        let result = calc.filters();
        checked::output(filters, length)?.copy_from_slice(&result);
        Ok(())
    })
}

#[cfg(test)]
mod tests {
    use super::*;
    #[test]
    fn rejects_aliases_dimensions_and_bad_metadata_without_writing() {
        unsafe {
            let mut channels = [MradmEarChannel {
                name: [0; 32],
                position: [0.; 3],
                nominal: [0.; 3],
                azimuth_range: [0.; 2],
                elevation_range: [0.; 2],
                lfe: 0,
            }; 6];
            let mut count = 99;
            let name = b"0+5+0";
            assert_eq!(
                mradm_ear_layout(
                    name.as_ptr(),
                    name.len(),
                    channels.as_mut_ptr(),
                    1,
                    &mut count,
                    ptr::null_mut(),
                    0
                ),
                1
            );
            assert_eq!(count, 99);
            assert_eq!(
                mradm_ear_layout(
                    name.as_ptr(),
                    name.len(),
                    channels.as_mut_ptr(),
                    6,
                    &mut count,
                    ptr::null_mut(),
                    0
                ),
                0
            );
            let mut handle = ptr::null_mut();
            assert_eq!(
                mradm_ear_create(
                    name.as_ptr(),
                    name.len(),
                    channels.as_ptr(),
                    6,
                    &mut handle,
                    ptr::null_mut(),
                    0
                ),
                0
            );
            let mut a = [17.; 6];
            let mut b = [19.; 6];
            let mut m = MradmEarObject {
                position: [0., 0., 1.],
                width: 0.,
                height: 0.,
                depth: 0.,
                gain: 1.,
                diffuse: 0.,
            };
            assert_eq!(
                mradm_ear_objects(
                    handle,
                    &m,
                    a.as_mut_ptr(),
                    a.as_mut_ptr(),
                    6,
                    ptr::null_mut(),
                    0
                ),
                1
            );
            assert_eq!(a, [17.; 6]);
            assert_eq!(
                mradm_ear_objects(
                    handle,
                    &m,
                    a.as_mut_ptr(),
                    b.as_mut_ptr(),
                    5,
                    ptr::null_mut(),
                    0
                ),
                1
            );
            m.diffuse = f64::NAN;
            assert_eq!(
                mradm_ear_objects(
                    handle,
                    &m,
                    a.as_mut_ptr(),
                    b.as_mut_ptr(),
                    6,
                    ptr::null_mut(),
                    0
                ),
                1
            );
            assert_eq!(a, [17.; 6]);
            assert_eq!(b, [19.; 6]);
            assert_eq!(
                mradm_ear_hoa(
                    handle,
                    [1].as_ptr(),
                    [2].as_ptr(),
                    1,
                    1,
                    a.as_mut_ptr(),
                    6,
                    ptr::null_mut(),
                    0
                ),
                1
            );
            assert_eq!(a, [17.; 6]);
            m.diffuse = 0.;
            assert_eq!(
                mradm_ear_objects(
                    handle,
                    &m,
                    a.as_mut_ptr(),
                    b.as_mut_ptr(),
                    6,
                    ptr::null_mut(),
                    0
                ),
                0
            );
            assert!((a[2] - 1.).abs() < 1e-12);
            mradm_ear_destroy(handle);
            mradm_ear_destroy(ptr::null_mut());
        }
    }
}
