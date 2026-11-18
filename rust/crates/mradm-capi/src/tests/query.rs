//! Probe, inspect, policy templates and the discovery documents (from `c_api_fixture_test`).
//! Each document is read as a C caller would: an owned string checked for its schema and rows.
use super::common::*;
use crate::context::adm_context_t;
use crate::query::*;
use crate::render::*;
use std::ffi::c_char;
use std::ptr;

#[test]
fn probes_report_the_input_shape() {
    let context = Context::new();
    let dir = Dir::new("probe");
    let input = c(&object_input(&dir, 48000, false));
    let mut info = ptr::null_mut();
    unsafe {
        assert_eq!(adm_probe_file(context.0, input.as_ptr(), &mut info), OK);
        assert!(!info.is_null());
        assert_eq!(adm_scene_info_sample_rate(info), 48000);
        assert_eq!(adm_scene_info_channels(info), 1);
        assert_eq!(adm_scene_info_frames(info), 9600);
        assert!((adm_scene_info_duration_seconds(info) - 0.2).abs() < 0.01);
        assert_eq!(adm_scene_info_programme_count(info), 1);
        assert_eq!(adm_scene_info_object_count(info), 1);
        adm_destroy_scene_info(info);
        adm_destroy_scene_info(ptr::null_mut());
        assert_eq!(
            adm_probe_file(context.0, input.as_ptr(), ptr::null_mut()),
            OK
        );
        assert_eq!(
            adm_probe_file(context.0, ptr::null(), ptr::null_mut()),
            INVALID_ARGUMENT
        );
        assert_eq!(
            adm_probe_file(context.0, c"".as_ptr(), ptr::null_mut()),
            INVALID_ARGUMENT
        );
        let missing = c(&dir.file("missing.wav"));
        let mut info = ptr::null_mut();
        assert_ne!(adm_probe_file(context.0, missing.as_ptr(), &mut info), OK);
        assert!(info.is_null());
        let n = ptr::null();
        assert_eq!(
            (
                adm_scene_info_sample_rate(n),
                adm_scene_info_channels(n),
                adm_scene_info_frames(n),
                adm_scene_info_programme_count(n),
                adm_scene_info_object_count(n)
            ),
            (0, 0, 0, 0, 0)
        );
        assert_eq!(adm_scene_info_duration_seconds(n), 0.0);
    }
}

type FileQuery = unsafe extern "C" fn(*mut adm_context_t, *const c_char, *mut *mut c_char) -> i32;

/// A file query's document, its validate-only call, and its argument and missing-file errors.
fn file_query(
    query: FileQuery,
    context: &Context,
    dir: &Dir,
    input: &std::path::Path,
    empty_rejected: bool,
) -> String {
    let input = c(input);
    let mut out = ptr::null_mut();
    assert_eq!(unsafe { query(context.0, input.as_ptr(), &mut out) }, OK);
    let document = unsafe { taken(out) }.unwrap();
    assert!(!document.is_empty());
    assert_eq!(
        unsafe { query(context.0, input.as_ptr(), ptr::null_mut()) },
        OK
    );
    let mut bad = ptr::null_mut();
    unsafe {
        assert_eq!(
            query(ptr::null_mut(), input.as_ptr(), &mut bad),
            INVALID_ARGUMENT
        );
        assert_eq!(query(context.0, ptr::null(), &mut bad), INVALID_ARGUMENT);
        if empty_rejected {
            assert_eq!(query(context.0, c"".as_ptr(), &mut bad), INVALID_ARGUMENT);
        }
        let missing = c(&dir.file("missing.wav"));
        assert_ne!(query(context.0, missing.as_ptr(), &mut bad), OK);
    }
    assert!(bad.is_null());
    document
}

#[test]
fn inspect_documents_carry_the_scene() {
    let context = Context::new();
    let dir = Dir::new("inspect");
    let input = object_input(&dir, 48000, false);
    let json = file_query(adm_inspect_file_json, &context, &dir, &input, true);
    for needle in [
        "\"schema\": \"mradm.scene-inspect\"",
        "\"schema_version\": 1",
        "\"programmes\"",
        "\"contents\"",
        "\"objects\"",
        "TestProgramme",
        "TestContent",
        "TestObject",
        "\"azimuth\"",
        "\"head_locked\": false",
    ] {
        assert!(json.contains(needle), "{needle}");
    }
    let xml = file_query(adm_inspect_file_xml, &context, &dir, &input, true);
    assert!(
        xml.contains("audioFormatExtended")
            && xml.contains("TestObject")
            && xml.contains("TestProgramme")
    );
    let template = file_query(adm_policy_template_json, &context, &dir, &input, false);
    for needle in [
        "\"schema\": \"mradm.semantic-policy.v1\"",
        "\"global\"",
        "\"objects\"",
        "TestObject",
    ] {
        assert!(template.contains(needle), "{needle}");
    }
    unsafe { crate::string::adm_free_string(ptr::null_mut()) };
}

