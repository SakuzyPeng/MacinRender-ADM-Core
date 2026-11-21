//! IAMF output (PCM consistency 7.2 C3): the final 32-bit PCM WAVE handed to the official AOM
//! iamf-tools bridge (`tools/iamf_aom_bridge/mr_iamf_aom_bridge.h`) through hand-written `extern`
//! declarations, with the checks, options and messages of the C++ island it replaces
//! (`convert_to_iamf`, `iamf_io.cpp`).
//!
//! The bridge is linked only by builds with `MR_ADM_ENABLE_IAMF=ON`: the islands' CMake project then
//! writes `feature:iamf` into its link manifest, and `mradm-islands-sys` hands that to this crate's
//! build script, which sets `cfg(mradm_iamf)`. Without it the output is unsupported.
use mradm_engine::request::Error;
use std::ffi::{c_char, c_int, c_void};

/// Whether this build links the bridge (the `iamf` platform fact).
pub(crate) const AVAILABLE: bool = cfg!(mradm_iamf);

/// `MR_IAMF_AOM_BRIDGE_ABI_VERSION`: the newest bridge ABI this caller fills in.
#[cfg_attr(not(mradm_iamf), allow(dead_code))]
const BRIDGE_ABI: u32 = 3;
#[cfg_attr(not(mradm_iamf), allow(dead_code))]
const RESULT_OK: c_int = 0;
#[cfg_attr(not(mradm_iamf), allow(dead_code))]
const RESULT_CANCELLED: c_int = -2;

/// `MrIamfAomEncodeOptions`. The v2 fields (`struct_size`, the cancel callback) are left zero for
/// a v1 bridge and the v3 field (`scalable_layers_csv`) for older bridges.
#[repr(C)]
#[cfg_attr(not(mradm_iamf), allow(dead_code))]
pub struct IamfEncodeOptions {
    abi_version: u32,
    input_wav_path: *const c_char,
    output_iamf_path: *const c_char,
    layout_id: *const c_char,
    opus_bitrate_per_ch_kbps: u32,
    /// `MrIamfAomProfile`; 0 is `MR_IAMF_AOM_PROFILE_AUTO`.
    profile: c_int,
    has_loudness_lufs: c_int,
    loudness_lufs: f64,
    has_peak_dbtp: c_int,
    peak_dbtp: f64,
    struct_size: usize,
    should_cancel: Option<unsafe extern "C" fn(*mut c_void) -> c_int>,
    cancel_user_data: *mut c_void,
    scalable_layers_csv: *const c_char,
}

#[cfg(mradm_iamf)]
unsafe extern "C" {
    fn mr_iamf_aom_bridge_abi_version() -> u32;
    fn mr_iamf_aom_encode_wav_to_iamf(
        options: *const IamfEncodeOptions,
        error_buffer: *mut c_char,
        error_buffer_size: usize,
    ) -> c_int;
}

/// The request of one IAMF encode (`Island::EncodeIamf`); a build without the bridge reads none of it.
#[cfg_attr(not(mradm_iamf), allow(dead_code))]
pub(crate) struct Request<'a> {
    pub src: &'a str,
    pub dst: &'a str,
    pub layout: &'a str,
    pub bitrate_per_ch_kbps: u32,
    pub lufs: Option<f64>,
    pub peak_dbtp: Option<f64>,
    pub layers: &'a [String],
}

const UNSUPPORTED: i32 = 2;
#[cfg_attr(not(mradm_iamf), allow(dead_code))]
const INVALID_ARGUMENT: i32 = 1;
#[cfg_attr(not(mradm_iamf), allow(dead_code))]
const IO_ERROR: i32 = 3;
#[cfg_attr(not(mradm_iamf), allow(dead_code))]
const CANCELLED: i32 = 5;

