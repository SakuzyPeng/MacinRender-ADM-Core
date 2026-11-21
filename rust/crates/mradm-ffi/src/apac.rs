//! APAC encoding runs in the bundled CLI so a stalled AudioToolbox call can be reclaimed.
use mradm_engine::{offline::Progress, request::Error};
use std::io::{self, Write};

#[cfg(target_os = "macos")]
mod encoder;
#[cfg(target_os = "macos")]
mod watchdog;
#[cfg(target_os = "macos")]
mod worker;

#[cfg(target_os = "macos")]
pub(crate) use watchdog::encode;

#[cfg_attr(not(target_os = "macos"), allow(dead_code))]
pub(crate) struct Request<'a> {
    pub src: &'a str,
    pub dst: &'a str,
    pub layout: &'a str,
    pub bitrate: u32,
    pub drc_music: bool,
    pub caf: bool,
}

#[cfg(not(target_os = "macos"))]
fn unsupported() -> Error {
    Error::new(2, "APAC encoding requires macOS (AudioToolbox)", "")
}

#[cfg(not(target_os = "macos"))]
pub(crate) fn encode(
    _: &Request<'_>,
    _: &dyn Fn() -> bool,
    _: &dyn Fn(&Progress),
) -> Result<(), Error> {
    Err(unsupported())
}

/// The hidden CLI command. The parent reconstructs the error context from its output path.
pub fn run_child(
    input: &[u8],
    output: &[u8],
    layout: &[u8],
    bitrate: u32,
    drc_music: bool,
    caf: bool,
) -> i32 {
    let mut out = io::stdout().lock();
    #[cfg(target_os = "macos")]
    let result = worker::run(input, output, layout, bitrate, drc_music, caf, &mut out);
    #[cfg(not(target_os = "macos"))]
    let result = {
        let _ = (input, output, layout, bitrate, drc_music, caf);
        Err::<(), _>(unsupported())
    };
    if let Err(error) = result {
        let _ = writeln!(out, "E {} {}", error.code, error.message);
        let _ = out.flush();
        return 1;
    }
    0
}

/// Test-table entry used by the AudioToolbox decode and progress smoke tests.
pub(crate) unsafe extern "C" fn test_encode(
    src: crate::adm::Text,
    dst: crate::adm::Text,
    layout: crate::adm::Text,
    caf: u32,
    driver: *const crate::engine_service::IslandContext,
    message: *mut u8,
    capacity: usize,
) -> i32 {
    crate::wav::boundary(message, capacity, || {
        let request = Request {
            src: unsafe { crate::wav::path(src.data, src.len)? },
            dst: unsafe { crate::wav::path(dst.data, dst.len)? },
            layout: unsafe { crate::wav::path(layout.data, layout.len)? },
            bitrate: 0,
            drc_music: true,
            caf: caf != 0,
        };
        let driver = unsafe { driver.as_ref() };
        let progress = |p: &Progress| {
            if let Some(d) = driver
                && let Some(callback) = d.progress
            {
                unsafe { callback(d.user, &crate::engine_service::progress_record(p)) };
            }
        };
        encode(&request, &|| false, &progress).map_err(|e| mradm_wav::Error {
            code: e.code,
            message: e.message,
        })
    })
}

#[cfg(all(test, not(target_os = "macos")))]
mod tests {
    use super::*;

    #[test]
    fn encoding_requires_audiotoolbox() {
        let request = Request {
            src: "",
            dst: "",
            layout: "wav71",
            bitrate: 0,
            drc_music: true,
            caf: false,
        };
        assert_eq!(
            encode(&request, &|| true, &|_| {}).unwrap_err(),
            Error::new(2, "APAC encoding requires macOS (AudioToolbox)", "")
        );
    }
}
