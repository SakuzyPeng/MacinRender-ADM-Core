//! Private boundary of `mradm-render`: the shared DirectSpeakers, label and
//! LFE routing rules, the offline loudspeaker driver the caller steps block by block, and the VBAP
//! backend with its file stream. Inputs are borrowed for the call; a resolved matrix is owned by its handle
//! and its view lives until `mradm_render_matrix_destroy`. Errors fill the caller's message and
//! context buffers (capacity in, length written out, NUL-terminated) with the C++ bytes.
use crate::adm::Text;
use mradm_render::routing::{self, Error, LfeTarget, MatrixRoute, Override};
use std::{
    panic::{AssertUnwindSafe, catch_unwind},
    ptr, slice,
    sync::Arc,
};

#[repr(C)]
pub struct RenderMatrixTarget {
    pub(crate) label: Text,
    pub(crate) gain: f32,
}

#[repr(C)]
pub struct RenderMatrixRoute {
    pub(crate) source_label: Text,
    pub(crate) mute: u32,
    pub(crate) targets: *const RenderMatrixTarget,
    pub(crate) targets_len: usize,
}

#[repr(C)]
pub struct RenderOverride {
    pub object_id: Text,
    pub speaker_label: Text,
    pub mute: u32,
    pub gain_db: f32,
    pub has_head_locked: u32,
    pub head_locked: u32,
}

/// level: 1 info, 2 warning (`adm::LogLevel`); the message is static text.
#[repr(C)]
#[derive(Clone, Copy)]
pub struct RenderLogLine {
    level: u32,
    message: Text,
}

#[repr(C)]
pub struct RenderLfeRouting {
    applies_to_22_2: u32,
    split_power: u32,
    has_lfe1: u32,
    has_lfe2: u32,
    logs: [RenderLogLine; 2],
    logs_len: usize,
}

/// A resolved DirectSpeakers matrix with the records its view points into.
pub(crate) fn bad(message: &str) -> Error {
    Error {
        code: 1,
        message: message.as_bytes().to_vec(),
        context: Vec::new(),
    }
}

pub(crate) unsafe fn input<'a, T>(data: *const T, len: usize) -> Result<&'a [T], Error> {
    if len == 0 {
        return Ok(&[]);
    }
    if data.is_null()
        || !(data as usize).is_multiple_of(align_of::<T>())
        || len > isize::MAX as usize / size_of::<T>()
    {
        return Err(bad("无效渲染缓冲区"));
    }
    Ok(unsafe { slice::from_raw_parts(data, len) })
}

pub(crate) unsafe fn bytes<'a>(text: &Text) -> Result<&'a [u8], Error> {
    unsafe { input(text.data, text.len) }
}

/// Copies `detail` into `out` (capacity `*len`), NUL-terminated, and stores the length written.
unsafe fn write(detail: &[u8], out: *mut u8, len: *mut usize) {
    if len.is_null() {
        return;
    }
    let capacity = unsafe { *len };
    let written = if out.is_null() || capacity == 0 {
        0
    } else {
        let n = detail.len().min(capacity - 1);
        unsafe {
            ptr::copy_nonoverlapping(detail.as_ptr(), out, n);
            *out.add(n) = 0;
        }
        n
    };
    unsafe {
        *len = written;
    }
}

pub(crate) fn boundary(
    message: *mut u8,
    message_len: *mut usize,
    context: *mut u8,
    context_len: *mut usize,
    f: impl FnOnce() -> Result<(), Error>,
) -> i32 {
    let error = match catch_unwind(AssertUnwindSafe(f)) {
        Ok(Ok(())) => None,
        Ok(Err(e)) => Some(e),
        Err(_) => Some(Error {
            code: 6,
            message: b"Rust render panic".to_vec(),
            context: Vec::new(),
        }),
    };
    let (detail, location) = error
        .as_ref()
        .map_or((&[][..], &[][..]), |e| (&e.message[..], &e.context[..]));
    unsafe {
        write(detail, message, message_len);
        write(location, context, context_len);
    }
    error.map_or(0, |e| e.code)
}

unsafe fn override_value<'a>(o: &RenderOverride) -> Result<Override<'a>, Error> {
    Ok(Override {
        object_id: unsafe { bytes(&o.object_id)? },
        speaker_label: unsafe { bytes(&o.speaker_label)? },
        mute: o.mute != 0,
        gain_db: o.gain_db,
        head_locked: (o.has_head_locked != 0).then_some(o.head_locked != 0),
    })
}

