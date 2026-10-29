//! Owned control-thread profiles. Public PODs are read only through their advertised sizes.
use super::{Band, Snapshot, dsp};
use std::{
    ffi::{CStr, c_void},
    mem::{offset_of, size_of},
    panic::{AssertUnwindSafe, catch_unwind},
    ptr,
};

#[repr(C)]
#[derive(Clone, Copy, Default)]
struct ApiBand {
    struct_size: u32,
    kind: i32,
    enabled: i32,
    reserved: u32,
    frequency: f64,
    gain_db: f64,
    q: f64,
}
#[repr(C)]
#[derive(Clone, Copy, Default)]
struct Config {
    struct_size: u32,
    path: *const u8,
    mode: i32,
    reserved: u32,
    revision: u64,
}
#[repr(C)]
#[derive(Clone, Copy, Default)]
struct Parameters {
    struct_size: u32,
    count: u32,
    bands: *const ApiBand,
    preamp: f64,
    mode: i32,
    reserved: u32,
    revision: u64,
}
#[repr(C)]
#[derive(Clone, Copy, Default)]
struct StereoConfig {
    struct_size: u32,
    mode: i32,
    left: *const u8,
    right: *const u8,
    revision: u64,
}
#[repr(C)]
#[derive(Clone, Copy, Default)]
struct StereoParameters {
    struct_size: u32,
    mode: i32,
    left_count: u32,
    right_count: u32,
    left: *const ApiBand,
    right: *const ApiBand,
    left_preamp: f64,
    right_preamp: f64,
    revision: u64,
}

