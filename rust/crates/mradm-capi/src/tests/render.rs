//! Offline renders through `adm_render_file*` to WAV (from `c_api_fixture_test`): error paths and
//! results, progress v1 / v2, logs, layouts, the DirectSpeakers matrix, loudness and final gain,
//! trim, cancellation, in-memory semantic policies, Triple Balance D and the final FLAC.
use super::common::*;
use crate::cancel::*;
use crate::options::*;
use crate::query::adm_render_stage_from_string;
use crate::render::*;
use std::ffi::{CString, c_char, c_void};
use std::path::Path;
use std::ptr;

const MUTE_POLICY: &std::ffi::CStr =
    c"{\"schema\":\"mradm.semantic-policy.v1\",\"global\":{\"gain\":{\"mute\":true}}}";

#[test]
fn bad_arguments_and_missing_inputs_fail_with_a_result() {
    let context = Context::new();
    let dir = Dir::new("render-errors");
    unsafe {
        assert_eq!(adm_render_result_error_code(ptr::null()), INVALID_ARGUMENT);
        assert!(!adm_render_result_message(ptr::null()).is_null());
        let none = ptr::null_mut();
        let null = ptr::null();
        assert_eq!(
            adm_render_file(
                ptr::null_mut(),
                c"x.wav".as_ptr(),
                null,
                None,
                none,
                ptr::null_mut()
            ),
            INVALID_ARGUMENT
        );
        assert_eq!(
            adm_render_file(context.0, null, null, None, none, ptr::null_mut()),
            INVALID_ARGUMENT
        );
        assert_eq!(
            adm_render_file(context.0, c"".as_ptr(), null, None, none, ptr::null_mut()),
            INVALID_ARGUMENT
        );
    }
    let missing = dir.file("does-not-exist.wav");
    let (code, result) = render(&context, &missing, &dir.file("out.wav"), None);
    assert_ne!(code, OK);
    assert!(!result.0.is_null());
    assert_eq!(result.code(), code);
    assert!(!result.message().is_empty());
}

#[derive(Default)]
struct V1 {
    events: Vec<(f64, String, bool)>,
}

unsafe extern "C" fn collect_v1(
    fraction: f64,
    stage: *const c_char,
    message: *const c_char,
    user: *mut c_void,
) {
    let state = unsafe { &mut *user.cast::<V1>() };
    let stage = unsafe { text(stage) }.unwrap_or_default();
    state.events.push((fraction, stage, !message.is_null()));
}

/// A WAV render succeeds with a stable result message, a v1 progress that walks the known stages
/// to "finished", and an output of the same size when no result is requested.
#[test]
fn wav_renders_report_progress_and_a_stable_result() {
    let context = Context::new();
    let dir = Dir::new("render-ok");
    let input = object_input(&dir, 48000, false);
    let output = dir.file("out.wav");
    let mut state = V1::default();
    let mut result = ptr::null_mut();
    let code = unsafe {
        adm_render_file(
            context.0,
            native_c_path(&input).as_ptr(),
            native_c_path(&output).as_ptr(),
            Some(collect_v1),
            ptr::from_mut(&mut state).cast(),
            &mut result,
        )
    };
    let result = Res(result);
    assert_eq!((code, result.code()), (OK, OK));
    let message = unsafe { adm_render_result_message(result.0) };
    assert!(!message.is_null() && message == unsafe { adm_render_result_message(result.0) });
    assert_eq!(
        result.output_path(),
        Some(native_c_path(&output).as_c_str())
    );
    assert!(std::fs::metadata(&output).unwrap().len() > 0);
    let known = [
        "validating",
        "probing",
        "importing_scene",
        "planning",
        "rendering",
        "post_processing",
        "finished",
    ];
    let mut previous = -1.0;
    for (fraction, stage, message) in &state.events {
        assert!(*fraction >= previous && (0.0..=1.0).contains(fraction));
        assert!(known.contains(&stage.as_str()) && *message);
        let cs = CString::new(stage.as_str()).unwrap();
        assert_ne!(
            unsafe { adm_render_stage_from_string(cs.as_ptr()) },
            STAGE_UNKNOWN
        );
        previous = *fraction;
    }
    let (fraction, stage, _) = state.events.last().unwrap();
    assert!(stage == "finished" && *fraction >= 0.99);
    let first = std::fs::read(&output).unwrap().len();

    // No result handle and no callback: the same render, nothing allocated.
    assert_eq!(
        unsafe {
            adm_render_file(
                context.0,
                native_c_path(&input).as_ptr(),
                native_c_path(&output).as_ptr(),
                None,
                ptr::null_mut(),
                ptr::null_mut(),
            )
        },
        OK
    );
    assert_eq!(std::fs::read(&output).unwrap().len(), first);
    // adm_render_file_ex with NULL options renders the same as adm_render_file.
    let ex = dir.file("ex.wav");
    let (code, result) = render(&context, &input, &ex, None);
    assert_eq!((code, result.code()), (OK, OK));
    assert_eq!(std::fs::read(&ex).unwrap().len(), first);
}

