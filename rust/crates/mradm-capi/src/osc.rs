//! OSC head tracking (`adm_osc_head_tracking_*`): the loopback receiver of `mradm-osc`
//! (`receiver::Receiver`), bound by `mradm-ffi` as the platform requires.
use crate::guard::{self, INTERNAL, INVALID_ARGUMENT, OK};
use crate::scene::{Message, lock, owned};
use mradm_osc::receiver::Receiver;
use mradm_osc::valid_source_id;
use std::ffi::{CStr, c_char};
use std::ptr;
use std::sync::Mutex;

/// `adm_osc_head_tracking_config_t`.
#[allow(non_camel_case_types)]
#[repr(C)]
pub struct adm_osc_head_tracking_config_t {
    pub(crate) struct_size: u32,
    pub(crate) listen_port: u32,
    pub(crate) source_id: *const c_char,
}

/// `adm_head_tracking_pose_t`.
#[allow(non_camel_case_types)]
#[repr(C)]
pub struct adm_head_tracking_pose_t {
    pub(crate) struct_size: u32,
    pub(crate) protocol_version: u32,
    pub(crate) has_pose: u32,
    pub(crate) fresh: u32,
    pub(crate) receiver_session_id: u64,
    pub(crate) receiver_sequence: u64,
    pub(crate) receiver_received_ns: u64,
    pub(crate) age_ms: u64,
    pub(crate) instance_id: u64,
    pub(crate) source_session_id: u64,
    pub(crate) source_sequence: u64,
    pub(crate) tx_sequence: u64,
    pub(crate) reference_epoch: u64,
    pub(crate) metadata_revision: u64,
    pub(crate) source_received_ns: u64,
    pub(crate) source_age_at_send_ns: u64,
    pub(crate) sample_time_ms: u64,
    pub(crate) sample_clock_epoch: u64,
    pub(crate) sample_time_kind: u32,
    pub(crate) quaternion_xyzw: [f32; 4],
    pub(crate) yaw_deg: f32,
    pub(crate) pitch_deg: f32,
    pub(crate) roll_deg: f32,
}

/// `adm_osc_head_tracking_status_t`.
#[allow(non_camel_case_types)]
#[repr(C)]
pub struct adm_osc_head_tracking_status_t {
    pub(crate) struct_size: u32,
    pub(crate) state: i32,
    pub(crate) bound_port: u32,
    pub(crate) has_pose: u32,
    pub(crate) session_id: u64,
    pub(crate) sequence: u64,
    pub(crate) packets_received: u64,
    pub(crate) rejected_packets: u64,
    pub(crate) recovery_count: u64,
    pub(crate) age_ms: u64,
    pub(crate) pose_packets: u64,
    pub(crate) telemetry_packets: u64,
    pub(crate) ignored_sources: u64,
    pub(crate) protocol_mismatches: u64,
    pub(crate) missing_tx_packets: u64,
    pub(crate) has_heartbeat: u32,
    pub(crate) heartbeat_alive: u32,
    pub(crate) heartbeat_age_ms: u64,
}

#[allow(non_camel_case_types)] // The `c_api.h` name.
pub struct adm_osc_head_tracking_t {
    receiver: Receiver,
    /// Backs `adm_osc_head_tracking_last_error_message` until the next call.
    error_copy: Mutex<Message>,
}

#[unsafe(no_mangle)]
pub unsafe extern "C" fn adm_create_osc_head_tracking(
    config: *const adm_osc_head_tracking_config_t,
    out: *mut *mut adm_osc_head_tracking_t,
) -> i32 {
    let Some(out) = (unsafe { out.as_mut() }) else {
        return INVALID_ARGUMENT;
    };
    *out = ptr::null_mut();
    let config = unsafe { config.as_ref() };
    if let Some(c) = config
        && ((c.struct_size as usize) < size_of::<adm_osc_head_tracking_config_t>()
            || c.listen_port > 65535)
    {
        return INVALID_ARGUMENT;
    }
    let source = match config {
        Some(c) if !c.source_id.is_null() => unsafe { CStr::from_ptr(c.source_id) }.to_bytes(),
        _ => &[][..],
    };
    if !source.is_empty() && !valid_source_id(source) {
        return INVALID_ARGUMENT;
    }
    guard::code(|| {
        let port = config.map_or(9000, |c| c.listen_port) as u16;
        let receiver = mradm_ffi::api::osc::receiver(port, source);
        let handle = crate::error::try_box(adm_osc_head_tracking_t {
            receiver,
            error_copy: Mutex::new(Message::default()),
        });
        if handle.is_null() {
            return INTERNAL;
        }
        *out = handle;
        OK
    })
}

#[unsafe(no_mangle)]
pub unsafe extern "C" fn adm_destroy_osc_head_tracking(receiver: *mut adm_osc_head_tracking_t) {
    if !receiver.is_null() {
        guard::unit(|| drop(unsafe { Box::from_raw(receiver) }));
    }
}

#[unsafe(no_mangle)]
pub unsafe extern "C" fn adm_osc_head_tracking_start(
    receiver: *mut adm_osc_head_tracking_t,
) -> i32 {
    let Some(r) = (unsafe { receiver.as_ref() }) else {
        return INVALID_ARGUMENT;
    };
    guard::code(|| match r.receiver.start() {
        Ok(()) => OK,
        Err(e) => crate::error::map(e.code),
    })
}