type Failure = (i32, String);
type Result<T> = std::result::Result<T, Failure>;
fn invalid(message: &str) -> Failure {
    (1, format!("HpTF:{message}"))
}
fn ear(index: usize, error: Failure) -> Failure {
    (
        error.0,
        format!(
            "HpTF:{}耳: {}",
            if index == 0 { "左" } else { "右" },
            error.1
        ),
    )
}
fn dsp_error(error: mradm_dsp::Error) -> Failure {
    (1, error.to_string())
}
fn boundary(message: *mut u8, capacity: usize, f: impl FnOnce() -> Result<()>) -> i32 {
    let (code, detail) = match catch_unwind(AssertUnwindSafe(f)) {
        Ok(Ok(())) => (0, String::new()),
        Ok(Err(e)) => e,
        Err(_) => (6, "HpTF:参数准备失败".into()),
    };
    if !message.is_null() && capacity != 0 {
        let mut len = detail.len().min(capacity - 1);
        while !detail.is_char_boundary(len) {
            len -= 1;
        }
        unsafe {
            ptr::copy_nonoverlapping(detail.as_ptr(), message, len);
            *message.add(len) = 0;
        }
    }
    code
}
unsafe fn sized<T: Copy + Default>(source: *const c_void, minimum: usize) -> Result<T> {
    if source.is_null() {
        return Err(invalid("参数指针无效"));
    }
    let size = unsafe { source.cast::<u32>().read_unaligned() } as usize;
    if size < minimum {
        return Err(invalid("参数结构大小无效"));
    }
    let mut result = T::default();
    unsafe {
        ptr::copy_nonoverlapping(
            source.cast::<u8>(),
            (&raw mut result).cast::<u8>(),
            size.min(size_of::<T>()),
        );
    }
    Ok(result)
}
fn mode(value: i32) -> Result<dsp::PreampMode> {
    dsp::PreampMode::try_from(value as u32).map_err(dsp_error)
}
fn profile_error(profile: &dsp::Profile) -> Option<(usize, &'static str)> {
    if !profile.preamp_db.is_finite() {
        return Some((0, "HpTF:Preamp 必须是有限数"));
    }
    let mut enabled = 0;
    for (index, b) in profile.bands.iter().enumerate() {
        if !b.frequency.is_finite()
            || b.frequency <= 0.0
            || !b.q.is_finite()
            || b.q <= 0.0
            || !b.gain_db.is_finite()
        {
            return Some((
                index + 1,
                "HpTF:滤波器类型必须有效,参数必须为有限数且 Fc/Q 为正",
            ));
        }
        enabled += usize::from(b.enabled);
        if enabled > dsp::MAX_BANDS {
            return Some((0, "HpTF:启用的滤波器段数超过上限 32"));
        }
    }
    None
}
fn validate(profile: dsp::Profile) -> Result<dsp::Profile> {
    if let Some((index, message)) = profile_error(&profile) {
        return Err((
            1,
            if index == 0 {
                message.into()
            } else {
                format!("{message} (band={index})")
            },
        ));
    }
    Ok(profile)
}
unsafe fn profile(bands: *const ApiBand, count: u32, preamp: f64) -> Result<dsp::Profile> {
    let mut result = dsp::Profile {
        bands: Vec::new(),
        preamp_db: preamp,
    };
    if count != 0 {
        if bands.is_null() {
            return Err(invalid("滤波器数组指针无效"));
        }
        let stride = unsafe { bands.cast::<u32>().read_unaligned() } as usize;
        let bytes = stride
            .checked_mul(count as usize)
            .filter(|n| *n <= isize::MAX as usize);
        if stride < size_of::<ApiBand>() || bytes.is_none() {
            return Err(invalid("滤波器数组 stride 无效"));
        }
        for index in 0..count as usize {
            let p = unsafe { bands.cast::<u8>().add(index * stride) };
            let b: ApiBand = unsafe { sized(p.cast(), size_of::<ApiBand>()) }?;
            if b.struct_size as usize != stride || b.reserved != 0 || !(0..=1).contains(&b.enabled)
            {
                return Err(invalid("滤波器 stride、启用状态或保留字段无效"));
            }
            result.bands.push(dsp::Band {
                kind: dsp::BandKind::try_from(b.kind as u32).map_err(dsp_error)?,
                enabled: b.enabled != 0,
                frequency: b.frequency,
                gain_db: b.gain_db,
                q: b.q,
            });
        }
    }
    validate(result)
}
unsafe fn file(path: *const u8) -> Result<dsp::Profile> {
    let path = if path.is_null() {
        ""
    } else {
        unsafe { CStr::from_ptr(path.cast()) }
            .to_str()
            .map_err(|_| invalid("路径必须为 UTF-8"))?
    };
    if path.is_empty() {
        return Ok(dsp::Profile {
            bands: Vec::new(),
            preamp_db: 0.0,
        });
    }
    let text = std::fs::read(path).map_err(|_| (3, "HpTF:无法读取 ParametricEQ 文件".into()))?;
    let profile = dsp::parse_parametric_eq(&text)
        .map_err(|e| invalid(&format!("ParametricEQ 解析失败: {:?}", e.kind)))?;
    validate(profile)
}

pub struct Settings {
    profiles: [dsp::Profile; 2],
    mode: dsp::PreampMode,
    revision: u64,
    independent: bool,
}
impl Settings {
    unsafe fn read(source: *const c_void, kind: u32) -> Result<Self> {
        let (profiles, mode_value, revision) = match kind {
            0 => {
                let mut c: Config =
                    unsafe { sized(source, offset_of!(Config, path) + size_of::<*const u8>()) }?;
                if (c.struct_size as usize) < offset_of!(Config, mode) + size_of::<i32>() {
                    c.mode = 0;
                }
                if (c.struct_size as usize) < offset_of!(Config, revision) + size_of::<u64>() {
                    c.revision = 0;
                }
                mode(c.mode)?;
                let p = unsafe { file(c.path) }?;
                ([p.clone(), p], c.mode, c.revision)
            }
            1 => {
                let c: Parameters = unsafe { sized(source, size_of::<Parameters>()) }?;
                if c.reserved != 0 {
                    return Err(invalid("保留字段无效"));
                }
                mode(c.mode)?;
                let p = unsafe { profile(c.bands, c.count, c.preamp) }?;
                ([p.clone(), p], c.mode, c.revision)
            }
            2 => {
                let c: StereoConfig = unsafe { sized(source, size_of::<StereoConfig>()) }?;
                mode(c.mode)?;
                let left = unsafe { file(c.left) }.map_err(|e| ear(0, e))?;
                let right = unsafe { file(c.right) }.map_err(|e| ear(1, e))?;
                ([left, right], c.mode, c.revision)
            }
            3 => {
                let c: StereoParameters = unsafe { sized(source, size_of::<StereoParameters>()) }?;
                mode(c.mode)?;
                let left = unsafe { profile(c.left, c.left_count, c.left_preamp) }
                    .map_err(|e| ear(0, e))?;
                let right = unsafe { profile(c.right, c.right_count, c.right_preamp) }
                    .map_err(|e| ear(1, e))?;
                ([left, right], c.mode, c.revision)
            }
            _ => return Err(invalid("未知参数来源")),
        };
        Ok(Self {
            profiles,
            mode: mode(mode_value)?,
            revision,
            independent: kind >= 2,
        })
    }
    fn design(&self, rate: u32) -> Result<dsp::StereoSnapshot> {
        let p = &self.profiles;
        let coefficients = if self.independent {
            dsp::design_stereo(
                [(&p[0].bands, p[0].preamp_db), (&p[1].bands, p[1].preamp_db)],
                rate,
                self.mode,
            )
            .map_err(|(i, e)| ear(i, dsp_error(e)))?
        } else {
            [dsp::design(&p[0].bands, p[0].preamp_db, rate, self.mode).map_err(dsp_error)?; 2]
        };
        Ok(dsp::StereoSnapshot {
            coefficients,
            revision: self.revision,
            independent: self.independent,
        })
    }
}