#[test]
fn progress_v2_walks_to_finish_through_post_processing() {
    let context = Context::new();
    let dir = Dir::new("render-v2");
    let input = object_input(&dir, 48000, false);
    let none = unsafe {
        adm_render_file_ex2(
            context.0,
            native_c_path(&input).as_ptr(),
            native_c_path(&dir.file("null.wav")).as_ptr(),
            ptr::null(),
            None,
            ptr::null_mut(),
            ptr::null_mut(),
        )
    };
    assert_eq!(none, OK);
    let mut events: Vec<Event> = Vec::new();
    let code = unsafe {
        adm_render_file_ex2(
            context.0,
            native_c_path(&input).as_ptr(),
            native_c_path(&dir.file("v2.wav")).as_ptr(),
            ptr::null(),
            Some(collect_v2),
            ptr::from_mut(&mut events).cast(),
            ptr::null_mut(),
        )
    };
    assert_eq!(code, OK);
    check_v2(&events, true);
}

/// Every captured log line has a module, a message and a known level; one comes from the engine;
/// the pointers are stable and out-of-range or NULL lookups answer 0.
#[test]
fn result_logs_are_owned_by_the_result() {
    let context = Context::new();
    let dir = Dir::new("render-logs");
    let input = object_input(&dir, 48000, false);
    let (code, result) = render(&context, &input, &dir.file("out.wav"), None);
    assert_eq!(code, OK);
    let logs = result.logs();
    assert!(!logs.is_empty());
    assert!(logs.iter().all(|l| (0..=3).contains(&l.level)));
    assert!(logs.iter().any(|l| l.module == "engine"));
    let count = logs.len() as u32;
    let (level, text) = (ptr::null_mut(), ptr::null_mut());
    unsafe {
        assert_eq!(
            adm_render_result_log_entry(result.0, count, level, text, text),
            0
        );
        assert_eq!(
            adm_render_result_log_entry(result.0, 0, level, text, text),
            1
        );
        let (mut a, mut b) = (ptr::null(), ptr::null());
        adm_render_result_log_entry(result.0, 0, level, text, &mut a);
        adm_render_result_log_entry(result.0, 0, level, text, &mut b);
        assert!(!a.is_null() && a == b);
        assert_eq!(adm_render_result_log_count(ptr::null()), 0);
        assert_eq!(
            adm_render_result_log_entry(ptr::null(), 0, level, text, text),
            0
        );
    }
}

