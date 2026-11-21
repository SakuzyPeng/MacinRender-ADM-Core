//! The IAMF-to-MP4 packager (PCM consistency 7.2 C2): finding mp4box (GPAC) or ffmpeg in `PATH`,
//! reading ffmpeg's major version, and running the packager with its output captured, with the
//! rules, arguments and messages of the C++ island it replaces (`iamf_io.cpp`).
//!
//! The packager runs without a shell. Its stdout and stderr share one pipe and at most 4096 bytes
//! of it are kept. Cancellation is polled every 20 ms: on POSIX the packager runs in its own
//! process group, which gets SIGTERM and, 500 ms later, SIGKILL; on Windows the process is
//! terminated.
use mradm_engine::request::{Error, Packager};
use std::ffi::OsStr;
use std::io::Read;
use std::process::{Command, Stdio};
use std::sync::{Arc, Mutex, mpsc};
use std::time::{Duration, Instant};

const UNSUPPORTED: i32 = 2;
const IO_ERROR: i32 = 3;
const CANCELLED: i32 = 5;

const MAX_OUTPUT: usize = 4096;
const TRUNCATED: &str = "...(truncated)";
const POLL: Duration = Duration::from_millis(20);
const TERMINATE_GRACE: Duration = Duration::from_millis(500);
/// How long the output is still read once the packager has exited: a process it left behind can
/// hold the pipe open, and the C++ island read only what was already there.
const FINAL_READ: Duration = Duration::from_millis(500);

/// The packager found in `PATH` and, for ffmpeg, its major version (-1 otherwise).
#[derive(Clone, Debug, PartialEq)]
pub(crate) struct Found {
    pub kind: Packager,
    pub ffmpeg_major: i32,
    executable: String,
}

impl Found {
    fn none() -> Self {
        Self {
            kind: Packager::None,
            ffmpeg_major: -1,
            executable: String::new(),
        }
    }
}

/// What one packager run gave: the exit code (-1 when it did not exit normally), the captured
/// output and whether it was cancelled.
#[derive(Debug, PartialEq)]
struct Run {
    code: i32,
    output: String,
    cancelled: bool,
}

/// `append_process_output`: keeps the first 4096 bytes and marks the cut once.
fn append(output: &mut Vec<u8>, data: &[u8]) {
    if output.len() >= MAX_OUTPUT {
        if !output
            .windows(TRUNCATED.len())
            .any(|w| w == TRUNCATED.as_bytes())
        {
            output.extend_from_slice(TRUNCATED.as_bytes());
        }
        return;
    }
    let remaining = MAX_OUTPUT - output.len();
    output.extend_from_slice(&data[..data.len().min(remaining)]);
    if data.len() > remaining {
        output.extend_from_slice(TRUNCATED.as_bytes());
    }
}

/// The first of `candidates` with an executable file in a directory of `path` (`PATH` when
/// `None`); the name itself is returned, and the run looks it up again as `execvp` did.
#[cfg(unix)]
fn find_in(path: Option<&OsStr>, candidates: &[&str]) -> Option<String> {
    use std::ffi::{CString, c_char, c_int};
    use std::os::unix::ffi::OsStrExt;
    unsafe extern "C" {
        fn access(path: *const c_char, mode: c_int) -> c_int;
    }
    const X_OK: c_int = 1;
    let path = match path {
        Some(p) => p.to_owned(),
        None => std::env::var_os("PATH")?,
    };
    for name in candidates {
        for dir in path.as_bytes().split(|&b| b == b':') {
            if dir.is_empty() {
                continue;
            }
            let mut candidate = dir.to_vec();
            candidate.push(b'/');
            candidate.extend_from_slice(name.as_bytes());
            let Ok(candidate) = CString::new(candidate) else {
                continue;
            };
            if unsafe { access(candidate.as_ptr(), X_OK) } == 0 {
                return Some((*name).to_owned());
            }
        }
    }
    None
}