/// source_kind: 0 legacy file config, 1 legacy parameters, 2 stereo file config, 3 stereo parameters.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn mradm_dsp_hptf_settings_create(
    source: *const c_void,
    source_kind: u32,
    out: *mut *mut Settings,
    message: *mut u8,
    capacity: usize,
) -> i32 {
    boundary(message, capacity, || {
        if out.is_null() {
            return Err(invalid("输出指针无效"));
        }
        unsafe {
            *out = ptr::null_mut();
        }
        let value = unsafe { Settings::read(source, source_kind) }?;
        unsafe {
            *out = Box::into_raw(Box::new(value));
        }
        Ok(())
    })
}
#[unsafe(no_mangle)]
pub unsafe extern "C" fn mradm_dsp_hptf_settings_destroy(settings: *mut Settings) {
    if !settings.is_null() {
        unsafe {
            drop(Box::from_raw(settings));
        }
    }
}
#[unsafe(no_mangle)]
pub unsafe extern "C" fn mradm_dsp_hptf_settings_design(
    settings: *const Settings,
    rate: u32,
    out: *mut Snapshot,
    message: *mut u8,
    capacity: usize,
) -> i32 {
    boundary(message, capacity, || {
        let settings = unsafe { settings.as_ref() }.ok_or_else(|| invalid("配置指针无效"))?;
        if out.is_null() {
            return Err(invalid("输出指针无效"));
        }
        let value = settings.design(rate)?;
        unsafe {
            *out = value.into();
        }
        Ok(())
    })
}

#[unsafe(no_mangle)]
pub unsafe extern "C" fn mradm_dsp_hptf_validate(
    bands: *const Band,
    count: usize,
    preamp: f64,
    invalid_band: *mut usize,
    message: *mut u8,
    capacity: usize,
) -> i32 {
    boundary(message, capacity, || {
        let bands = unsafe { super::input(bands, count) }.map_err(dsp_error)?;
        let bands = bands
            .iter()
            .map(|b| {
                Ok(dsp::Band {
                    kind: dsp::BandKind::try_from(b.kind).map_err(dsp_error)?,
                    enabled: b.enabled != 0,
                    frequency: b.frequency,
                    gain_db: b.gain_db,
                    q: b.q,
                })
            })
            .collect::<Result<Vec<_>>>()?;
        if invalid_band.is_null() {
            return Err(invalid("错误位置指针无效"));
        }
        unsafe {
            *invalid_band = 0;
        }
        if let Some((index, message)) = profile_error(&dsp::Profile {
            preamp_db: preamp,
            bands,
        }) {
            unsafe {
                *invalid_band = index;
            }
            return Err((1, message.into()));
        }
        Ok(())
    })
}