/// The live overrides of one input channel; `key` is its canonical speaker label (empty for
/// Objects and HOA channels). Bit 1: a gain multiplier was written; bit 2: an explicit head lock
/// (0 or 1) was written; otherwise the channel inherits. Null output pointers skip that field.
/// Valid queries borrow the records and do not allocate, even for noncanonical labels.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn mradm_render_live_override(
    values: *const RenderOverride,
    values_len: usize,
    object_id: Text,
    key: Text,
    gain: *mut f32,
    head_locked: *mut u32,
) -> u32 {
    catch_unwind(|| {
        let (Ok(o), Ok(id), Ok(key)) = (
            unsafe { input(values, values_len) },
            unsafe { bytes(&object_id) },
            unsafe { bytes(&key) },
        ) else {
            return 0;
        };
        let Ok((g, h)) = routing::try_live_override(
            o.iter().map(|o| unsafe { override_value(o) }),
            id,
            key,
            !gain.is_null(),
            !head_locked.is_null(),
        ) else {
            return 0;
        };
        let mut flags = 0;
        if let (Some(g), Some(out)) = (g, unsafe { gain.as_mut() }) {
            *out = g;
            flags |= 1;
        }
        if let (Some(h), Some(out)) = (h, unsafe { head_locked.as_mut() }) {
            *out = u32::from(h);
            flags |= 2;
        }
        flags
    })
    .unwrap_or(0)
}

/// The gain from one LFE input target to one 22.2 LFE output (0, 1, 2) under `routing`.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn mradm_render_lfe_gain(
    routing: *const RenderLfeRouting,
    input_target: u32,
    output_target: u32,
) -> f32 {
    let Some(r) = (unsafe { routing.as_ref() }) else {
        return 0.;
    };
    let target = |t| match t {
        1 => LfeTarget::Lfe1,
        2 => LfeTarget::Lfe2,
        _ => LfeTarget::None,
    };
    routing::LfeRouting {
        applies_to_22_2: r.applies_to_22_2 != 0,
        split_power: r.split_power != 0,
        has_lfe1: r.has_lfe1 != 0,
        has_lfe2: r.has_lfe2 != 0,
    }
    .gain(target(input_target), target(output_target))
}

/// A prepared backend (`mradm-engine::backend`) and the Apple island's preparation a render made,
/// for a preview's later windows.
pub struct RenderPrepared {
    pub(crate) prepared: Option<Arc<mradm_engine::backend::Prepared>>,
    /// The Apple island's preparation (a render's cache only).
    pub(crate) apple: Option<Arc<apple_island::Preparation>>,
}

pub(crate) fn room_polar(
    azimuth: f32,
    elevation: f32,
    distance: f32,
) -> mradm_dsp::Result<[f32; 3]> {
    // The former C++ preparation used mradm_room_polar: only this conversion enters the
    // ROOM environment. Preserve that scope for the Rust event table's gain/offset arithmetic.
    let _environment = crate::room_environment::Environment::enter();
    mradm_dsp::room::loading::polar(azimuth, elevation, distance)
}

/// A borrowed DirectSpeakers matrix.
pub(crate) unsafe fn matrix_routes<'a>(
    data: *const RenderMatrixRoute,
    len: usize,
) -> Result<Vec<MatrixRoute<'a>>, Error> {
    unsafe { input(data, len)? }
        .iter()
        .map(|m| {
            Ok(MatrixRoute {
                source_label: unsafe { bytes(&m.source_label)? },
                mute: m.mute != 0,
                targets: unsafe { input(m.targets, m.targets_len)? }
                    .iter()
                    .map(|t| Ok((unsafe { bytes(&t.label)? }, t.gain)))
                    .collect::<Result<_, Error>>()?,
            })
        })
        .collect()
}

/// A live edit (`LiveObjectOverride`): the common override and the topology scales.
#[repr(C)]
pub struct RenderLiveOverride {
    pub base: RenderOverride,
    pub extent_scale: f32,
    pub extent_width_scale: f32,
    pub extent_height_scale: f32,
    pub extent_depth_scale: f32,
    pub diffuse_scale: f32,
    pub divergence_scale: f32,
}

#[path = "engine_scene.rs"]
pub mod engine_scene;
#[path = "engine_scene_output.rs"]
pub mod engine_scene_output;

#[path = "render_room.rs"]
pub mod room_ffi;