type Document = unsafe extern "C" fn(*mut adm_context_t, *mut *mut c_char) -> i32;

fn document(query: Document, context: &Context) -> String {
    let mut out = ptr::null_mut();
    assert_eq!(unsafe { query(context.0, &mut out) }, OK);
    let text = unsafe { taken(out) }.unwrap();
    let mut bad = ptr::null_mut();
    assert_eq!(
        unsafe { query(ptr::null_mut(), &mut bad) },
        INVALID_ARGUMENT
    );
    assert!(bad.is_null());
    assert_eq!(
        unsafe { query(context.0, ptr::null_mut()) },
        INVALID_ARGUMENT
    );
    text
}

fn has_all(text: &str, needles: &[&str]) {
    for needle in needles {
        assert!(text.contains(needle), "missing {needle}");
    }
}

/// The discovery documents keep their schemas and representative rows. The platform-dependent
/// rows (APAC on macOS) are checked by the C consumer, which links the platform islands.
#[test]
fn discovery_documents_keep_their_schemas_and_rows() {
    let context = Context::new();
    let capabilities = document(adm_capabilities_json, &context);
    has_all(
        &capabilities,
        &[
            "\"schema\": \"mradm.capabilities\"",
            "\"schema_version\": 1",
            "triple-balance",
            "\"backends\"",
            "\"renderer\": \"ear\"",
            "\"renderer\": \"saf\"",
            "\"renderer\": \"hoa\"",
            "\"renderer\": \"saf-binaural\"",
            "\"supports_hoa\"",
            "\"layouts\"",
            "\"channel_count\"",
            "\"hrtf_sources\"",
            "\"built-in\"",
        ],
    );
    assert!(!capabilities.contains("\"id\": \"0+2+0\""));
    has_all(
        &document(adm_layouts_json, &context),
        &[
            "\"schema\": \"mradm.layouts\"",
            "\"schema_version\": 1",
            "\"layouts\"",
            "L R C LFE Rls Rrs Ls Rs U+045 U-045 U+135 U-135",
            "\"channels\": 12",
            "\"supported_by\"",
        ],
    );
    has_all(
        &document(adm_input_layouts_json, &context),
        &[
            "\"schema\": \"mradm.input-layouts\"",
            "\"M+030\"",
            "\"U+110@45\"",
        ],
    );
    has_all(
        &document(adm_output_formats_json, &context),
        &[
            "\"schema\": \"mradm.output-formats\"",
            "\"schema_version\": 1",
            "\"features\"",
            "\"formats\"",
            "\"format\": \"wav\"",
            "\"format\": \"caf\"",
            "\"format\": \"flac\"",
            "\"format\": \"opus_mka\"",
            "\"format\": \"apac\"",
            "\"format\": \"iamf\"",
            "\"format\": \"iamf_mp4\"",
            "\"max_channels\": 8",
            "\"supports_height\": false",
            "\"bitrate_kbps_per_ch\"",
            "\"bitrate_kbps_total\"",
        ],
    );
    has_all(
        &document(adm_render_support_matrix_json, &context),
        &[
            "\"schema\": \"mradm.render-support-matrix\"",
            "\"schema_version\": 1",
            "\"features\"",
            "\"backends\"",
            "\"layouts\"",
            "\"targets\"",
            "\"entries\"",
            "\"target\": \"apac_mpeg4\"",
            "\"target\": \"apac_caf\"",
            "\"required_option\"",
            "\"value\": \"caf\"",
            "\"target\": \"iamf\"",
            "\"target\": \"iamf_mp4\"",
            "\"renderer\": \"saf-binaural\"",
            "\"layout\": \"binaural\"",
            "\"supported\": false",
            "\"reason\"",
        ],
    );
}