#[cfg(test)]
mod tests {
    use super::*;
    #[test]
    fn public_pod_layouts_are_stable_on_supported_64_bit_hosts() {
        assert_eq!(
            (
                size_of::<ApiBand>(),
                size_of::<Config>(),
                size_of::<Parameters>()
            ),
            (40, 32, 40)
        );
        assert_eq!(
            (size_of::<StereoConfig>(), size_of::<StereoParameters>()),
            (32, 56)
        );
        assert_eq!(
            (
                offset_of!(StereoParameters, left),
                offset_of!(StereoParameters, revision)
            ),
            (16, 48)
        );
    }
    #[test]
    fn owned_stereo_settings_redesign_after_caller_memory_is_gone() {
        let mut band = ApiBand {
            struct_size: size_of::<ApiBand>() as u32,
            kind: 0,
            enabled: 1,
            frequency: 20000.0,
            gain_db: 3.0,
            q: 1.0,
            ..ApiBand::default()
        };
        let mut request = StereoParameters {
            struct_size: size_of::<StereoParameters>() as u32,
            left: &band,
            left_count: 1,
            left_preamp: -6.0,
            right_preamp: -9.0,
            revision: 73,
            ..StereoParameters::default()
        };
        let settings = unsafe { Settings::read((&raw const request).cast(), 3) }.unwrap();
        band.q = f64::NAN;
        request.left_preamp = 0.0;
        let low = settings.design(32000).unwrap();
        let high = settings.design(96000).unwrap();
        assert_eq!(
            (
                low.coefficients[0].band_count,
                high.coefficients[0].band_count
            ),
            (0, 1)
        );
        assert_eq!(
            (
                high.coefficients[0].preamp_db,
                high.coefficients[1].preamp_db,
                high.revision
            ),
            (-6.0, -9.0, 73)
        );
        assert!(unsafe { Settings::read((&raw const request).cast(), 3) }.is_err());
        assert!(band.q.is_nan());
    }
    #[test]
    fn truncated_legacy_file_defaults_mode_and_revision_but_not_partial_fields() {
        let mut c = Config {
            struct_size: (offset_of!(Config, path) + size_of::<*const u8>()) as u32,
            mode: 7,
            revision: 99,
            ..Config::default()
        };
        let s = unsafe { Settings::read((&raw const c).cast(), 0) }.unwrap();
        assert_eq!(s.revision, 0);
        assert!(!s.independent);
        c.struct_size = (offset_of!(Config, mode) + 1) as u32;
        assert!(unsafe { Settings::read((&raw const c).cast(), 0) }.is_ok());
        c.struct_size = size_of::<Config>() as u32;
        assert!(unsafe { Settings::read((&raw const c).cast(), 0) }.is_err());
    }
    #[test]
    fn failed_creation_clears_handle_and_failed_design_preserves_output() {
        let mut handle = ptr::dangling_mut();
        let mut message = [0u8; 256];
        unsafe {
            assert_eq!(
                mradm_dsp_hptf_settings_create(
                    ptr::null(),
                    3,
                    &mut handle,
                    message.as_mut_ptr(),
                    message.len()
                ),
                1
            );
            assert!(handle.is_null());
            let request = StereoParameters {
                struct_size: size_of::<StereoParameters>() as u32,
                left_preamp: -3.0,
                right_preamp: -9.0,
                revision: 8,
                ..StereoParameters::default()
            };
            assert_eq!(
                mradm_dsp_hptf_settings_create(
                    (&raw const request).cast(),
                    3,
                    &mut handle,
                    message.as_mut_ptr(),
                    message.len()
                ),
                0
            );
            let good = (*handle).design(48000).unwrap();
            let mut output: Snapshot = good.into();
            assert_eq!(
                mradm_dsp_hptf_settings_design(
                    handle,
                    0,
                    &mut output,
                    message.as_mut_ptr(),
                    message.len()
                ),
                1
            );
            assert_eq!(dsp::StereoSnapshot::from(output), good);
            assert!(
                std::str::from_utf8(&message[..message.iter().position(|v| *v == 0).unwrap()])
                    .unwrap()
                    .contains("左耳")
            );
            mradm_dsp_hptf_settings_destroy(handle);
        }
    }
}
