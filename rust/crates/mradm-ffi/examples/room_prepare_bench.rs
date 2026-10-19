//! Release preparation probe at the private C boundary, with all handles owned
//! here. Cache-cold means no live prepared handle; the OS file cache is retained.
use std::{
    ffi::{CStr, c_void},
    ptr,
    time::Instant,
};
unsafe extern "C" {
    fn mradm_room_profile_load(
        name: *const u8,
        length: usize,
        out: *mut *mut c_void,
        message: *mut u8,
        capacity: usize,
    ) -> i32;
    fn mradm_room_profile_destroy(handle: *mut c_void);
    fn mradm_room_prepare_quality(
        profile: *const c_void,
        hrir: *const u8,
        hrir_length: usize,
        sofa: *const u8,
        sofa_length: usize,
        reuse: *const c_void,
        quality: u32,
        metres: f64,
        out: *mut *mut c_void,
        message: *mut u8,
        capacity: usize,
    ) -> i32;
    fn mradm_room_prepared_destroy(handle: *mut c_void);
    fn mradm_room_prepared_json(handle: *const c_void) -> *const u8;
}
struct Handle {
    pointer: *mut c_void,
    profile: bool,
}
impl Drop for Handle {
    fn drop(&mut self) {
        unsafe {
            if self.profile {
                mradm_room_profile_destroy(self.pointer)
            } else {
                mradm_room_prepared_destroy(self.pointer)
            }
        }
    }
}
fn check(code: i32, message: &[u8]) -> std::io::Result<()> {
    if code == 0 {
        return Ok(());
    }
    let end = message
        .iter()
        .position(|v| *v == 0)
        .unwrap_or(message.len());
    Err(std::io::Error::other(format!(
        "{code}: {}",
        String::from_utf8_lossy(&message[..end])
    )))
}
fn prepare(profile: &Handle, path: &str) -> std::io::Result<Handle> {
    let mut pointer = ptr::null_mut();
    let mut message = [0u8; 1024];
    let code = unsafe {
        mradm_room_prepare_quality(
            profile.pointer,
            path.as_ptr(),
            path.len(),
            ptr::null(),
            0,
            ptr::null(),
            1,
            0.,
            &mut pointer,
            message.as_mut_ptr(),
            message.len(),
        )
    };
    check(code, &message)?;
    assert!(!pointer.is_null());
    Ok(Handle {
        pointer,
        profile: false,
    })
}
fn main() -> std::result::Result<(), Box<dyn std::error::Error>> {
    if cfg!(debug_assertions) {
        return Err("Use Release for preparation timing".into());
    }
    // Keep the rlib on the link line; the declarations above exercise its
    // production exported symbols without exposing private Rust handle types.
    std::hint::black_box(mradm_ffi::mradm_dsp_fft_create as *const ());
    let args: Vec<_> = std::env::args().collect();
    if !(3..=4).contains(&args.len()) {
        return Err("usage: room_prepare_bench ROOM HRIR [REPETITIONS=3]".into());
    }
    let repeats = args
        .get(3)
        .map(|v| v.parse::<usize>())
        .transpose()?
        .unwrap_or(3);
    if repeats == 0 {
        return Err("repetitions must be positive".into());
    }
    let mut pointer = ptr::null_mut();
    let mut message = [0u8; 1024];
    let start = Instant::now();
    let code = unsafe {
        mradm_room_profile_load(
            args[1].as_ptr(),
            args[1].len(),
            &mut pointer,
            message.as_mut_ptr(),
            message.len(),
        )
    };
    check(code, &message)?;
    assert!(!pointer.is_null());
    let profile = Handle {
        pointer,
        profile: true,
    };
    let profile_seconds = start.elapsed().as_secs_f64();
    let mut rows = Vec::new();
    let mut metadata = serde_json::Value::Null;
    for repetition in 1..=repeats {
        let start = Instant::now();
        let first = prepare(&profile, &args[2])?;
        let cold = start.elapsed().as_secs_f64();
        let start = Instant::now();
        let second = prepare(&profile, &args[2])?;
        let hit = start.elapsed().as_secs_f64();
        let text = unsafe { mradm_room_prepared_json(first.pointer) };
        assert!(!text.is_null());
        metadata = serde_json::from_str(unsafe { CStr::from_ptr(text.cast()).to_str()? })?;
        let row = serde_json::json!({"repetition":repetition,"cold_seconds":cold,"active_hit_seconds":hit});
        eprintln!("{row}");
        rows.push(row);
        drop(second);
        drop(first);
    }
    println!(
        "{}",
        serde_json::json!({"schema":"mradm.room-private-prepare-benchmark","version":1,"profile_seconds":profile_seconds,"sample_rate":metadata["sample_rate"],"processing_policy":metadata["processing_policy"],"hrir_hash":metadata["hrir_hash"],"results":rows,"scope":"Release private C boundary; profile load separate, HRIR authentication/preparation and PreparedSet included; no live handle before cold, first handle pinned during hit; OS cache retained"})
    );
    Ok(())
}
