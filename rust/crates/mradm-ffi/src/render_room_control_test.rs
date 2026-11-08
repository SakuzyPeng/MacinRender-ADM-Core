//! ROOM control queries stay independent of the worker and of the status message buffer.
use super::super::test_common as common;

use super::super::stream_ffi::{
    EngineStream, body, mradm_engine_stream_destroy, mradm_engine_stream_validate,
};
use super::*;
use crate::room::scope;
use mradm_dsp::room::loading;
use mradm_engine::stream::{Body, Validator};
use mradm_render::room;
use mradm_render::room_runtime::Resources;
use mradm_render::room_stream::FileStream;
use std::mem::MaybeUninit;
use std::sync::{
    Barrier,
    atomic::{AtomicPtr, Ordering},
};

struct Fixture {
    stream: *mut EngineStream,
    control: *mut RenderRoomControl,
    profile: *mut RoomProfileHandle,
    _input: common::Temp,
}

impl Fixture {
    fn new() -> Self {
        let manifest = concat!(
            env!("CARGO_MANIFEST_DIR"),
            "/../../../tests/fixtures/room/phase2/room.json"
        );
        let mut profile = ptr::null_mut();
        assert_eq!(
            unsafe {
                crate::room::mradm_room_profile_load(
                    manifest.as_ptr(),
                    manifest.len(),
                    &mut profile,
                    ptr::null_mut(),
                    0,
                )
            },
            0
        );
        let package = unsafe { package(profile) }.unwrap();
        let set = loading::prepare(&package, b"", b"", None, Quality::Exact).unwrap();
        let config = Config {
            package,
            hrir: String::new(),
            metres: 1.,
            quality: Quality::Exact,
        };
        let input = common::input(&common::sine(4096, 0.5));
        let scene = common::scene(
            common::single(
                "AO_1001",
                1.,
                common::TrackSpec {
                    blocks: vec![common::object_block(0, u64::MAX, 30.)],
                    direct: Vec::new(),
                },
            ),
            4096,
        );
        let table = room::prepare(
            &room::Request {
                scene: &scene,
                sample_rate: common::RATE,
                layout_id: b"binaural",
                spread: room::Spread::Automatic,
                routing: room::Routing::Automatic,
                has_matrix: false,
                default_interp_ms: 5,
                screens: room::Screens::default(),
            },
            &mut |_, _| {},
            loading::polar,
        )
        .unwrap();
        let stream = FileStream::open(
            input.path(),
            Arc::new(table),
            Arc::new(Resources { set, cuda: false }),
            config,
            [0.; 3],
            scope,
            switch(),
        )
        .unwrap();
        let control = control_handle(stream.control());
        Self {
            stream: Box::into_raw(Box::new(EngineStream::new(
                Validator::Room(scope),
                Body::Room(Box::new(stream)),
            ))),
            control,
            profile,
            _input: input,
        }
    }
}

impl Drop for Fixture {
    fn drop(&mut self) {
        // Every scoped worker/control call has joined before destruction.
        unsafe {
            mradm_room_control_destroy(self.control);
            mradm_engine_stream_destroy(self.stream);
            crate::room::mradm_room_profile_destroy(self.profile);
        }
    }
}

fn validate(handle: *const EngineStream) {
    let mut edit = RenderLiveOverride {
        base: RenderOverride {
            object_id: Text::bytes(b"AO_1001"),
            speaker_label: Text::bytes(b""),
            mute: 0,
            gain_db: -6.,
            has_head_locked: 0,
            head_locked: 0,
        },
        extent_scale: 0.5,
        extent_width_scale: 1.,
        extent_height_scale: 1.,
        extent_depth_scale: 1.,
        diffuse_scale: 1.,
        divergence_scale: 1.,
    };
    let mut message = [0u8; 128];
    let mut context = [0u8; 16];
    for expected in [0, 1] {
        let (mut size, mut context_len) = (message.len(), context.len());
        assert_eq!(
            unsafe {
                mradm_engine_stream_validate(
                    handle,
                    &edit,
                    1,
                    message.as_mut_ptr(),
                    &mut size,
                    context.as_mut_ptr(),
                    &mut context_len,
                )
            },
            expected
        );
        assert_eq!(context_len, 0);
        if expected == 0 {
            assert_eq!(size, 0);
        } else {
            assert_eq!(
                &message[..size],
                "房间 extent 比例必须有限且非负".as_bytes()
            );
        }
        edit.extent_height_scale = -1.;
    }
}