// The binaural file graph's allocation test (the allocation counter lives in this crate).
#[cfg(test)]
#[path = "render_binaural_test.rs"]
mod binaural_allocations;

#[path = "render_stream.rs"]
mod stream_ffi;

#[path = "apple_island.rs"]
pub mod apple_island;
#[path = "device_island.rs"]
pub mod device_island;
#[path = "engine_monitor.rs"]
pub mod engine_monitor;
#[path = "engine_monitor_session.rs"]
pub mod engine_monitor_session;

#[cfg(test)]
#[path = "../../mradm-render/tests/common/mod.rs"]
mod test_common;

#[cfg(all(test, any(target_arch = "x86_64", target_arch = "aarch64")))]
#[path = "render_room_test.rs"]
mod room_tests;

#[cfg(test)]
mod tests {
    use super::*;
    use crate::test_alloc::COUNT;

    fn record(id: &[u8], label: &[u8], gain_db: f32, head: Option<bool>) -> RenderOverride {
        RenderOverride {
            object_id: Text::bytes(id),
            speaker_label: Text::bytes(label),
            mute: 0,
            gain_db,
            has_head_locked: u32::from(head.is_some()),
            head_locked: u32::from(head.unwrap_or(false)),
        }
    }

    #[test]
    fn borrowed_live_queries_do_not_allocate_and_select_fields_independently() {
        let long_label = b"_x".repeat(256);
        let long_key = b"X".repeat(256);
        let mut records: Vec<_> = (0..128)
            .map(|_| record(b"other", b"m_+030", -6., Some(false)))
            .collect();
        records.extend([
            record(b"bed", b"", -12., Some(false)),
            record(b"bed", b"", 0., Some(true)),
            record(b"bed", b"M+030", -12., Some(false)),
            record(b"bed", b"m_+030 \xc3\xa9", -6., Some(true)),
            record(b"bed", b"M+030", -6., None),
            record(b"bed", b"M-030", 0., Some(false)),
            record(b"bed", &long_label, 0., Some(false)),
        ]);
        // A gain-only channel mute must not shadow the last explicit head-lock value.
        records[132].mute = 1;
        let cases: &[(&[u8], f32, u32)] = &[
            (b"M+030", 0., 1),
            (b"M-030", 1., 0),
            (b"", 1., 1),
            (b"L", 1., 1), // live labels are canonicalized, not alias-resolved
            (&long_key, 1., 0),
        ];
        let mut correct = true;
        COUNT.with(|c| c.set(Some(0)));
        for _ in 0..100 {
            for &(key, expected_gain, expected_head) in cases {
                for fields in 0..4 {
                    let (mut gain, mut head) = (17., 19);
                    let flags = unsafe {
                        mradm_render_live_override(
                            records.as_ptr(),
                            records.len(),
                            Text::bytes(b"bed"),
                            Text::bytes(key),
                            if fields & 1 != 0 {
                                &mut gain
                            } else {
                                ptr::null_mut()
                            },
                            if fields & 2 != 0 {
                                &mut head
                            } else {
                                ptr::null_mut()
                            },
                        )
                    };
                    correct &= flags == fields
                        && gain == if fields & 1 != 0 { expected_gain } else { 17. }
                        && head == if fields & 2 != 0 { expected_head } else { 19 };
                }
            }
        }
        let allocations = COUNT.with(|c| c.replace(None).unwrap());
        assert!(correct, "live precedence and nullable outputs");
        assert_eq!(
            allocations, 0,
            "including the first lookup and label folding"
        );
    }

    #[test]
    fn live_queries_preserve_outputs_when_a_later_record_is_invalid() {
        let mut records = [
            record(b"bed", b"", 0., Some(true)),
            record(b"other", b"", 0., None),
        ];
        records[1].speaker_label = Text {
            data: ptr::null(),
            len: 1,
        };
        let (mut gain, mut head) = (17., 19);
        let flags = unsafe {
            mradm_render_live_override(
                records.as_ptr(),
                records.len(),
                Text::bytes(b"bed"),
                Text::bytes(b"M+030"),
                &mut gain,
                &mut head,
            )
        };
        assert_eq!((flags, gain, head), (0, 17., 19));
        records[1] = record(b"other", b"", 0., None);
        let flags = unsafe {
            mradm_render_live_override(
                records.as_ptr(),
                records.len(),
                Text::bytes(b"missing"),
                Text::bytes(b"M+030"),
                &mut gain,
                &mut head,
            )
        };
        assert_eq!((flags, gain, head), (0, 17., 19));
    }
}