#[test]
fn layouts_set_the_channel_count() {
    let context = Context::new();
    let dir = Dir::new("render-layouts");
    let input = object_input(&dir, 48000, false);
    for (renderer, layout, channels) in [(3, c"hoa3", 16), (0, c"5.1", 6)] {
        let options = Options::new();
        unsafe {
            adm_render_options_set_renderer(options.0, renderer);
            adm_render_options_set_output_layout(options.0, layout.as_ptr());
        }
        let output = dir.file("out.wav");
        let (code, result) = render(&context, &input, &output, Some(&options));
        assert_eq!(code, OK);
        assert_eq!(wav_shape(&output).0, channels);
        assert_eq!(
            result.output_path(),
            Some(native_c_path(&output).as_c_str())
        );
    }
}

const MATRIX: &str = r#"{
  "schema":"mradm.direct-speakers-matrix.v1",
  "output_layout":"5.1",
  "routes":[{"source_label":"C","targets":[{"label":"L","weight":1},{"label":"R","weight":1}]}]
}"#;

fn matrix_options(
    renderer: i32,
    layout: &std::ffi::CStr,
    mode: i32,
    json: Option<&str>,
) -> Options {
    let options = Options::new();
    unsafe {
        adm_render_options_set_renderer(options.0, renderer);
        adm_render_options_set_output_layout(options.0, layout.as_ptr());
        adm_render_options_set_direct_speakers_routing_mode(options.0, mode);
        if let Some(json) = json {
            let json = CString::new(json).unwrap();
            adm_render_options_set_direct_speakers_matrix_json(options.0, json.as_ptr());
        }
    }
    options
}

/// A matrix file and an in-memory matrix both render; the in-memory one wins over an unreadable
/// path with a warning; matrix mode needs a profile, a profile needs matrix mode, and a binaural
/// backend rejects it.
#[test]
fn direct_speakers_matrices_render_from_a_file_or_memory() {
    let context = Context::new();
    let dir = Dir::new("render-matrix");
    let input = direct_speakers_input(&dir);
    let matrix = dir.file("matrix.json");
    std::fs::write(&matrix, MATRIX).unwrap();
    let options = matrix_options(2, c"5.1", 3, None);
    unsafe {
        assert_eq!(
            adm_render_options_set_direct_speakers_matrix_path(
                options.0,
                native_c_path(&matrix).as_ptr()
            ),
            OK
        );
        adm_render_options_set_peak_limit(options.0, 0);
    }
    let output = dir.file("path.wav");
    assert_eq!(render(&context, &input, &output, Some(&options)).0, OK);
    assert!(output.exists());
    unsafe {
        adm_render_options_set_direct_speakers_matrix_path(
            options.0,
            c"/missing/matrix-must-not-be-read.json".as_ptr(),
        );
        let json = CString::new(MATRIX).unwrap();
        adm_render_options_set_direct_speakers_matrix_json(options.0, json.as_ptr());
    }
    let (code, result) = render(&context, &input, &dir.file("json.wav"), Some(&options));
    assert_eq!(code, OK);
    assert!(
        result
            .logs()
            .iter()
            .any(|l| l.level == 2 && l.message.contains("ignoring"))
    );
    for (options, expected) in [
        (matrix_options(2, c"5.1", 3, None), INVALID_ARGUMENT),
        (matrix_options(2, c"5.1", 0, Some(MATRIX)), INVALID_ARGUMENT),
        (matrix_options(6, c"binaural", 3, Some(MATRIX)), UNSUPPORTED),
    ] {
        assert_eq!(
            render(&context, &input, &dir.file("bad.wav"), Some(&options)).0,
            expected
        );
    }
}