#[unsafe(no_mangle)]
pub unsafe extern "C" fn adm_osc_head_tracking_stop(receiver: *mut adm_osc_head_tracking_t) {
    if let Some(r) = unsafe { receiver.as_ref() } {
        guard::unit(|| r.receiver.stop());
    }
}

/// A caller's output record of at least `size_of::<T>()` bytes.
unsafe fn output<'a, T>(out: *mut T) -> Option<&'a mut T> {
    let header = unsafe { out.cast::<u32>().as_ref() }?;
    ((*header as usize) >= size_of::<T>()).then(|| unsafe { &mut *out })
}

#[unsafe(no_mangle)]
pub unsafe extern "C" fn adm_osc_head_tracking_get_pose(
    receiver: *const adm_osc_head_tracking_t,
    out: *mut adm_head_tracking_pose_t,
) -> i32 {
    let (Some(r), Some(out)) = (unsafe { receiver.as_ref() }, unsafe { output(out) }) else {
        return INVALID_ARGUMENT;
    };
    guard::code(|| {
        let s = r.receiver.snapshot();
        let t = &s.timing;
        *out = adm_head_tracking_pose_t {
            struct_size: size_of::<adm_head_tracking_pose_t>() as u32,
            protocol_version: t.protocol_version,
            has_pose: u32::from(s.has_pose),
            fresh: u32::from(s.fresh),
            receiver_session_id: s.session_id,
            receiver_sequence: s.sequence,
            receiver_received_ns: s.received_ns,
            age_ms: s.age_ms,
            instance_id: t.instance_id,
            source_session_id: t.source_session_id,
            source_sequence: t.source_sequence,
            tx_sequence: t.tx_sequence,
            reference_epoch: t.reference_epoch,
            metadata_revision: t.metadata_revision,
            source_received_ns: t.source_received_ns,
            source_age_at_send_ns: t.source_age_at_send_ns,
            sample_time_ms: t.sample_time_ms,
            sample_clock_epoch: t.sample_clock_epoch,
            sample_time_kind: t.sample_time_kind,
            quaternion_xyzw: s.quaternion_xyzw,
            yaw_deg: s.euler_deg[0],
            pitch_deg: s.euler_deg[1],
            roll_deg: s.euler_deg[2],
        };
        OK
    })
}

#[unsafe(no_mangle)]
pub unsafe extern "C" fn adm_osc_head_tracking_get_status(
    receiver: *const adm_osc_head_tracking_t,
    out: *mut adm_osc_head_tracking_status_t,
) -> i32 {
    let (Some(r), Some(out)) = (unsafe { receiver.as_ref() }, unsafe { output(out) }) else {
        return INVALID_ARGUMENT;
    };
    guard::code(|| {
        let s = r.receiver.snapshot();
        *out = adm_osc_head_tracking_status_t {
            struct_size: size_of::<adm_osc_head_tracking_status_t>() as u32,
            state: s.state as i32,
            bound_port: u32::from(s.bound_port),
            has_pose: u32::from(s.has_pose),
            session_id: s.session_id,
            sequence: s.sequence,
            packets_received: s.packets_received,
            rejected_packets: s.rejected_packets,
            recovery_count: s.recovery_count,
            age_ms: s.age_ms,
            pose_packets: s.pose_packets,
            telemetry_packets: s.telemetry_packets,
            ignored_sources: s.ignored_sources,
            protocol_mismatches: s.protocol_mismatches,
            missing_tx_packets: s.missing_tx_packets,
            has_heartbeat: u32::from(s.has_heartbeat),
            heartbeat_alive: u32::from(s.heartbeat_alive),
            heartbeat_age_ms: s.heartbeat_age_ms,
        };
        OK
    })
}

#[unsafe(no_mangle)]
pub unsafe extern "C" fn adm_osc_head_tracking_snapshot_json(
    receiver: *const adm_osc_head_tracking_t,
    out_json: *mut *mut c_char,
) -> i32 {
    if let Some(out) = unsafe { out_json.as_mut() } {
        *out = ptr::null_mut();
    }
    let Some(r) = (unsafe { receiver.as_ref() }) else {
        return INVALID_ARGUMENT;
    };
    if out_json.is_null() {
        return INVALID_ARGUMENT;
    }
    guard::code(|| {
        let Ok(json) = r.receiver.snapshot_json() else {
            return INTERNAL;
        };
        match unsafe { owned(json.as_bytes(), out_json) } {
            Ok(()) => OK,
            Err(_) => INTERNAL,
        }
    })
}

/// The text the C++ returned when the error query threw.
const QUERY_FAILED: &CStr = c"OSC 错误信息查询失败";

#[unsafe(no_mangle)]
pub unsafe extern "C" fn adm_osc_head_tracking_last_error_message(
    receiver: *mut adm_osc_head_tracking_t,
) -> *const c_char {
    let Some(r) = (unsafe { receiver.as_ref() }) else {
        return c"".as_ptr();
    };
    guard::guard(QUERY_FAILED.as_ptr(), || {
        let text = r.receiver.last_error();
        let mut copy = lock(&r.error_copy);
        if copy.set(text.as_bytes()) {
            copy.as_ptr()
        } else {
            QUERY_FAILED.as_ptr()
        }
    })
}
