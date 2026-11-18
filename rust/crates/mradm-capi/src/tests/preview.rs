//! The preview session (from `c_api_fixture_test`): argument checks, windows that reuse the
//! imported scene and the prepared backend, "to the end" windows, progress v2, and a non-binaural
//! session.
use super::common::*;
use crate::options::*;
use crate::render::*;
use std::ptr;

struct Session(*mut adm_preview_session_t);

impl Drop for Session {
    fn drop(&mut self) {
        unsafe { adm_destroy_preview_session(self.0) };
    }
}

fn window(session: &Session, start: f64, end: f64, output: &std::path::Path) -> (i32, Res) {
    let mut result = ptr::null_mut();
    let code = unsafe {
        adm_preview_render_window(
            session.0,
            start,
            end,
            c(output).as_ptr(),
            None,
            ptr::null_mut(),
            &mut result,
        )
    };
    let result = Res(result);
    if code != OK && !result.0.is_null() {
        eprintln!("window: {code} {}", result.message());
    }
    (code, result)
}

#[test]
fn preview_sessions_reject_bad_arguments() {
    let context = Context::new();
    let dir = Dir::new("preview-errors");
    let input = c(&object_input_1s(&dir));
    let mut bad = ptr::null_mut();
    unsafe {
        assert_eq!(
            adm_create_preview_session(ptr::null_mut(), input.as_ptr(), ptr::null(), &mut bad),
            INVALID_ARGUMENT
        );
        assert_eq!(
            adm_create_preview_session(context.0, ptr::null(), ptr::null(), &mut bad),
            INVALID_ARGUMENT
        );
        assert_eq!(
            adm_create_preview_session(context.0, input.as_ptr(), ptr::null(), ptr::null_mut()),
            INVALID_ARGUMENT
        );
        assert_ne!(
            adm_create_preview_session(
                context.0,
                c"/no/such/adm/file.wav".as_ptr(),
                ptr::null(),
                &mut bad
            ),
            OK
        );
        assert!(bad.is_null());
        let null = ptr::null_mut();
        assert_eq!(
            adm_preview_render_window(
                ptr::null_mut(),
                0.0,
                0.0,
                ptr::null(),
                None,
                null,
                ptr::null_mut()
            ),
            INVALID_ARGUMENT
        );
        adm_destroy_preview_session(ptr::null_mut());
    }
    let mut session = ptr::null_mut();
    assert_eq!(
        unsafe { adm_create_preview_session(context.0, input.as_ptr(), ptr::null(), &mut session) },
        OK
    );
    let session = Session(session);
    let mut result = ptr::null_mut();
    assert_eq!(
        unsafe {
            adm_preview_render_window(
                session.0,
                -1.0,
                0.0,
                ptr::null(),
                None,
                ptr::null_mut(),
                &mut result,
            )
        },
        INVALID_ARGUMENT
    );
    drop(Res(result));
}

/// The default session (binaural) builds the HRTF in the first window only; windows have the
/// requested length, `end <= 0` runs to the end, and the v2 window reports progress.
#[test]
fn preview_windows_reuse_the_prepared_backend() {
    let context = Context::new();
    let dir = Dir::new("preview");
    let input = c(&object_input_1s(&dir));
    let mut session = ptr::null_mut();
    assert_eq!(
        unsafe { adm_create_preview_session(context.0, input.as_ptr(), ptr::null(), &mut session) },
        OK
    );
    assert!(!session.is_null());
    let session = Session(session);
    let a = dir.file("a.wav");
    let (code, result) = window(&session, 0.25, 0.75, &a);
    assert_eq!(code, OK);
    assert!(result.logs_contain("HRTF source:"));
    assert_eq!(wav_shape(&a).1, 24000);
    let b = dir.file("b.wav");
    let (code, result) = window(&session, 0.0, 0.5, &b);
    assert_eq!(code, OK);
    assert!(!result.logs_contain("HRTF source:"));
    assert_eq!(wav_shape(&b).1, 24000);
    let tail = dir.file("tail.wav");
    assert_eq!(window(&session, 0.5, 0.0, &tail).0, OK);
    assert_eq!(wav_shape(&tail).1, 24000);

    let v2 = dir.file("v2.wav");
    let mut events: Vec<Event> = Vec::new();
    let mut result = ptr::null_mut();
    let code = unsafe {
        adm_preview_render_window_v2(
            session.0,
            0.0,
            0.25,
            c(&v2).as_ptr(),
            Some(collect_v2),
            ptr::from_mut(&mut events).cast(),
            &mut result,
        )
    };
    drop(Res(result));
    assert_eq!(code, OK);
    check_v2(&events, true);
    assert_eq!(wav_shape(&v2).1, 12000);
}

/// An EAR 5.1 session round-trips its own prepared state through the same cache.
#[test]
fn speaker_preview_sessions_render_their_layout() {
    let context = Context::new();
    let dir = Dir::new("preview-ear");
    let input = c(&object_input_1s(&dir));
    let options = Options::new();
    unsafe {
        adm_render_options_set_renderer(options.0, 1);
        adm_render_options_set_output_layout(options.0, c"5.1".as_ptr());
    }
    let mut session = ptr::null_mut();
    assert_eq!(
        unsafe { adm_create_preview_session(context.0, input.as_ptr(), options.0, &mut session) },
        OK
    );
    let session = Session(session);
    let output = dir.file("ear.wav");
    assert_eq!(window(&session, 0.25, 0.75, &output).0, OK);
    assert_eq!(wav_shape(&output), (6, 24000));
}