/// Normalisation to -23 LUFS moves the reported loudness toward the target, with a plausible
/// peak; a +20 dB final gain bypasses the peak ceiling and raises the reported peak by ~20 dB.
#[test]
fn loudness_and_final_gain_reach_the_result_metrics() {
    let context = Context::new();
    let dir = Dir::new("render-loudness");
    let input = object_input_1s(&dir);
    let raw = render(&context, &input, &dir.file("raw.wav"), None)
        .1
        .lufs()
        .unwrap();
    let options = Options::new();
    unsafe { adm_render_options_set_loudness_target(options.0, -23.0) };
    let (code, result) = render(&context, &input, &dir.file("lufs.wav"), Some(&options));
    assert_eq!(code, OK);
    let lufs = result.lufs().unwrap();
    assert!(
        (lufs + 23.0).abs() < (raw + 23.0).abs(),
        "{lufs} vs raw {raw}"
    );
    let peak = result.peak().unwrap();
    assert!(peak <= 0.0 && peak > -40.0);
    assert_eq!(
        unsafe { adm_render_result_loudness_lufs(result.0, ptr::null_mut()) },
        1
    );

    let short = object_input(&dir, 48000, false);
    let base = render(&context, &short, &dir.file("base.wav"), None)
        .1
        .peak()
        .unwrap();
    let options = Options::new();
    unsafe { adm_render_options_set_final_gain_db(options.0, 20.0) };
    let (code, result) = render(&context, &short, &dir.file("boost.wav"), Some(&options));
    assert_eq!(code, OK);
    let boosted = result.peak().unwrap();
    assert!(boosted > 0.0);
    assert!((boosted - base - 20.0).abs() < 0.5, "{base} -> {boosted}");
}

#[test]
fn trim_windows_shorten_the_output() {
    let context = Context::new();
    let dir = Dir::new("render-trim");
    let input = object_input_1s(&dir);
    let full = dir.file("full.wav");
    assert_eq!(render(&context, &input, &full, None).0, OK);
    assert_eq!(wav_shape(&full).1, 48000);
    let options = Options::new();
    unsafe {
        assert_eq!(adm_render_options_set_render_start_sec(options.0, 0.25), OK);
        assert_eq!(adm_render_options_set_render_end_sec(options.0, 0.75), OK);
    }
    let trimmed = dir.file("trim.wav");
    let (code, result) = render(&context, &input, &trimmed, Some(&options));
    assert_eq!((code, result.code()), (OK, OK));
    assert_eq!(wav_shape(&trimmed).1, 24000);
}

fn cancelled_cleanly(dir: &Dir, output: &Path) -> bool {
    !output.exists()
        && !dir.has_sidecar(output, "render_tmp")
        && !dir.has_sidecar(output, "output_tmp")
}

/// A token cancelled before the render stops it at the first checkpoint with no output; after a
/// reset the same token and options render.
#[test]
fn cancelled_renders_leave_nothing_and_tokens_are_reusable() {
    let context = Context::new();
    let dir = Dir::new("render-cancel");
    let input = object_input(&dir, 48000, false);
    let output = dir.file("out.wav");
    let token = adm_create_cancel_token();
    let options = Options::new();
    unsafe {
        adm_render_options_set_cancel_token(options.0, token);
        adm_cancel(token);
    }
    let (code, result) = render(&context, &input, &output, Some(&options));
    assert_eq!((code, result.code()), (CANCELLED, CANCELLED));
    assert!(cancelled_cleanly(&dir, &output));
    unsafe { adm_reset_cancel_token(token) };
    assert_eq!(render(&context, &input, &output, Some(&options)).0, OK);
    assert!(output.exists());
    drop(options);
    unsafe { adm_destroy_cancel_token(token) };
}

/// A cancel from another thread while a render runs ends it either way, cleanly.
#[test]
fn a_cancel_from_another_thread_ends_the_render_cleanly() {
    let dir = Dir::new("render-cancel-thread");
    let input = object_input_1s(&dir);
    for _ in 0..4 {
        let output = dir.file("out.wav");
        let _ = std::fs::remove_file(&output);
        let token = adm_create_cancel_token();
        let options = Options::new();
        unsafe { adm_render_options_set_cancel_token(options.0, token) };
        let (shared, path) = (options.0 as usize, output.clone());
        let input = input.clone();
        let worker = std::thread::spawn(move || {
            let context = Context::new();
            let mut result = ptr::null_mut();
            let code = unsafe {
                adm_render_file_ex(
                    context.0,
                    native_c_path(&input).as_ptr(),
                    native_c_path(&path).as_ptr(),
                    shared as *const adm_render_options_t,
                    None,
                    ptr::null_mut(),
                    &mut result,
                )
            };
            drop(Res(result));
            code
        });
        unsafe { adm_cancel(token) };
        let code = worker.join().unwrap();
        assert!(code == OK || code == CANCELLED);
        if code == CANCELLED {
            assert!(cancelled_cleanly(&dir, &output));
        }
        drop(options);
        unsafe { adm_destroy_cancel_token(token) };
    }
}

