//! OSC head tracking (`adm_osc_head_tracking_*`): the loopback receiver of `mradm-osc` behind the
//! entries the C++ `OscHeadTrackingReceiver` called.
use crate::guard::{self, INTERNAL, INVALID_ARGUMENT, OK};
use crate::scene::{Message, lock, owned};
use mradm_ffi::osc::{
    OscReceiver, OscSnapshot, OscText, mradm_osc_receiver_create, mradm_osc_receiver_destroy,
    mradm_osc_receiver_last_error, mradm_osc_receiver_snapshot, mradm_osc_receiver_snapshot_json,
    mradm_osc_receiver_start, mradm_osc_receiver_stop, mradm_osc_text_destroy, mradm_osc_text_view,
    mradm_osc_valid_source_id,
};
use std::ffi::{CStr, c_char};
use std::ptr;
use std::sync::Mutex;

/// `adm_osc_head_tracking_config_t`.
#[allow(non_camel_case_types)]
#[repr(C)]
pub struct adm_osc_head_tracking_config_t {
    struct_size: u32,
    listen_port: u32,
    source_id: *const c_char,
}

/// `adm_head_tracking_pose_t`.
#[allow(non_camel_case_types)]
#[repr(C)]
pub struct adm_head_tracking_pose_t {
    struct_size: u32,
    protocol_version: u32,
    has_pose: u32,
    fresh: u32,
    receiver_session_id: u64,
    receiver_sequence: u64,
    receiver_received_ns: u64,
    age_ms: u64,
    instance_id: u64,
    source_session_id: u64,
    source_sequence: u64,
    tx_sequence: u64,
    reference_epoch: u64,
    metadata_revision: u64,
    source_received_ns: u64,
    source_age_at_send_ns: u64,
    sample_time_ms: u64,
    sample_clock_epoch: u64,
    sample_time_kind: u32,
    quaternion_xyzw: [f32; 4],
    yaw_deg: f32,
    pitch_deg: f32,
    roll_deg: f32,
}

/// `adm_osc_head_tracking_status_t`.
#[allow(non_camel_case_types)]
#[repr(C)]
pub struct adm_osc_head_tracking_status_t {
    struct_size: u32,
    state: i32,
    bound_port: u32,
    has_pose: u32,
    session_id: u64,
    sequence: u64,
    packets_received: u64,
    rejected_packets: u64,
    recovery_count: u64,
    age_ms: u64,
    pose_packets: u64,
    telemetry_packets: u64,
    ignored_sources: u64,
    protocol_mismatches: u64,
    missing_tx_packets: u64,
    has_heartbeat: u32,
    heartbeat_alive: u32,
    heartbeat_age_ms: u64,
}

#[allow(non_camel_case_types)] // The `c_api.h` name.
pub struct adm_osc_head_tracking_t {
    receiver: *mut OscReceiver,
    /// Backs `adm_osc_head_tracking_last_error_message` until the next call.
    error_copy: Mutex<Message>,
}

// SAFETY: The receiver serialises its own state (the C++ receiver was shared the same way).
unsafe impl Send for adm_osc_head_tracking_t {}
unsafe impl Sync for adm_osc_head_tracking_t {}

impl Drop for adm_osc_head_tracking_t {
    fn drop(&mut self) {
        unsafe { mradm_osc_receiver_destroy(self.receiver) };
    }
}

/// A receiver text, released on drop.
struct Owned(*mut OscText);

impl Drop for Owned {
    fn drop(&mut self) {
        unsafe { mradm_osc_text_destroy(self.0) };
    }
}

impl Owned {
    fn bytes(&self) -> &[u8] {
        let mut data = ptr::null();
        let len = unsafe { mradm_osc_text_view(self.0, &mut data) };
        if len == 0 {
            &[]
        } else {
            unsafe { std::slice::from_raw_parts(data, len) }
        }
    }
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
    if !source.is_empty()
        && unsafe { mradm_osc_valid_source_id(source.as_ptr(), source.len()) } == 0
    {
        return INVALID_ARGUMENT;
    }
    guard::code(|| {
        let port = config.map_or(9000, |c| c.listen_port) as u16;
        let receiver = unsafe { mradm_osc_receiver_create(port, source.as_ptr(), source.len()) };
        if receiver.is_null() {
            return INTERNAL;
        }
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
    guard::code(|| {
        let mut message = ptr::null_mut();
        let code = unsafe { mradm_osc_receiver_start(r.receiver, &mut message) };
        drop(Owned(message));
        crate::error::map(code)
    })
}

#[unsafe(no_mangle)]
pub unsafe extern "C" fn adm_osc_head_tracking_stop(receiver: *mut adm_osc_head_tracking_t) {
    if let Some(r) = unsafe { receiver.as_ref() } {
        guard::unit(|| unsafe { mradm_osc_receiver_stop(r.receiver) });
    }
}

/// The receiver's snapshot; `None` where the C++ threw.
fn snapshot(r: &adm_osc_head_tracking_t) -> Option<OscSnapshot> {
    let mut raw: OscSnapshot = unsafe { std::mem::zeroed() };
    (unsafe { mradm_osc_receiver_snapshot(r.receiver, &mut raw) } == 0).then_some(raw)
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
        let Some(s) = snapshot(r) else {
            return INTERNAL;
        };
        let t = &s.timing;
        *out = adm_head_tracking_pose_t {
            struct_size: size_of::<adm_head_tracking_pose_t>() as u32,
            protocol_version: t.protocol_version,
            has_pose: u32::from(s.has_pose != 0),
            fresh: u32::from(s.fresh != 0),
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
        let Some(s) = snapshot(r) else {
            return INTERNAL;
        };
        *out = adm_osc_head_tracking_status_t {
            struct_size: size_of::<adm_osc_head_tracking_status_t>() as u32,
            state: s.state,
            // `static_cast<std::uint16_t>` of the bound port.
            bound_port: u32::from(s.bound_port as u16),
            has_pose: u32::from(s.has_pose != 0),
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
            has_heartbeat: u32::from(s.has_heartbeat != 0),
            heartbeat_alive: u32::from(s.heartbeat_alive != 0),
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
        let mut text = ptr::null_mut();
        let code = unsafe { mradm_osc_receiver_snapshot_json(r.receiver, &mut text) };
        let text = Owned(text);
        if code != 0 {
            return INTERNAL;
        }
        match unsafe { owned(text.bytes(), out_json) } {
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
        let mut text = ptr::null_mut();
        let code = unsafe { mradm_osc_receiver_last_error(r.receiver, &mut text) };
        let text = Owned(text);
        if code != 0 {
            return QUERY_FAILED.as_ptr();
        }
        let mut copy = lock(&r.error_copy);
        if copy.set(text.bytes()) {
            copy.as_ptr()
        } else {
            QUERY_FAILED.as_ptr()
        }
    })
}