/// `SearchPathW` with the default search order and the `.exe` extension; `path` is only used by
/// the Unix tests.
#[cfg(windows)]
fn find_in(_path: Option<&OsStr>, candidates: &[&str]) -> Option<String> {
    unsafe extern "system" {
        fn SearchPathW(
            path: *const u16,
            file_name: *const u16,
            extension: *const u16,
            buffer_length: u32,
            buffer: *mut u16,
            file_part: *mut *mut u16,
        ) -> u32;
    }
    const MAX_PATH: usize = 260;
    let wide = |s: &str| s.encode_utf16().chain([0]).collect::<Vec<u16>>();
    let extension = wide(".exe");
    for name in candidates {
        let file = wide(name);
        let mut found = [0u16; MAX_PATH];
        let n = unsafe {
            SearchPathW(
                std::ptr::null(),
                file.as_ptr(),
                extension.as_ptr(),
                MAX_PATH as u32,
                found.as_mut_ptr(),
                std::ptr::null_mut(),
            )
        };
        if n > 0 {
            return Some((*name).to_owned());
        }
    }
    None
}

/// `sscanf(line, "ffmpeg version %d.", &major) == 1 && major > 0`: a format blank matches any
/// run of white space, including none, and `%d` skips white space and takes an optional sign.
fn ffmpeg_major(first_line: &str) -> Option<i32> {
    fn blanks(s: &str) -> &str {
        s.trim_start_matches([' ', '\t', '\n', '\x0b', '\x0c', '\r'])
    }
    let rest = blanks(first_line.strip_prefix("ffmpeg")?);
    let rest = blanks(rest.strip_prefix("version")?);
    let rest = blanks(rest);
    let (negative, digits) = match rest.as_bytes().first() {
        Some(b'-') => (true, &rest[1..]),
        Some(b'+') => (false, &rest[1..]),
        _ => (false, rest),
    };
    let len = digits.bytes().take_while(u8::is_ascii_digit).count();
    if len == 0 {
        return None;
    }
    let value: i32 = digits[..len].parse().ok()?;
    let value = if negative { -value } else { value };
    (value > 0).then_some(value)
}

/// `detect_iamf_mp4_packager`: mp4box (or MP4Box) first, then ffmpeg whose `-version` names a
/// positive major version.
fn detect_in(path: Option<&OsStr>) -> Found {
    if let Some(executable) = find_in(path, &["mp4box", "MP4Box"]) {
        return Found {
            kind: Packager::Mp4box,
            ffmpeg_major: -1,
            executable,
        };
    }
    let Some(executable) = find_in(path, &["ffmpeg"]) else {
        return Found::none();
    };
    let run = run_in(path, &executable, &[OsStr::new("-version")], &|| false);
    if run.code != 0 || run.output.is_empty() {
        return Found::none();
    }
    let first_line = run.output.split('\n').next().unwrap_or_default();
    match ffmpeg_major(first_line) {
        Some(major) => Found {
            kind: Packager::Ffmpeg,
            ffmpeg_major: major,
            executable,
        },
        None => Found::none(),
    }
}

/// The packager in `PATH` (the `ProbeMp4Packager` island).
pub(crate) fn detect() -> Found {
    detect_in(None)
}

/// Whether a packager is in `PATH` (the platform fact of the output-format documents).
pub(crate) fn available() -> bool {
    detect().kind != Packager::None
}

#[cfg(unix)]
fn configure(command: &mut Command) {
    use std::os::unix::process::CommandExt;
    command.process_group(0);
}

#[cfg(windows)]
fn configure(command: &mut Command) {
    use std::os::windows::process::CommandExt;
    const CREATE_NO_WINDOW: u32 = 0x0800_0000;
    command.creation_flags(CREATE_NO_WINDOW);
}

/// Stops a cancelled packager: SIGTERM to its process group first, SIGKILL after the grace
/// period (`kill` falls back to the process when the group is gone); Windows terminates it.
#[cfg(unix)]
fn signal(child: &mut std::process::Child, kill: bool) {
    use std::ffi::c_int;
    unsafe extern "C" {
        #[link_name = "kill"]
        fn kill_process(pid: c_int, signal: c_int) -> c_int;
    }
    let signal = if kill { 9 } else { 15 };
    let pid = child.id() as c_int;
    if unsafe { kill_process(-pid, signal) } != 0 {
        unsafe { kill_process(pid, signal) };
    }
}