/// An in-memory policy that mutes the scene is applied before rendering and its captured report
/// shows the mute; without capture there is no report; malformed JSON fails the render (not the
/// setter); a failure after the report was built still exposes it.
#[test]
fn in_memory_policies_apply_and_reports_survive_late_failures() {
    let context = Context::new();
    let dir = Dir::new("render-policy");
    let input = object_input(&dir, 48000, false);
    let options = Options::new();
    unsafe {
        assert_eq!(
            adm_render_options_set_semantic_policy_json(options.0, MUTE_POLICY.as_ptr()),
            OK
        );
        adm_render_options_set_capture_semantic_report(options.0, 1);
    }
    let (code, result) = render(&context, &input, &dir.file("muted.wav"), Some(&options));
    assert_eq!(code, OK);
    let report = result.report().unwrap();
    assert!(report.contains("mradm.semantic-report.v1"));
    assert!(report.contains("\"effective_mute\": true"));
    unsafe { adm_render_options_set_capture_semantic_report(options.0, 0) };
    assert!(
        render(&context, &input, &dir.file("muted.wav"), Some(&options))
            .1
            .report()
            .is_none()
    );

    let bad = Options::new();
    unsafe {
        assert_eq!(
            adm_render_options_set_semantic_policy_json(bad.0, c"{ not valid json".as_ptr()),
            OK
        );
    }
    assert_ne!(
        render(&context, &input, &dir.file("bad.wav"), Some(&bad)).0,
        OK
    );

    // FLAC with a height layout fails in format validation, after the report was captured.
    unsafe {
        adm_render_options_set_capture_semantic_report(options.0, 1);
        adm_render_options_set_output_layout(options.0, c"7.1.4".as_ptr());
    }
    let (code, result) = render(&context, &input, &dir.file("late.flac"), Some(&options));
    assert_ne!(code, OK);
    assert!(
        result
            .report()
            .unwrap()
            .contains("mradm.semantic-report.v1")
    );
}

/// IAMF layers must grow monotonically; the check runs before any encoder.
#[test]
fn iamf_layers_must_be_monotonic() {
    let context = Context::new();
    let dir = Dir::new("render-iamf");
    let input = object_input(&dir, 48000, false);
    let options = Options::new();
    unsafe {
        adm_render_options_set_output_layout(options.0, c"7.1.4".as_ptr());
        adm_render_options_set_iamf_layers(options.0, c"5.1,7.1,5.1.2,7.1.4".as_ptr());
    }
    let (code, result) = render(&context, &input, &dir.file("out.iamf"), Some(&options));
    assert_eq!(code, INVALID_ARGUMENT);
    assert!(
        result.message().contains("monotonic"),
        "{}",
        result.message()
    );
}

