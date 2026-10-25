//! The room event table must keep the floating-point boundary of its former C++ polar calls.
use super::*;
use mradm_wav::{Format, SampleFormat, Writer, WriterOptions};
use std::{mem::MaybeUninit, path::PathBuf};

struct Input(PathBuf);
impl Input {
    fn new() -> Self {
        let path =
            std::env::temp_dir().join(format!("mradm-room-environment-{}.wav", std::process::id()));
        let mut writer = Writer::new(
            std::fs::File::create(&path).unwrap(),
            Format {
                channels: 1,
                sample_rate: 48000,
                sample_format: SampleFormat::Float32,
                channel_mask: None,
            },
            WriterOptions::default(),
        )
        .unwrap();
        writer.write_frames(&[0.; 64]).unwrap();
        writer.finish().unwrap();
        Self(path)
    }
}
impl Drop for Input {
    fn drop(&mut self) {
        let _ = std::fs::remove_file(&self.0);
    }
}

fn control() -> u64 {
    #[cfg(target_arch = "x86_64")]
    unsafe {
        let mut value = 0u32;
        core::arch::asm!("stmxcsr [{p}]", p = in(reg) &mut value, options(nostack, preserves_flags));
        u64::from(value)
    }
    #[cfg(target_arch = "aarch64")]
    unsafe {
        let value: u64;
        core::arch::asm!("mrs {v}, fpcr", v = out(reg) value, options(nostack, preserves_flags));
        value
    }
}

fn set_control(value: u64) {
    #[cfg(target_arch = "x86_64")]
    unsafe {
        let value = value as u32;
        core::arch::asm!("ldmxcsr [{p}]", p = in(reg) &value, options(nostack, preserves_flags));
    }
    #[cfg(target_arch = "aarch64")]
    unsafe {
        core::arch::asm!("msr fpcr, {v}", v = in(reg) value, options(nostack, preserves_flags));
    }
}

struct Scene(*mut SceneHandle);
impl Drop for Scene {
    fn drop(&mut self) {
        unsafe { crate::scene::mradm_scene_destroy(self.0) };
    }
}

struct Preparation(*mut RenderPrepared);
impl Drop for Preparation {
    fn drop(&mut self) {
        unsafe { mradm_render_prepared_destroy(self.0) };
    }
}

fn positions(scene: &Scene) -> Vec<[u32; 3]> {
    let request = RenderRoomRequest {
        layout_id: Text::bytes(b"binaural"),
        sample_rate: 48000,
        spread: 0,
        routing: 0,
        has_matrix: 0,
        default_interp_ms: 5,
        has_reference: 0,
        reference: RoomScreenGeometry::default(),
        has_reproduction: 0,
        reproduction: RoomScreenGeometry::default(),
    };
    let mut prepared = Preparation(ptr::null_mut());
    let mut view = MaybeUninit::uninit();
    let code =
        unsafe { mradm_render_room_prepare(scene.0, &request, &mut prepared.0, view.as_mut_ptr()) };
    assert_eq!(code, 0);
    let room = unsafe { &*prepared.0 }.room.clone().unwrap();
    room.events
        .iter()
        .filter(|e| e.priority == 2)
        .map(|e| e.command.state.position.map(f32::to_bits))
        .collect()
}

#[test]
fn event_positions_preserve_the_room_environment_boundary() {
    let _restore = crate::room_environment::Environment::enter();
    let input = Input::new();
    let path = input.0.to_str().unwrap().as_bytes();
    let label = Text::bytes(b"M+030");
    let mut scene = Scene(ptr::null_mut());
    let mut message = [0u8; 512];
    let mut context = 0;
    assert_eq!(
        unsafe {
            crate::scene::mradm_scene_import(
                path.as_ptr(),
                path.len(),
                1,
                0,
                Text::bytes(b""),
                &label,
                1,
                &mut scene.0,
                &mut context,
                message.as_mut_ptr(),
                message.len(),
            )
        },
        0
    );
    let mut expected = [0.; 3];
    assert_eq!(
        unsafe { crate::room::mradm_room_polar(30., 0., 1., expected.as_mut_ptr()) },
        0
    );
    let expected = vec![expected.map(f32::to_bits)];
    let tiny = room_polar(41.25, 15.75, 1.0e-38).unwrap().map(f32::to_bits);
    let canonical = control();
    #[cfg(target_arch = "x86_64")]
    let (shift, flush) = (13, (1 << 15) | (1 << 6));
    #[cfg(target_arch = "aarch64")]
    let (shift, flush) = (22, (1 << 24) | (1 << 19));
    for rounding in 0..4 {
        for flush_enabled in [false, true] {
            let changed = (canonical & !((3 << shift) | flush))
                | (rounding << shift)
                | if flush_enabled { flush } else { 0 };
            set_control(changed);
            let actual = positions(&scene);
            assert_eq!(control(), changed, "restore the caller's control register");
            assert_eq!(
                actual, expected,
                "rounding {rounding}, flush {flush_enabled}"
            );
            assert_eq!(
                room_polar(41.25, 15.75, 1.0e-38).unwrap().map(f32::to_bits),
                tiny
            );
            assert!(room_polar(f32::NAN, 0., 1.).is_err());
            assert_eq!(
                control(),
                changed,
                "restore after subnormal and invalid input"
            );
        }
    }
}
