use super::{Error, Progress, Request};
use crate::unix_process::{set_nonblocking, signal_group};
use mradm_engine::render::{operation, stage};
use std::ffi::{CStr, CString, OsStr};
use std::io::{self, PipeReader, Read};
use std::os::fd::AsRawFd;
use std::os::unix::{ffi::OsStrExt, process::CommandExt};
use std::path::{Path, PathBuf};
use std::process::Command;
use std::time::{Duration, Instant};

const POLL: Duration = Duration::from_millis(50);
const CHILD_FAILED: &str = "APAC 子进程编码失败";

#[derive(Clone, Copy)]
struct Budgets {
    startup: Duration,
    writing: Duration,
    flushing: Duration,
}

impl Budgets {
    fn from_env() -> Self {
        fn budget(name: &str, seconds: u64) -> Duration {
            let value = std::env::var_os(name).and_then(|s| CString::new(s.as_bytes()).ok());
            // strtol historically accepts leading space, a positive prefix and trailing text.
            let ms = value.map_or(0, |s| unsafe {
                libc::strtol(s.as_ptr(), std::ptr::null_mut(), 10)
            });
            if ms > 0 {
                Duration::from_millis(ms as u64)
            } else {
                Duration::from_secs(seconds)
            }
        }
        Self {
            startup: budget("MRADM_APAC_STARTUP_MS", 30),
            writing: budget("MRADM_APAC_WRITE_STALL_MS", 10),
            flushing: budget("MRADM_APAC_FLUSH_BUDGET_MS", 15),
        }
    }
}

fn executable(path: &Path) -> bool {
    CString::new(path.as_os_str().as_bytes())
        .is_ok_and(|p| unsafe { libc::access(p.as_ptr(), libc::X_OK) } == 0)
}

/// The image containing this code can be a CLI, a static host, or the C ABI dylib.
fn helper() -> Option<PathBuf> {
    let override_path = std::env::var_os("MRADM_APAC_HELPER").map(PathBuf::from);
    if let Some(path) = override_path.filter(|p| executable(p)) {
        // execv resolved a bare override against the working directory. Command would search PATH.
        return std::path::absolute(path).ok();
    }
    let mut info = std::mem::MaybeUninit::uninit();
    if unsafe { libc::dladdr(encode as *const () as *const _, info.as_mut_ptr()) } != 0 {
        let name = unsafe { info.assume_init().dli_fname };
        if !name.is_null() {
            let image = Path::new(OsStr::from_bytes(
                unsafe { CStr::from_ptr(name) }.to_bytes(),
            ));
            let sibling = image.with_file_name("mradm");
            if executable(&sibling) {
                return std::path::absolute(sibling).ok();
            }
        }
    }
    std::env::current_exe()
        .ok()
        .filter(|p| p.file_name() == Some(OsStr::new("mradm")) && executable(p))
}

struct Heartbeat {
    since: Instant,
    timeout: Duration,
    error_code: i32,
    error_message: String,
    pending: Vec<u8>,
    skipping_line: bool,
}

impl Heartbeat {
    fn new(budgets: Budgets) -> Self {
        Self {
            since: Instant::now(),
            timeout: budgets.startup,
            error_code: 3,
            error_message: String::new(),
            pending: Vec::new(),
            skipping_line: false,
        }
    }

    fn receive(&mut self, bytes: &[u8], budgets: Budgets, progress: &dyn Fn(&Progress)) {
        // A valid worker line fits in one exec argument plus a short prefix. Bound stray logs
        // even when a helper continuously writes without a newline.
        const MAX_LINE: usize = 1024 * 1024;
        for piece in bytes.split_inclusive(|b| *b == b'\n') {
            let line_end = piece.last() == Some(&b'\n');
            if !self.skipping_line {
                if self.pending.len() + piece.len() > MAX_LINE {
                    self.skipping_line = true;
                } else {
                    self.pending.extend_from_slice(piece);
                    if line_end {
                        self.line(budgets, progress);
                    }
                }
            }
            if line_end {
                self.pending.clear();
                self.skipping_line = false;
            }
        }
    }

    fn line(&mut self, budgets: Budgets, progress: &dyn Fn(&Progress)) {
        // Replace the newline in place so scanf can read the same numeric prefixes as the C++ worker.
        let last = self.pending.len() - 1;
        self.pending[last] = 0;
        let line = &self.pending[..last];
        if line.starts_with(b"P ") {
            let (mut current, mut total) = (0u64, 0u64);
            let count = unsafe {
                libc::sscanf(
                    self.pending.as_ptr().cast(),
                    c"P %llu %llu".as_ptr(),
                    &raw mut current,
                    &raw mut total,
                )
            };
            if count != 2 {
                return;
            }
            self.since = Instant::now();
            self.timeout = budgets.writing;
            let fraction = if total == 0 {
                0.
            } else {
                (current as f64 / total as f64).min(1.)
            };
            progress(&Progress {
                stage: stage::POST_PROCESSING,
                operation: operation::ENCODE_APAC,
                fraction,
                stage_fraction: fraction,
                current,
                total,
                message: "encoding APAC",
            });
        } else if line == b"F" {
            self.since = Instant::now();
            self.timeout = budgets.flushing;
        } else if let Some(fields) = line.strip_prefix(b"E ") {
            let split = fields.iter().position(|&b| b == b' ');
            self.error_code = 3;
            unsafe {
                libc::sscanf(
                    self.pending.as_ptr().cast(),
                    c"E %d".as_ptr(),
                    &raw mut self.error_code,
                )
            };
            self.error_message = split.map_or_else(String::new, |i| {
                String::from_utf8_lossy(&fields[i + 1..]).into_owned()
            });
        }
    }