#[cfg(windows)]
fn signal(child: &mut std::process::Child, kill: bool) {
    if !kill {
        let _ = child.kill();
    }
}

/// `run_process`: runs `program` with `args`, the child's `PATH` replaced by `path` when given.
fn run_in(
    path: Option<&OsStr>,
    program: &str,
    args: &[&OsStr],
    cancelled: &dyn Fn() -> bool,
) -> Run {
    let failed = |output: &str| Run {
        code: -1,
        output: output.to_owned(),
        cancelled: false,
    };
    if cancelled() {
        return Run {
            code: -1,
            output: "cancelled".into(),
            cancelled: true,
        };
    }
    let Ok((mut reader, writer)) = std::io::pipe() else {
        return failed("pipe() failed");
    };
    let Ok(error_writer) = writer.try_clone() else {
        return failed("pipe() failed");
    };
    let mut command = Command::new(program);
    command
        .args(args)
        .stdout(Stdio::from(writer))
        .stderr(Stdio::from(error_writer));
    if let Some(path) = path {
        command.env("PATH", path);
    }
    configure(&mut command);
    let mut child = match command.spawn() {
        Ok(child) => child,
        // The C++ child exited with 127 when exec failed.
        Err(_) => {
            return Run {
                code: 127,
                output: String::new(),
                cancelled: false,
            };
        }
    };
    // `command` holds the parent's copies of the write end: drop them so the read reaches EOF.
    drop(command);

    let output = Arc::new(Mutex::new(Vec::new()));
    let (done, finished) = mpsc::channel();
    let sink = Arc::clone(&output);
    std::thread::spawn(move || {
        let mut buffer = [0u8; 512];
        loop {
            match reader.read(&mut buffer) {
                Ok(0) => break,
                Ok(n) => append(
                    &mut sink.lock().unwrap_or_else(|e| e.into_inner()),
                    &buffer[..n],
                ),
                Err(e) if e.kind() == std::io::ErrorKind::Interrupted => continue,
                Err(_) => break,
            }
        }
        let _ = done.send(());
    });

    let mut is_cancelled = false;
    let mut killed = false;
    let mut deadline = None;
    let code = loop {
        match child.try_wait() {
            Ok(Some(status)) => break status.code().unwrap_or(-1),
            Ok(None) => {}
            Err(_) => break -1,
        }
        if !is_cancelled && cancelled() {
            is_cancelled = true;
            deadline = Some(Instant::now() + TERMINATE_GRACE);
            signal(&mut child, false);
        } else if is_cancelled && !killed && deadline.is_some_and(|d| Instant::now() >= d) {
            killed = true;
            signal(&mut child, true);
        }
        std::thread::sleep(POLL);
    };
    let _ = finished.recv_timeout(FINAL_READ);
    let bytes = output.lock().unwrap_or_else(|e| e.into_inner()).clone();
    Run {
        code,
        output: String::from_utf8_lossy(&bytes).into_owned(),
        cancelled: is_cancelled,
    }
}

fn package_in(
    path: Option<&OsStr>,
    src: &str,
    dst: &str,
    cancelled: &dyn Fn() -> bool,
) -> Result<(), Error> {
    let cancel = || Error::new(CANCELLED, "render cancelled", format!("path={dst}"));
    if cancelled() {
        return Err(cancel());
    }
    let found = detect_in(path);
    let (name, args): (&str, Vec<&OsStr>) = match found.kind {
        Packager::Mp4box => (
            "mp4box",
            vec!["-add".as_ref(), src.as_ref(), "-new".as_ref(), dst.as_ref()],
        ),
        Packager::Ffmpeg => (
            "ffmpeg",
            vec![
                "-y".as_ref(),
                "-i".as_ref(),
                src.as_ref(),
                "-c".as_ref(),
                "copy".as_ref(),
                dst.as_ref(),
            ],
        ),
        Packager::None => {
            return Err(Error::new(
                UNSUPPORTED,
                "IAMF-to-MP4 packaging requires mp4box (GPAC) or ffmpeg in PATH; install GPAC: https://gpac.io",
                "",
            ));
        }
    };
    let run = run_in(path, &found.executable, &args, cancelled);
    if run.cancelled {
        return Err(cancel());
    }
    if run.code != 0 {
        let message = run.output.trim_end_matches('\n');
        return Err(Error::new(
            IO_ERROR,
            format!("{name} packaging failed: {message}"),
            "",
        ));
    }
    Ok(())
}

