//! Option setters, the cancel token and the version (from `c_api_fixture_test`: options lifecycle,
//! trim, final gain, semantic and channel-bed setters, cancel token lifecycle).
use super::common::*;
use crate::cancel::*;
use crate::options::*;
use crate::version::*;
use std::ffi::{CStr, c_char};
use std::ptr;

#[test]
fn the_version_is_the_header_version() {
    assert_eq!(
        (
            adm_api_version_major(),
            adm_api_version_minor(),
            adm_api_version_patch()
        ),
        (1, 50, 0)
    );
}

type Opts = *mut adm_render_options_t;

fn s(text: &CStr) -> *const c_char {
    text.as_ptr()
}

/// Every setter is a no-op on NULL options; the checked ones still return OK.
#[test]
fn null_options_are_a_no_op() {
    let n: Opts = ptr::null_mut();
    unsafe {
        let codes = [
            adm_render_options_set_renderer(n, 5),
            adm_render_options_set_triple_balance_mode(n, 1),
            adm_render_options_set_output_layout(n, s(c"5.1")),
            adm_render_options_set_output_bit_depth(n, 1),
            adm_render_options_set_speaker_spread_mode(n, 2),
            adm_render_options_set_speaker_geometry(n, 1),
            adm_render_options_set_direct_speakers_routing_mode(n, 1),
            adm_render_options_set_direct_speakers_matrix_path(n, s(c"/any/matrix.json")),
            adm_render_options_set_direct_speakers_matrix_json(n, s(c"{}")),
            adm_render_options_set_binaural_spread_mode(n, 2),
            adm_render_options_set_lfe_routing_mode(n, 1),
            adm_render_options_set_apac_container(n, 1),
            adm_render_options_set_loudness_target(n, -23.0),
            adm_render_options_set_sofa_path(n, s(c"/any/path.sofa")),
            adm_render_options_set_semantic_policy_path(n, s(c"/any/policy.json")),
            adm_render_options_set_semantic_report_path(n, s(c"/any/report.json")),
            adm_render_options_set_semantic_policy_json(n, s(c"x")),
            adm_render_options_set_iamf_layers(n, s(c"5.1,7.1.4")),
            adm_render_options_set_render_start_sec(n, 1.0),
            adm_render_options_set_render_end_sec(n, 2.0),
            adm_render_options_set_final_gain_db(n, 6.0),
            adm_render_options_set_input_layout(n, s(c"5.1")),
            adm_render_options_set_input_channel_labels(n, s(c"L,R")),
        ];
        assert!(codes.iter().all(|&c| c == OK), "{codes:?}");
        adm_render_options_set_peak_limit(n, 1);
        adm_render_options_set_peak_normalize_to_limit(n, 1);
        adm_render_options_set_apac_drc_music(n, 1);
        adm_render_options_set_apple_speaker_rendering_flags(n, 1);
        adm_render_options_set_capture_semantic_report(n, 1);
        adm_render_options_set_cancel_token(n, ptr::null_mut());
        adm_destroy_render_options(ptr::null_mut());
    }
}