    fn drain(
        &mut self,
        pipe: &mut PipeReader,
        limit: usize,
        budgets: Budgets,
        progress: &dyn Fn(&Progress),
    ) {
        let mut buffer = [0; 8192];
        let mut remaining = limit;
        while remaining != 0 {
            let count = remaining.min(buffer.len());
            match pipe.read(&mut buffer[..count]) {
                Ok(0) => break,
                Ok(n) => {
                    self.receive(&buffer[..n], budgets, progress);
                    remaining -= n;
                }
                Err(e) if e.kind() == io::ErrorKind::Interrupted => continue,
                Err(_) => break,
            }
        }
    }

    fn drain_tail(
        &mut self,
        reader: &mut PipeReader,
        budgets: Budgets,
        progress: &dyn Fn(&Progress),
    ) {
        // Snapshot the buffered tail. A descendant may still hold or write to the pipe.
        let mut pending: libc::c_int = 0;
        if unsafe { libc::ioctl(reader.as_raw_fd(), libc::FIONREAD, &raw mut pending) } == 0 {
            self.drain(reader, pending.max(0) as usize, budgets, progress);
        }
    }
}

fn run(
    request: &Request<'_>,
    helper: &Path,
    budgets: Budgets,
    cancelled: &dyn Fn() -> bool,
    progress: &dyn Fn(&Progress),
) -> Result<(), Error> {
    let failure = |code, message| Error::new(code, message, format!("path={}", request.dst));
    let pipe_error = |_| failure(3, "APAC: pipe() failed");
    let (mut reader, writer) = io::pipe().map_err(pipe_error)?;
    set_nonblocking(&reader).map_err(pipe_error)?;
    let stderr = writer.try_clone().map_err(pipe_error)?;
    let mut child = Command::new(helper)
        .args([
            "__apac-encode",
            "--in",
            request.src,
            "--out",
            request.dst,
            "--layout",
            request.layout,
            "--bitrate",
            &request.bitrate.to_string(),
            "--drc",
            if request.drc_music { "1" } else { "0" },
            "--container",
            if request.caf { "caf" } else { "mpeg4" },
        ])
        .process_group(0)
        .stdout(writer)
        .stderr(stderr)
        .spawn()
        .map_err(|e| {
            failure(
                3,
                if matches!(e.raw_os_error(), Some(libc::EAGAIN | libc::ENOMEM)) {
                    "APAC: fork() failed"
                } else {
                    CHILD_FAILED
                },
            )
        })?;
    let mut heartbeat = Heartbeat::new(budgets);
    loop {
        heartbeat.drain(&mut reader, 128 * 1024, budgets, progress);
        match child.try_wait() {
            Ok(Some(status)) => {
                heartbeat.drain_tail(&mut reader, budgets, progress);
                if status.success() {
                    return Ok(());
                }
                let message = if heartbeat.error_message.is_empty() {
                    CHILD_FAILED
                } else {
                    &heartbeat.error_message
                };
                return Err(failure(heartbeat.error_code, message));
            }
            Err(e) if e.kind() != io::ErrorKind::Interrupted => {
                return Err(failure(3, CHILD_FAILED));
            }
            _ => {}
        }
        let cancel = cancelled();
        if cancel || heartbeat.since.elapsed() > heartbeat.timeout {
            signal_group(&child, libc::SIGKILL);
            let _ = child.wait();
            heartbeat.drain_tail(&mut reader, budgets, progress);
            if cancel {
                return Err(failure(5, "render cancelled"));
            }
            return Err(Error::new(
                3,
                "APAC 编码在此码率下停滞(编码器无响应),请降低码率后重试",
                format!("path={} bitrate={}kbps", request.dst, request.bitrate),
            ));
        }
        std::thread::sleep(POLL);
    }
}

pub(crate) fn encode(
    request: &Request<'_>,
    cancelled: &dyn Fn() -> bool,
    progress: &dyn Fn(&Progress),
) -> Result<(), Error> {
    if cancelled() {
        return Err(Error::new(
            5,
            "render cancelled",
            format!("path={}", request.dst),
        ));
    }
    let helper = helper().ok_or_else(|| {
        Error::new(
            2,
            "APAC 编码需要随附的 mradm helper:请与库放在同一目录,或设置 MRADM_APAC_HELPER",
            format!("path={}", request.dst),
        )
    })?;
    let result = run(request, &helper, Budgets::from_env(), cancelled, progress);
    if result.is_err() {
        let _ = std::fs::remove_file(request.dst);
    }
    result
}

#[cfg(test)]
mod tests;