/// `package_iamf_to_mp4` (the `PackageMp4` island): packages the IAMF stream `src` into the MP4
/// `dst` with the packager found in `PATH`.
pub(crate) fn package(src: &str, dst: &str, cancelled: &dyn Fn() -> bool) -> Result<(), Error> {
    package_in(None, src, dst, cancelled)
}

#[cfg(all(test, unix))]
mod tests {
    use super::*;
    use std::ffi::OsString;
    use std::fs;
    use std::os::unix::fs::PermissionsExt;
    use std::path::{Path, PathBuf};

    fn scratch(name: &str) -> PathBuf {
        let dir =
            std::env::temp_dir().join(format!("mradm-packager-{}-{name}", std::process::id()));
        let _ = fs::remove_dir_all(&dir);
        fs::create_dir_all(&dir).unwrap();
        dir
    }

    fn tool(dir: &Path, name: &str, script: &str, executable: bool) {
        let path = dir.join(name);
        fs::write(&path, format!("#!/bin/sh\n{script}\n")).unwrap();
        let mode = if executable { 0o755 } else { 0o644 };
        fs::set_permissions(&path, fs::Permissions::from_mode(mode)).unwrap();
    }

    /// `dirs`, then a directory with only the utilities the scripts call, so a packager installed
    /// on the machine is never found.
    fn path_of(dirs: &[&Path]) -> OsString {
        static UTILITIES: std::sync::OnceLock<PathBuf> = std::sync::OnceLock::new();
        let utilities = UTILITIES.get_or_init(|| {
            let dir = scratch("utilities");
            for name in ["head", "tr", "sleep", "touch"] {
                let target = ["/usr/bin", "/bin"]
                    .iter()
                    .map(|d| Path::new(d).join(name))
                    .find(|p| p.exists())
                    .unwrap();
                std::os::unix::fs::symlink(target, dir.join(name)).unwrap();
            }
            dir
        });
        let mut parts: Vec<String> = dirs.iter().map(|d| d.display().to_string()).collect();
        parts.push(utilities.display().to_string());
        OsString::from(parts.join(":"))
    }

    #[test]
    fn ffmpeg_versions_follow_sscanf() {
        let cases = [
            ("ffmpeg version 7.1.1 Copyright (c) 2000-2025", Some(7)),
            ("ffmpeg version 6.0", Some(6)),
            ("ffmpeg version  12", Some(12)),
            ("ffmpegversion8.0", Some(8)),
            ("ffmpeg \t version \t +5.1", Some(5)),
            ("ffmpeg version n7.0", None),
            ("ffmpeg version N-112233-gabc", None),
            ("ffmpeg version 0.9", None),
            ("ffmpeg version -3.1", None),
            ("ffmpeg version ", None),
            ("ffprobe version 7.0", None),
            (" ffmpeg version 7.0", None),
            ("ffmpeg Version 7.0", None),
        ];
        for (line, want) in cases {
            assert_eq!(ffmpeg_major(line), want, "{line:?}");
        }
    }

    #[test]
    fn output_keeps_4096_bytes_and_marks_the_cut_once() {
        let mut out = Vec::new();
        append(&mut out, &[b'a'; 4000]);
        append(&mut out, &[b'b'; 200]);
        assert_eq!(out.len(), MAX_OUTPUT + TRUNCATED.len());
        assert!(out.ends_with(TRUNCATED.as_bytes()));
        append(&mut out, b"more");
        assert_eq!(out.len(), MAX_OUTPUT + TRUNCATED.len());
        let mut exact = Vec::new();
        append(&mut exact, &[b'c'; MAX_OUTPUT]);
        assert_eq!(exact.len(), MAX_OUTPUT);
        append(&mut exact, b"x");
        assert!(exact.ends_with(TRUNCATED.as_bytes()));
    }