/// The bridge's cancel callback: `user` is the encode's `&(dyn Fn() -> bool + Sync)`. The bridge
/// may call it from its own threads; a panic counts as no cancellation.
#[cfg_attr(not(mradm_iamf), allow(dead_code))]
unsafe extern "C" fn should_cancel(user: *mut c_void) -> c_int {
    if user.is_null() {
        return 0;
    }
    let cancelled = unsafe { &*user.cast::<&(dyn Fn() -> bool + Sync)>() };
    std::panic::catch_unwind(std::panic::AssertUnwindSafe(|| c_int::from(cancelled()))).unwrap_or(0)
}

/// A path in the spelling the bridge opens: UTF-8 on POSIX; on Windows the active code page, as
/// the C++ island's `std::filesystem::path::string()` gave it (a path that code page cannot spell
/// fails as that conversion did).
#[cfg(not(windows))]
#[cfg_attr(not(mradm_iamf), allow(dead_code))]
fn native_path(path: &str) -> Result<std::ffi::CString, Error> {
    std::ffi::CString::new(path).map_err(|_| internal())
}

#[cfg(windows)]
#[cfg_attr(not(mradm_iamf), allow(dead_code))]
fn native_path(path: &str) -> Result<std::ffi::CString, Error> {
    unsafe extern "system" {
        fn WideCharToMultiByte(
            code_page: u32,
            flags: u32,
            wide: *const u16,
            wide_len: c_int,
            narrow: *mut u8,
            narrow_len: c_int,
            default_char: *const u8,
            used_default_char: *mut c_int,
        ) -> c_int;
    }
    const CP_ACP: u32 = 0;
    const WC_NO_BEST_FIT_CHARS: u32 = 0x400;
    let wide: Vec<u16> = path.encode_utf16().collect();
    if wide.is_empty() {
        return Ok(std::ffi::CString::default());
    }
    let len = c_int::try_from(wide.len()).map_err(|_| internal())?;
    let convert = |out: *mut u8, out_len: c_int, used: *mut c_int| unsafe {
        WideCharToMultiByte(
            CP_ACP,
            WC_NO_BEST_FIT_CHARS,
            wide.as_ptr(),
            len,
            out,
            out_len,
            std::ptr::null(),
            used,
        )
    };
    let size = convert(std::ptr::null_mut(), 0, std::ptr::null_mut());
    if size <= 0 {
        return Err(internal());
    }
    let mut narrow = vec![0u8; size as usize];
    let mut used_default = 0;
    if convert(narrow.as_mut_ptr(), size, &mut used_default) != size || used_default != 0 {
        return Err(internal());
    }
    std::ffi::CString::new(narrow).map_err(|_| internal())
}

/// The C++ island's answer to an exception.
#[cfg_attr(not(mradm_iamf), allow(dead_code))]
fn internal() -> Error {
    Error::new(6, "render island failed", "")
}

