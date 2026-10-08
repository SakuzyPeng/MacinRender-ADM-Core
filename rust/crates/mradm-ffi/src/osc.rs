use crate::{boundary, slice};
use mradm_dsp::Error;
use mradm_osc as osc;
use std::panic::{AssertUnwindSafe, catch_unwind};

/// Same field order as `mradm::HeadTrackingTiming`.
#[repr(C)]
#[derive(Clone, Copy)]
pub struct OscTiming {
    protocol_version: u32,
    sample_time_kind: u32,
    instance_id: u64,
    tx_sequence: u64,
    reference_epoch: u64,
    metadata_revision: u64,
    source_age_at_send_ns: u64,
    source_session_id: u64,
    source_sequence: u64,
    source_received_ns: u64,
    sample_time_ms: u64,
    sample_clock_epoch: u64,
}

impl From<osc::Timing> for OscTiming {
    fn from(t: osc::Timing) -> Self {
        Self {
            protocol_version: t.protocol_version,
            sample_time_kind: t.sample_time_kind,
            instance_id: t.instance_id,
            tx_sequence: t.tx_sequence,
            reference_epoch: t.reference_epoch,
            metadata_revision: t.metadata_revision,
            source_age_at_send_ns: t.source_age_at_send_ns,
            source_session_id: t.source_session_id,
            source_sequence: t.source_sequence,
            source_received_ns: t.source_received_ns,
            sample_time_ms: t.sample_time_ms,
            sample_clock_epoch: t.sample_clock_epoch,
        }
    }
}

impl From<OscTiming> for osc::Timing {
    fn from(t: OscTiming) -> Self {
        Self {
            protocol_version: t.protocol_version,
            sample_time_kind: t.sample_time_kind,
            instance_id: t.instance_id,
            tx_sequence: t.tx_sequence,
            reference_epoch: t.reference_epoch,
            metadata_revision: t.metadata_revision,
            source_age_at_send_ns: t.source_age_at_send_ns,
            source_session_id: t.source_session_id,
            source_sequence: t.source_sequence,
            source_received_ns: t.source_received_ns,
            sample_time_ms: t.sample_time_ms,
            sample_clock_epoch: t.sample_clock_epoch,
        }
    }
}

/// kind: 0 pose, 1 info, 2 status, 3 incompatible. Telemetry source ids come from JSON
/// escapes, so they are copied; the JSON text stays in the caller's datagram.
#[repr(C)]
pub struct OscMessage {
    kind: u32,
    source_active: u32,
    quaternion_xyzw: [f32; 4],
    euler_deg: [f32; 3],
    timing: OscTiming,
    message_sequence: u64,
    reported_samples: u64,
    source_id_len: usize,
    source_id: [u8; osc::MAX_SOURCE_ID],
    json_offset: usize,
    json_len: usize,
}

/// Plain state owned by C++; all-zero is the initial state.
#[repr(C)]
#[derive(Clone, Copy)]
pub struct OscSourceOrder {
    last: OscTiming,
    clock: OscTiming,
    retired: [u64; 16],
    retired_next: usize,
    last_gap: u64,
}

impl From<OscSourceOrder> for osc::SourceOrder {
    fn from(o: OscSourceOrder) -> Self {
        Self {
            last: o.last.into(),
            clock: o.clock.into(),
            retired: o.retired,
            retired_next: o.retired_next,
            last_gap: o.last_gap,
        }
    }
}

impl From<osc::SourceOrder> for OscSourceOrder {
    fn from(o: osc::SourceOrder) -> Self {
        Self {
            last: o.last.into(),
            clock: o.clock.into(),
            retired: o.retired,
            retired_next: o.retired_next,
            last_gap: o.last_gap,
        }
    }
}

/// Returns 0 with `out` filled for a decoded datagram, nonzero for any rejected input.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn mradm_osc_decode(
    data: *const u8,
    len: usize,
    out: *mut OscMessage,
) -> i32 {
    boundary(std::ptr::null_mut(), 0, || {
        if out.is_null() || (data.is_null() && len != 0) {
            return Err(Error::InvalidArgument("Null OSC argument"));
        }
        let bytes = if len == 0 {
            &[][..]
        } else {
            unsafe { slice::from_raw_parts(data, len) }
        };
        let msg = osc::decode(bytes).ok_or(Error::InvalidArgument("Rejected OSC datagram"))?;
        let mut source_id = [0u8; osc::MAX_SOURCE_ID];
        source_id[..msg.source_id.len()].copy_from_slice(msg.source_id.as_bytes());
        let result = OscMessage {
            kind: match msg.kind {
                osc::Kind::Pose => 0,
                osc::Kind::Info => 1,
                osc::Kind::Status => 2,
                osc::Kind::Incompatible => 3,
            },
            source_active: u32::from(msg.source_active),
            quaternion_xyzw: msg.quaternion_xyzw,
            euler_deg: msg.euler_deg,
            timing: msg.timing.into(),
            message_sequence: msg.message_sequence,
            reported_samples: msg.reported_samples,
            source_id_len: msg.source_id.len(),
            source_id,
            json_offset: msg.json.start,
            json_len: msg.json.len(),
        };
        unsafe { out.write(result) };
        Ok(())
    })
}

fn guarded(f: impl FnOnce() -> bool) -> u32 {
    catch_unwind(AssertUnwindSafe(f)).map_or(0, u32::from)
}

#[unsafe(no_mangle)]
pub unsafe extern "C" fn mradm_osc_valid_source_id(data: *const u8, len: usize) -> u32 {
    guarded(|| !data.is_null() && osc::valid_source_id(unsafe { slice::from_raw_parts(data, len) }))
}

#[unsafe(no_mangle)]
pub unsafe extern "C" fn mradm_osc_source_order_accept(
    order: *mut OscSourceOrder,
    timing: *const OscTiming,
) -> u32 {
    guarded(|| {
        if order.is_null() || timing.is_null() {
            return false;
        }
        let mut state = osc::SourceOrder::from(unsafe { *order });
        let accepted = state.accept(&unsafe { *timing }.into());
        unsafe { *order = state.into() };
        accepted
    })
}

#[unsafe(no_mangle)]
pub unsafe extern "C" fn mradm_osc_source_order_retired(
    order: *const OscSourceOrder,
    instance_id: u64,
) -> u32 {
    guarded(|| !order.is_null() && osc::SourceOrder::from(unsafe { *order }).retired(instance_id))
}