/// Out-of-range enums and values are rejected; each range end and every documented value is kept.
#[test]
fn setters_check_their_ranges() {
    let options = Options::new();
    let o = options.0;
    type Case<'a> = (&'a str, i32, Box<dyn Fn() -> i32>);
    let enumerated = |name, setter: unsafe extern "C" fn(Opts, i32) -> i32, valid: &[i32]| {
        let mut cases: Vec<Case> = vec![(
            name,
            INVALID_ARGUMENT,
            Box::new(move || unsafe { setter(o, 99) }),
        )];
        for &v in valid {
            cases.push((name, OK, Box::new(move || unsafe { setter(o, v) })));
        }
        cases
    };
    let mut cases: Vec<Case> = Vec::new();
    cases.extend(enumerated(
        "renderer",
        adm_render_options_set_renderer,
        &[7, 3, 6, 0],
    ));
    cases.extend(enumerated(
        "triple_balance_mode",
        adm_render_options_set_triple_balance_mode,
        &[1, 0],
    ));
    cases.extend(enumerated(
        "bit_depth",
        adm_render_options_set_output_bit_depth,
        &[],
    ));
    cases.extend(enumerated(
        "speaker_spread",
        adm_render_options_set_speaker_spread_mode,
        &[],
    ));
    cases.extend(enumerated(
        "speaker_geometry",
        adm_render_options_set_speaker_geometry,
        &[1, 0],
    ));
    cases.extend(enumerated(
        "routing",
        adm_render_options_set_direct_speakers_routing_mode,
        &[1, 2, 3, 0],
    ));
    cases.extend(enumerated(
        "binaural_spread",
        adm_render_options_set_binaural_spread_mode,
        &[],
    ));
    cases.extend(enumerated(
        "lfe_routing",
        adm_render_options_set_lfe_routing_mode,
        &[0, 1, 0],
    ));
    cases.extend(enumerated(
        "iamf_container",
        adm_render_options_set_iamf_container,
        &[],
    ));
    cases.extend(enumerated(
        "apac_container",
        adm_render_options_set_apac_container,
        &[1, 0],
    ));
    let real = |name, setter: unsafe extern "C" fn(Opts, f64) -> i32, bad: &[f64], good: &[f64]| {
        let mut cases: Vec<Case> = Vec::new();
        for &v in bad {
            cases.push((
                name,
                INVALID_ARGUMENT,
                Box::new(move || unsafe { setter(o, v) }),
            ));
        }
        for &v in good {
            cases.push((name, OK, Box::new(move || unsafe { setter(o, v) })));
        }
        cases
    };
    let non_finite = [f64::NAN, f64::INFINITY];
    cases.extend(real(
        "loudness_target",
        adm_render_options_set_loudness_target,
        &[f64::NAN, f64::INFINITY, 0.1, -70.1],
        &[-70.0, 0.0],
    ));
    cases.extend(real(
        "peak_limit_dbtp",
        adm_render_options_set_peak_limit_dbtp,
        &[f64::NAN, 0.1, -60.1],
        &[-60.0, 0.0],
    ));
    // Start rejects negative and non-finite; end clears at <= 0 and rejects non-finite.
    cases.extend(real(
        "render_start_sec",
        adm_render_options_set_render_start_sec,
        &[-0.1, f64::NAN],
        &[0.0, 1.5],
    ));
    cases.extend(real(
        "render_end_sec",
        adm_render_options_set_render_end_sec,
        &[f64::INFINITY],
        &[4.0, 0.0, -1.0],
    ));
    // The final gain has no range limit.
    cases.extend(real(
        "final_gain_db",
        adm_render_options_set_final_gain_db,
        &non_finite,
        &[0.0, 12.0, -30.0, 100.0],
    ));
    let count =
        |name, setter: unsafe extern "C" fn(Opts, u32) -> i32, bad: &[u32], good: &[u32]| {
            let mut cases: Vec<Case> = Vec::new();
            for &v in bad {
                cases.push((
                    name,
                    INVALID_ARGUMENT,
                    Box::new(move || unsafe { setter(o, v) }),
                ));
            }
            for &v in good {
                cases.push((name, OK, Box::new(move || unsafe { setter(o, v) })));
            }
            cases
        };
    cases.extend(count(
        "opus_bitrate",
        adm_render_options_set_opus_bitrate_per_ch_kbps,
        &[5, 321],
        &[0, 128],
    ));
    cases.extend(count(
        "apac_bitrate",
        adm_render_options_set_apac_bitrate_kbps,
        &[63, 32769],
        &[0, 32768],
    ));
    cases.extend(count(
        "interp_ms",
        adm_render_options_set_default_interp_ms,
        &[501],
        &[500],
    ));
    cases.extend(count(
        "smoothing_frames",
        adm_render_options_set_object_smoothing_frames,
        &[48001],
        &[48000],
    ));
    for (name, expected, call) in &cases {
        assert_eq!(call(), *expected, "{name}");
    }
}

/// String setters: NULL is rejected for the layout only; NULL or "" clears the optional paths and
/// documents, and any string is stored (a policy is validated at render time).
#[test]
fn string_setters_clear_and_store() {
    let options = Options::new();
    let o = options.0;
    let n = ptr::null();
    unsafe {
        assert_eq!(adm_render_options_set_output_layout(o, n), INVALID_ARGUMENT);
        assert_eq!(adm_render_options_set_output_layout(o, s(c"7.1.4")), OK);
        type Setter = unsafe extern "C" fn(Opts, *const c_char) -> i32;
        let clearable: [(Setter, &CStr); 8] = [
            (adm_render_options_set_iamf_layers, c"5.1,5.1.2,5.1.4,7.1.4"),
            (adm_render_options_set_sofa_path, c"/valid/path.sofa"),
            (
                adm_render_options_set_semantic_policy_path,
                c"/valid/policy.json",
            ),
            (
                adm_render_options_set_semantic_report_path,
                c"/valid/report.json",
            ),
            (
                adm_render_options_set_semantic_policy_json,
                c"{ not valid json",
            ),
            (
                adm_render_options_set_direct_speakers_matrix_path,
                c"/tmp/matrix.json",
            ),
            (
                adm_render_options_set_direct_speakers_matrix_json,
                c"{\"schema\":\"test\"}",
            ),
            (adm_render_options_set_input_channel_labels, c"L,R,C,LFE"),
        ];
        for (setter, value) in clearable {
            assert_eq!(setter(o, value.as_ptr()), OK);
            assert_eq!(setter(o, s(c"")), OK);
            assert_eq!(setter(o, n), OK);
        }
        assert_eq!(adm_render_options_set_input_layout(o, s(c"7.1.4")), OK);
        assert_eq!(adm_render_options_set_input_layout(o, s(c"auto")), OK);
        for on in [1, 0] {
            adm_render_options_set_peak_limit(o, on);
            adm_render_options_set_peak_normalize_to_limit(o, on);
            adm_render_options_set_apac_drc_music(o, on);
            adm_render_options_set_apple_speaker_rendering_flags(o, on);
            adm_render_options_set_capture_semantic_report(o, on);
        }
    }
}

/// Every token entry tolerates NULL; cancel is idempotent and a token can be set and cleared.
#[test]
fn cancel_tokens_survive_null_and_repetition() {
    unsafe {
        adm_cancel(ptr::null_mut());
        adm_reset_cancel_token(ptr::null_mut());
        adm_destroy_cancel_token(ptr::null_mut());
        let token = adm_create_cancel_token();
        assert!(!token.is_null());
        adm_cancel(token);
        adm_cancel(token);
        adm_reset_cancel_token(token);
        let options = Options::new();
        adm_render_options_set_cancel_token(options.0, token);
        adm_render_options_set_cancel_token(options.0, ptr::null_mut());
        drop(options);
        adm_destroy_cancel_token(token);
    }
}