/// `convert_to_iamf`: in order, cancellation, the disabled 9.1.6 layouts, the source (48 kHz), the
/// bridge ABI and the scalable layers it supports; then the bridge encodes, polling `cancelled`.
#[cfg(mradm_iamf)]
pub(crate) fn encode(r: &Request<'_>, cancelled: &(dyn Fn() -> bool + Sync)) -> Result<(), Error> {
    use std::ffi::{CStr, CString};
    let at = |path: &str| format!("path={path}");
    if cancelled() {
        return Err(Error::new(CANCELLED, "render cancelled", at(r.dst)));
    }
    if r.layout == "9.1.6" || r.layout == "atmos916" {
        return Err(Error::new(
            UNSUPPORTED,
            "IAMF 9.1.6 output is disabled: the official AOM bridge requires expanded/Base-Enhanced \
             IAMF for this layout, which is not currently compatible enough for release output",
            "",
        ));
    }
    let reader = std::fs::File::open(r.src)
        .map_err(mradm_wav::Error::from)
        .and_then(|f| mradm_wav::Reader::new(std::io::BufReader::new(f)))
        .map_err(|e| {
            let mut n = e.message.len().min(511);
            while !e.message.is_char_boundary(n) {
                n -= 1;
            }
            let code = if (1..=6).contains(&e.code) { e.code } else { 6 };
            Error::new(code, &e.message[..n], at(r.src))
        })?;
    let rate = reader.info().format.sample_rate;
    drop(reader);
    if rate != 48_000 {
        return Err(Error::new(
            INVALID_ARGUMENT,
            format!("IAMF output requires 48000 Hz; got {rate}"),
            "",
        ));
    }
    let abi = unsafe { mr_iamf_aom_bridge_abi_version() };
    if abi == 0 || abi > BRIDGE_ABI {
        return Err(Error::new(
            IO_ERROR,
            "AOM IAMF bridge ABI version mismatch",
            "",
        ));
    }
    if !r.layers.is_empty() && abi < 3 {
        return Err(Error::new(
            UNSUPPORTED,
            "IAMF scalable layers require AOM bridge ABI v3 or newer",
            "",
        ));
    }
    let src = native_path(r.src)?;
    let dst = native_path(r.dst)?;
    let layout = CString::new(r.layout).map_err(|_| internal())?;
    let layers = CString::new(r.layers.join(",")).map_err(|_| internal())?;
    let mut user: &(dyn Fn() -> bool + Sync) = cancelled;
    let mut options = IamfEncodeOptions {
        abi_version: abi.min(3),
        input_wav_path: src.as_ptr(),
        output_iamf_path: dst.as_ptr(),
        layout_id: layout.as_ptr(),
        opus_bitrate_per_ch_kbps: r.bitrate_per_ch_kbps,
        profile: 0,
        has_loudness_lufs: c_int::from(r.lufs.is_some()),
        loudness_lufs: r.lufs.unwrap_or(0.0),
        has_peak_dbtp: c_int::from(r.peak_dbtp.is_some()),
        peak_dbtp: r.peak_dbtp.unwrap_or(0.0),
        struct_size: 0,
        should_cancel: None,
        cancel_user_data: std::ptr::null_mut(),
        scalable_layers_csv: std::ptr::null(),
    };
    if abi >= 2 {
        options.struct_size = size_of::<IamfEncodeOptions>();
        options.should_cancel = Some(should_cancel);
        options.cancel_user_data = (&raw mut user).cast();
    }
    if abi >= 3 && !r.layers.is_empty() {
        options.scalable_layers_csv = layers.as_ptr();
    }
    let mut error = [0 as c_char; 2048];
    let rc = unsafe { mr_iamf_aom_encode_wav_to_iamf(&options, error.as_mut_ptr(), error.len()) };
    if rc == RESULT_CANCELLED || cancelled() {
        return Err(Error::new(CANCELLED, "render cancelled", at(r.dst)));
    }
    if rc != RESULT_OK {
        error[error.len() - 1] = 0;
        let detail = if error[0] != 0 {
            unsafe { CStr::from_ptr(error.as_ptr()) }
                .to_string_lossy()
                .into_owned()
        } else {
            "unknown bridge error".to_owned()
        };
        return Err(Error::new(
            IO_ERROR,
            format!("AOM IAMF bridge encode failed: {detail}"),
            at(r.dst),
        ));
    }
    Ok(())
}

/// Without the bridge every IAMF output is unsupported (no hand-written OBU writer stands in).
#[cfg(not(mradm_iamf))]
pub(crate) fn encode(_: &Request<'_>, _: &(dyn Fn() -> bool + Sync)) -> Result<(), Error> {
    Err(Error::new(
        UNSUPPORTED,
        "IAMF encoding requires MR_ADM_ENABLE_IAMF=ON and the official AOM iamf-tools bridge",
        "",
    ))
}

#[cfg(test)]
mod tests {
    use super::*;

    fn request<'a>(layout: &'a str, layers: &'a [String]) -> Request<'a> {
        Request {
            src: "missing-input.wav",
            dst: "missing-output.iamf",
            layout,
            bitrate_per_ch_kbps: 0,
            lufs: None,
            peak_dbtp: None,
            layers,
        }
    }