    #[test]
    fn detection_prefers_mp4box_and_reads_the_ffmpeg_version() {
        let mp4box = scratch("find-mp4box");
        let upper = scratch("find-upper");
        let ffmpeg = scratch("find-ffmpeg");
        let plain = scratch("find-plain");
        tool(&mp4box, "mp4box", "exit 0", true);
        tool(&upper, "MP4Box", "exit 0", true);
        tool(
            &ffmpeg,
            "ffmpeg",
            "echo 'ffmpeg version 7.1 Copyright'; echo second",
            true,
        );
        tool(&plain, "mp4box", "exit 0", false);
        tool(&plain, "ffmpeg", "exit 0", false);

        let found = detect_in(Some(&path_of(&[&ffmpeg, &mp4box])));
        assert_eq!(
            (found.kind, found.ffmpeg_major, found.executable.as_str()),
            (Packager::Mp4box, -1, "mp4box")
        );
        // "mp4box" is looked up in every directory before "MP4Box".
        let found = detect_in(Some(&path_of(&[&upper, &mp4box])));
        assert_eq!(found.executable, "mp4box");
        let found = detect_in(Some(&path_of(&[&upper])));
        assert_eq!(
            (found.kind, found.executable.as_str()),
            (Packager::Mp4box, "MP4Box")
        );
        let found = detect_in(Some(&path_of(&[&plain, &ffmpeg])));
        assert_eq!((found.kind, found.ffmpeg_major), (Packager::Ffmpeg, 7));
        // Empty entries are skipped; files that are not executable do not count.
        let path = OsString::from(format!("::{}:", plain.display()));
        assert_eq!(detect_in(Some(&path)), Found::none());
        assert_eq!(find_in(Some(OsStr::new("")), &["sh"]), None);
    }

    #[test]
    fn an_unusable_ffmpeg_is_no_packager() {
        let dir = scratch("bad-ffmpeg");
        for script in [
            "echo 'ffmpeg version 7.0'; exit 1",
            "exit 0",
            "echo 'ffmpeg version n7.0'",
            "echo 'ffmpeg version 0.1'",
        ] {
            tool(&dir, "ffmpeg", script, true);
            assert_eq!(
                detect_in(Some(&path_of(&[&dir]))),
                Found::none(),
                "{script}"
            );
        }
        // The version on stderr counts: both streams share the pipe.
        tool(&dir, "ffmpeg", "echo 'ffmpeg version 8.0' >&2", true);
        assert_eq!(detect_in(Some(&path_of(&[&dir]))).ffmpeg_major, 8);
    }

    #[test]
    fn runs_capture_merged_output_and_exit_codes() {
        let dir = scratch("run");
        tool(&dir, "talk", "echo out; echo err >&2; exit 3", true);
        let run = run_in(Some(&path_of(&[&dir])), "talk", &[], &|| false);
        assert_eq!(
            (run.code, run.output.as_str(), run.cancelled),
            (3, "out\nerr\n", false)
        );
        tool(&dir, "flood", "head -c 10000 /dev/zero | tr '\\0' x", true);
        let run = run_in(Some(&path_of(&[&dir])), "flood", &[], &|| false);
        assert_eq!(run.output, format!("{}{TRUNCATED}", "x".repeat(MAX_OUTPUT)));
        let run = run_in(Some(&path_of(&[&dir])), "missing-tool", &[], &|| false);
        assert_eq!((run.code, run.output.as_str()), (127, ""));
        tool(&dir, "signalled", "kill -9 $$", true);
        assert_eq!(
            run_in(Some(&path_of(&[&dir])), "signalled", &[], &|| false).code,
            -1
        );
        let run = run_in(Some(&path_of(&[&dir])), "talk", &[], &|| true);
        assert_eq!(
            (run.code, run.output.as_str(), run.cancelled),
            (-1, "cancelled", true)
        );
    }

