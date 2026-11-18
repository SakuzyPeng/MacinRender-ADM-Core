//! The Monitor and Scene output entries that need no output device (from `c_api_fixture_test`'s
//! monitor ABI check): argument checks and a failed creation. The live monitor checks (playback,
//! levels, overrides, backend switches, devices) run in the C consumer, which links the devices.
use super::common::*;
use crate::monitor::*;
use crate::scene::{adm_hptf_config_t, adm_scene_output_set_hptf};
use std::mem::MaybeUninit;
use std::ptr;

#[test]
fn monitor_entries_reject_null_handles() {
    let m = ptr::null_mut();
    unsafe {
        assert_eq!(adm_monitor_set_hptf(m, ptr::null()), INVALID_ARGUMENT);
        assert_eq!(
            adm_monitor_get_hptf_info(m, ptr::null_mut()),
            INVALID_ARGUMENT
        );
        assert_eq!(
            adm_monitor_set_listener_orientation(m, 0.0, 0.0, 0.0),
            INVALID_ARGUMENT
        );
        assert_eq!(
            adm_monitor_set_output_device(m, c"".as_ptr()),
            INVALID_ARGUMENT
        );
        assert_eq!(
            adm_monitor_set_overrides(m, ptr::null(), 0, 0),
            INVALID_ARGUMENT
        );
        assert_eq!(adm_monitor_switch_backend(m, ptr::null()), INVALID_ARGUMENT);
        assert_eq!(*adm_monitor_last_error_message(ptr::null()), 0);
        assert_eq!(
            adm_scene_output_set_hptf(ptr::null_mut(), ptr::null()),
            INVALID_ARGUMENT
        );
        // A config too small to carry `profile_path` is rejected before anything is read from it.
        let mut undersized = MaybeUninit::<adm_hptf_config_t>::zeroed();
        undersized.as_mut_ptr().cast::<u32>().write(4);
        assert_eq!(
            adm_scene_output_set_hptf(ptr::null_mut(), undersized.as_ptr()),
            INVALID_ARGUMENT
        );
    }
}

#[test]
fn monitor_creation_checks_its_arguments_and_reports_failures() {
    let context = Context::new();
    let dir = Dir::new("monitor");
    let input = native_c_path(&object_input(&dir, 48000, false));
    let mut monitor = ptr::null_mut();
    unsafe {
        assert_eq!(
            adm_create_monitor(ptr::null_mut(), input.as_ptr(), ptr::null(), &mut monitor),
            INVALID_ARGUMENT
        );
        assert_eq!(
            adm_create_monitor(context.0, ptr::null(), ptr::null(), &mut monitor),
            INVALID_ARGUMENT
        );
        assert_eq!(
            adm_create_monitor(context.0, input.as_ptr(), ptr::null(), ptr::null_mut()),
            INVALID_ARGUMENT
        );
        let missing = native_c_path(&dir.file("missing.wav"));
        assert_ne!(
            adm_create_monitor(context.0, missing.as_ptr(), ptr::null(), &mut monitor),
            OK
        );
    }
    assert!(monitor.is_null());
    assert!(context.has_error());
}