    #[cfg(not(mradm_iamf))]
    #[test]
    fn without_the_bridge_iamf_is_unsupported() {
        const { assert!(!AVAILABLE) };
        // Even a cancelled request reports the missing bridge, as the C++ island did.
        let e = encode(&request("4+7+0", &[]), &|| true).unwrap_err();
        assert_eq!(e.code, UNSUPPORTED);
        assert_eq!(
            e.message,
            "IAMF encoding requires MR_ADM_ENABLE_IAMF=ON and the official AOM iamf-tools bridge"
        );
        assert_eq!(e.context, "");
    }

    /// `iamf_smoke_test.cpp`'s checks of a bridge build, and the source checks before the bridge.
    #[cfg(mradm_iamf)]
    #[test]
    fn the_checks_before_the_bridge_follow_the_cpp_island() {
        const { assert!(AVAILABLE) };
        let e = encode(&request("4+7+0", &[]), &|| true).unwrap_err();
        assert_eq!(
            (e.code, e.message.as_str(), e.context.as_str()),
            (CANCELLED, "render cancelled", "path=missing-output.iamf")
        );
        for layout in ["9.1.6", "atmos916"] {
            let e = encode(&request(layout, &[]), &|| false).unwrap_err();
            assert_eq!(e.code, UNSUPPORTED, "{layout}");
        }
        let e = encode(&request("4+7+0", &[]), &|| false).unwrap_err();
        assert_eq!(
            (e.code, e.context.as_str()),
            (IO_ERROR, "path=missing-input.wav")
        );
        let dir = std::env::temp_dir().join(format!("mradm-iamf-{}", std::process::id()));
        std::fs::create_dir_all(&dir).unwrap();
        let src = dir.join("44k.wav");
        let mut writer = mradm_wav::create_file_writer(&src, 2, 44_100, 32, true, false).unwrap();
        writer.write_frames(&[0.0; 2]).unwrap();
        writer.finish().unwrap();
        let src = src.to_str().unwrap().to_owned();
        let r = Request {
            src: &src,
            ..request("0+2+0", &[])
        };
        let e = encode(&r, &|| false).unwrap_err();
        assert_eq!(
            (e.code, e.message.as_str()),
            (INVALID_ARGUMENT, "IAMF output requires 48000 Hz; got 44100")
        );
        std::fs::remove_dir_all(&dir).unwrap();
    }

    #[test]
    fn the_cancel_callback_reads_the_closure() {
        let yes = || true;
        let no = || false;
        let mut user: &(dyn Fn() -> bool + Sync) = &yes;
        assert_eq!(unsafe { should_cancel((&raw mut user).cast()) }, 1);
        user = &no;
        assert_eq!(unsafe { should_cancel((&raw mut user).cast()) }, 0);
        let panics = || -> bool { panic!("cancel check") };
        user = &panics;
        assert_eq!(unsafe { should_cancel((&raw mut user).cast()) }, 0);
        assert_eq!(unsafe { should_cancel(std::ptr::null_mut()) }, 0);
    }

    #[test]
    fn options_match_the_bridge_layout() {
        // The offsets of MrIamfAomEncodeOptions on LP64 / LLP64 targets (8-byte pointers).
        if size_of::<usize>() == 8 {
            assert_eq!(std::mem::offset_of!(IamfEncodeOptions, input_wav_path), 8);
            assert_eq!(
                std::mem::offset_of!(IamfEncodeOptions, opus_bitrate_per_ch_kbps),
                32
            );
            assert_eq!(std::mem::offset_of!(IamfEncodeOptions, profile), 36);
            assert_eq!(std::mem::offset_of!(IamfEncodeOptions, loudness_lufs), 48);
            assert_eq!(std::mem::offset_of!(IamfEncodeOptions, peak_dbtp), 64);
            assert_eq!(std::mem::offset_of!(IamfEncodeOptions, struct_size), 72);
            assert_eq!(
                std::mem::offset_of!(IamfEncodeOptions, scalable_layers_csv),
                96
            );
            assert_eq!(size_of::<IamfEncodeOptions>(), 104);
        }
    }
}