/// Triple Balance D renders 9.1.6 and 22.2 (with equal-power LFE) and names its model in the
/// report; other layouts are unsupported.
#[test]
fn triple_balance_d_reports_its_model() {
    let context = Context::new();
    let dir = Dir::new("render-d");
    let input = object_input(&dir, 48000, true);
    let options = Options::new();
    unsafe {
        adm_render_options_set_renderer(options.0, 7);
        adm_render_options_set_output_layout(options.0, c"9.1.6".as_ptr());
        adm_render_options_set_triple_balance_mode(options.0, 1);
        adm_render_options_set_capture_semantic_report(options.0, 1);
    }
    let output = dir.file("d.wav");
    let (code, result) = render(&context, &input, &output, Some(&options));
    assert_eq!(code, OK);
    assert!(result.report().unwrap().contains("triple-balance-d-v1"));
    unsafe {
        adm_render_options_set_output_layout(options.0, c"22.2".as_ptr());
        adm_render_options_set_lfe_routing_mode(options.0, 1);
    }
    let (code, result) = render(&context, &input, &output, Some(&options));
    assert_eq!(code, OK);
    let report = result.report().unwrap();
    assert!(report.contains("triple-balance-d-222-v1") && report.contains("split-power"));
    for layout in [c"7.1.4", c"5.1"] {
        unsafe { adm_render_options_set_output_layout(options.0, layout.as_ptr()) };
        assert_eq!(
            render(&context, &input, &dir.file("no.wav"), Some(&options)).0,
            UNSUPPORTED
        );
    }
}

/// The channel count and Vorbis comment tags of a FLAC file.
fn flac_shape(path: &Path) -> (u32, Vec<String>) {
    let bytes = std::fs::read(path).unwrap();
    assert_eq!(&bytes[..4], b"fLaC");
    let (mut channels, mut tags, mut at) = (0, Vec::new(), 4);
    loop {
        let header = bytes[at];
        let len = u32::from_be_bytes([0, bytes[at + 1], bytes[at + 2], bytes[at + 3]]) as usize;
        let body = &bytes[at + 4..at + 4 + len];
        match header & 0x7f {
            0 => channels = u32::from((body[12] >> 1) & 7) + 1,
            4 => {
                let le = |o: usize| u32::from_le_bytes(body[o..o + 4].try_into().unwrap()) as usize;
                let mut o = 4 + le(0);
                let count = le(o);
                o += 4;
                for _ in 0..count {
                    let n = le(o);
                    tags.push(String::from_utf8(body[o + 4..o + 4 + n].to_vec()).unwrap());
                    o += 4 + n;
                }
            }
            _ => {}
        }
        at += 4 + len;
        if header & 0x80 != 0 {
            return (channels, tags);
        }
    }
}

/// The final FLAC (from `flac_io_smoke_test`, encoded in Rust since PCM consistency 7.2 A): rendered
/// to a unique float32 temporary, encoded, the temporary removed while a stale file at the former
/// fixed temporary path stays untouched, and the Vorbis comment written with the 5.1 channel mask.
#[test]
fn flac_outputs_are_encoded_and_tagged() {
    let context = Context::new();
    let dir = Dir::new("render-flac");
    let input = object_input(&dir, 48000, false);
    let output = dir.file("out.flac");
    let legacy = dir.file("out.render_tmp.wav");
    std::fs::write(&legacy, "stale").unwrap();
    let options = Options::new();
    unsafe {
        adm_render_options_set_renderer(options.0, 2);
        adm_render_options_set_output_layout(options.0, c"5.1".as_ptr());
        adm_render_options_set_peak_limit(options.0, 0);
    }
    let (code, result) = render(&context, &input, &output, Some(&options));
    assert_eq!(code, OK, "{}", result.message());
    assert_eq!(std::fs::read(&legacy).unwrap(), b"stale");
    std::fs::remove_file(&legacy).unwrap();
    assert!(!dir.has_sidecar(&output, "render_tmp"));
    assert!(!dir.has_sidecar(&output, "output_tmp"));
    let (channels, tags) = flac_shape(&output);
    assert_eq!(channels, 6);
    assert!(
        tags.iter()
            .any(|t| t == "WAVEFORMATEXTENSIBLE_CHANNEL_MASK=0x0000003F"),
        "{tags:?}"
    );
    assert!(
        tags.iter().any(|t| t.starts_with("COMMENT=renderer=")),
        "{tags:?}"
    );
}