fn render_seeks(worker: &mut Body) -> Vec<u32> {
    let mut bits = Vec::new();
    for start in [0, 1023, 2049, 17] {
        if worker.seek_begin(start, false).unwrap() {
            while worker.seek_step(false).unwrap() {}
        }
        for frames in [7, 333, 512] {
            let mut output = vec![0.; frames * 2];
            assert_eq!(worker.process(&mut output, frames).unwrap(), frames);
            bits.extend(output.into_iter().map(f32::to_bits));
        }
    }
    bits
}

#[test]
fn validation_can_overlap_worker_seeks_and_processing() {
    let fixture = Fixture::new();
    let worker = unsafe { body(fixture.stream) }.unwrap();
    let expected = render_seeks(worker);
    let handle = AtomicPtr::new(fixture.stream);
    let start = Barrier::new(2);
    std::thread::scope(|threads| {
        threads.spawn(|| {
            start.wait();
            for _ in 0..64 {
                validate(handle.load(Ordering::Relaxed));
                std::thread::yield_now();
            }
        });
        start.wait();
        for _ in 0..8 {
            assert_eq!(render_seeks(worker), expected);
        }
    });
}

fn request(handle: *const RenderRoomControl, profile: *const RoomProfileHandle) -> u64 {
    let config = RenderRoomConfig {
        profile,
        hrir: Text::bytes(b""),
        metres: 1.,
        quality: 0,
    };
    let mut revision = 0;
    assert_eq!(
        unsafe {
            mradm_room_control_request(
                handle,
                &config,
                &mut revision,
                ptr::null_mut(),
                ptr::null_mut(),
                ptr::null_mut(),
                ptr::null_mut(),
            )
        },
        0
    );
    revision
}

fn source(handle: *const RenderRoomControl) {
    let mut out = MaybeUninit::uninit();
    unsafe {
        mradm_room_control_source(handle, 1, out.as_mut_ptr());
        assert_eq!(out.assume_init().id, 1);
    }
}

fn status(control: &Control, handle: *mut RenderRoomControl) {
    // Keep this shared field borrow live through the status call. No enclosing &mut handle
    // may be created while it is protected, even though the message field may be written.
    let before = control.status().requested_revision;
    let mut out = MaybeUninit::uninit();
    unsafe {
        mradm_room_control_status(handle, out.as_mut_ptr());
        let out = out.assume_init();
        assert!(out.requested_revision >= before);
        assert!(out.requested_revision <= control.status().requested_revision);
        assert!(out.applied_revision <= out.requested_revision);
        assert!(std::str::from_utf8(bytes(&out.message).unwrap()).is_ok());
    }
}

#[test]
fn room_status_and_requests_borrow_disjoint_handle_fields() {
    let fixture = Fixture::new();
    let handle = fixture.control;
    let control = unsafe { room_control(handle) }.unwrap();
    status(control, handle);
    fn query_with_message(
        message: &mut Vec<u8>,
        handle: *const RenderRoomControl,
        profile: *const RoomProfileHandle,
    ) {
        *message = b"retained message".to_vec();
        assert_eq!(request(handle, profile), 1);
        source(handle);
        assert_eq!(message, b"retained message");
    }
    query_with_message(unsafe { &mut (*handle).message }, handle, fixture.profile);
    let handle = AtomicPtr::new(handle);
    let profile = AtomicPtr::new(fixture.profile);
    let start = Barrier::new(2);
    std::thread::scope(|threads| {
        threads.spawn(|| {
            start.wait();
            for revision in 2..=33 {
                assert_eq!(
                    request(
                        handle.load(Ordering::Relaxed),
                        profile.load(Ordering::Relaxed)
                    ),
                    revision
                );
                source(handle.load(Ordering::Relaxed));
                std::thread::yield_now();
            }
        });
        start.wait();
        for _ in 0..128 {
            status(control, handle.load(Ordering::Relaxed));
        }
    });
    assert_eq!(control.status().requested_revision, 33);
}
