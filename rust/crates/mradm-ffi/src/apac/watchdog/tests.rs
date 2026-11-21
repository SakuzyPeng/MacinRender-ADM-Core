use super::*;
use std::cell::{Cell, RefCell};
use std::fs;
use std::os::unix::fs::PermissionsExt;

const LONG: Duration = Duration::from_secs(20);
const SHORT: Duration = Duration::from_millis(75);
const BUDGETS: Budgets = Budgets {
    startup: LONG,
    writing: LONG,
    flushing: LONG,
};

struct Fixture(PathBuf);

impl Fixture {
    fn new(name: &str) -> Self {
        let dir = std::env::temp_dir().join(format!("mradm-apac-{}-{name}", std::process::id()));
        fs::create_dir(&dir).unwrap();
        Self(dir)
    }

    fn script(&self, script: &str) -> PathBuf {
        let path = self.0.join("APAC 测试 helper");
        fs::write(&path, format!("#!/bin/sh\n{script}\n")).unwrap();
        fs::set_permissions(&path, fs::Permissions::from_mode(0o700)).unwrap();
        path
    }
}

impl Drop for Fixture {
    fn drop(&mut self) {
        let _ = fs::remove_dir_all(&self.0);
    }
}

fn request(dst: &str) -> Request<'_> {
    Request {
        src: "in put.wav",
        dst,
        layout: "wav71",
        bitrate: 512,
        drc_music: false,
        caf: true,
    }
}

#[test]
fn complete_protocol_lines_select_the_budget_and_forward_progress() {
    let budgets = Budgets {
        startup: LONG,
        writing: SHORT,
        flushing: POLL,
    };
    let mut hb = Heartbeat::new(budgets);
    let events = RefCell::new(Vec::new());
    let progress = |p: &Progress| {
        assert_eq!(
            (p.stage, p.operation, p.message),
            (
                stage::POST_PROCESSING,
                operation::ENCODE_APAC,
                "encoding APAC"
            )
        );
        assert_eq!(p.fraction, p.stage_fraction);
        events.borrow_mut().push((p.current, p.total, p.fraction));
    };
    hb.receive(b"stray\nP nonsense\nP 0 0", budgets, &progress);
    assert_eq!(hb.timeout, LONG);
    for &byte in b"\nP 4 8\nP 10 9\n" {
        hb.receive(&[byte], budgets, &progress);
    }
    assert_eq!(hb.timeout, SHORT);
    assert_eq!(*events.borrow(), [(0, 0, 0.), (4, 8, 0.5), (10, 9, 1.)]);
    hb.receive(&vec![b'x'; 1024 * 1024 + 1], budgets, &progress);
    hb.receive(b"P 1 1\nF\nE 2 error with spaces\n", budgets, &progress);
    assert_eq!(events.borrow().len(), 3);
    assert_eq!(hb.timeout, POLL);
    assert_eq!(
        (hb.error_code, hb.error_message.as_str()),
        (2, "error with spaces")
    );
}

#[test]
fn child_receives_literal_arguments_and_errors_from_either_pipe() {
    let f = Fixture::new("args");
    let dst = f.0.join("out put.caf");
    let req = request(dst.to_str().unwrap());
    let helper = f.script(r#"
[ "$#" = 13 ] && [ "$1" = __apac-encode ] && [ "$2" = --in ] && [ "$3" = 'in put.wav' ] &&
[ "$4" = --out ] && [ "$6" = --layout ] && [ "$7" = wav71 ] && [ "$8" = --bitrate ] &&
[ "$9" = 512 ] && [ "${10}" = --drc ] && [ "${11}" = 0 ] && [ "${12}" = --container ] && [ "${13}" = caf ] || exit 42
printf 'partial' > "$5"
printf 'P 2'
printf ' 7\nE 1 child diagnostic\n' >&2
exit 1"#);
    let event = Cell::new(None);
    let err = run(&req, &helper, BUDGETS, &|| false, &|p| {
        event.set(Some((p.current, p.total)))
    })
    .unwrap_err();
    assert_eq!(event.get(), Some((2, 7)));
    assert_eq!(
        err,
        Error::new(1, "child diagnostic", format!("path={}", req.dst))
    );
    assert_eq!(fs::read(dst).unwrap(), b"partial");
}

#[test]
fn each_watchdog_phase_times_out_without_a_new_heartbeat() {
    let f = Fixture::new("timeouts");
    for (line, budgets) in [
        (
            "",
            Budgets {
                startup: SHORT,
                ..BUDGETS
            },
        ),
        (
            "P 0 10",
            Budgets {
                writing: SHORT,
                ..BUDGETS
            },
        ),
        (
            "F",
            Budgets {
                flushing: SHORT,
                ..BUDGETS
            },
        ),
    ] {
        let helper = f.script(&format!("echo '{line}'; exec /bin/sleep 4"));
        let err = run(&request("out"), &helper, budgets, &|| false, &|_| {}).unwrap_err();
        assert_eq!(
            err,
            Error::new(
                3,
                "APAC 编码在此码率下停滞(编码器无响应),请降低码率后重试",
                "path=out bitrate=512kbps"
            )
        );
    }
}

#[test]
fn cancellation_reclaims_the_process_group_and_pre_cancel_preserves_output() {
    let f = Fixture::new("cancel");
    let dst = f.0.join("out.m4a");
    let req = request(dst.to_str().unwrap());
    fs::write(&dst, b"prior").unwrap();
    assert_eq!(encode(&req, &|| true, &|_| {}).unwrap_err().code, 5);
    assert_eq!(fs::read(&dst).unwrap(), b"prior");

    let helper = f.script("/bin/sleep 10 &\necho $! > \"$5\"\necho 'P 0 1'\nwait");
    let seen = Cell::new(false);
    assert_eq!(
        run(&req, &helper, BUDGETS, &|| seen.get(), &|_| seen.set(true))
            .unwrap_err()
            .code,
        5
    );
    let pid = fs::read_to_string(dst)
        .unwrap()
        .trim()
        .parse::<i32>()
        .unwrap();
    let start = Instant::now();
    while unsafe { libc::kill(pid, 0) } == 0 && start.elapsed() < Duration::from_secs(2) {
        std::thread::sleep(POLL);
    }
    assert_eq!(
        unsafe { libc::kill(pid, 0) },
        -1,
        "helper descendant still alive"
    );
    assert_eq!(io::Error::last_os_error().raw_os_error(), Some(libc::ESRCH));
}