    #[test]
    fn cancellation_stops_the_process_group() {
        let dir = scratch("cancel");
        let marker = dir.join("grandchild-alive");
        // The packager ignores SIGTERM and leaves a grandchild that would outlive it.
        tool(
            &dir,
            "stubborn",
            &format!(
                "trap '' TERM; (sleep 3; touch {}) & sleep 30",
                marker.display()
            ),
            true,
        );
        let start = Instant::now();
        let polls = std::cell::Cell::new(0);
        let cancelled = || {
            polls.set(polls.get() + 1);
            polls.get() > 5
        };
        let run = run_in(Some(&path_of(&[&dir])), "stubborn", &[], &cancelled);
        let elapsed = start.elapsed();
        assert!(run.cancelled);
        assert_eq!(run.code, -1, "killed by a signal");
        assert!(
            elapsed >= TERMINATE_GRACE,
            "SIGTERM was ignored: {elapsed:?}"
        );
        assert!(
            elapsed < Duration::from_secs(3),
            "SIGKILL ended it: {elapsed:?}"
        );
        std::thread::sleep(Duration::from_millis(3500));
        assert!(!marker.exists(), "the grandchild was killed with its group");
    }

    #[test]
    fn packaging_runs_the_found_tool_with_its_arguments() {
        let mp4box = scratch("pack-mp4box");
        let log = mp4box.join("args");
        tool(
            &mp4box,
            "mp4box",
            &format!("printf '%s|' \"$@\" > {}", log.display()),
            true,
        );
        package_in(
            Some(&path_of(&[&mp4box])),
            "in put.iamf",
            "out.mp4",
            &|| false,
        )
        .unwrap();
        assert_eq!(
            fs::read_to_string(&log).unwrap(),
            "-add|in put.iamf|-new|out.mp4|"
        );

        let ffmpeg = scratch("pack-ffmpeg");
        let log = ffmpeg.join("args");
        tool(
            &ffmpeg,
            "ffmpeg",
            &format!(
                "if [ \"$1\" = -version ]; then echo 'ffmpeg version 7.0'; exit 0; fi; printf '%s|' \"$@\" > {}",
                log.display()
            ),
            true,
        );
        package_in(Some(&path_of(&[&ffmpeg])), "a.iamf", "b.mp4", &|| false).unwrap();
        assert_eq!(
            fs::read_to_string(&log).unwrap(),
            "-y|-i|a.iamf|-c|copy|b.mp4|"
        );
    }

    #[test]
    fn packaging_failures_carry_the_tool_output() {
        let dir = scratch("pack-fail");
        tool(
            &dir,
            "mp4box",
            "printf 'bad input\\r\\n\\n\\n'; exit 2",
            true,
        );
        let e = package_in(Some(&path_of(&[&dir])), "a.iamf", "b.mp4", &|| false).unwrap_err();
        assert_eq!(
            (e.code, e.message.as_str(), e.context.as_str()),
            (IO_ERROR, "mp4box packaging failed: bad input\r", "")
        );

        let none = scratch("pack-none");
        let e = package_in(Some(&path_of(&[&none])), "a.iamf", "b.mp4", &|| false).unwrap_err();
        assert_eq!(e.code, UNSUPPORTED);
        assert_eq!(
            e.message,
            "IAMF-to-MP4 packaging requires mp4box (GPAC) or ffmpeg in PATH; install GPAC: https://gpac.io"
        );
        let e = package_in(Some(&path_of(&[&dir])), "a.iamf", "b.mp4", &|| true).unwrap_err();
        assert_eq!(
            (e.code, e.message.as_str(), e.context.as_str()),
            (CANCELLED, "render cancelled", "path=b.mp4")
        );

        tool(&dir, "mp4box", "sleep 30", true);
        let polls = std::cell::Cell::new(0);
        let cancelled = || {
            polls.set(polls.get() + 1);
            polls.get() > 2
        };
        let e = package_in(Some(&path_of(&[&dir])), "a.iamf", "b.mp4", &cancelled).unwrap_err();
        assert_eq!((e.code, e.context.as_str()), (CANCELLED, "path=b.mp4"));
    }
}
