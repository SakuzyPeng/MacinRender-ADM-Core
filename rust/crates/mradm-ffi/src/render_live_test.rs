//! Count allocations through the Scene renderer FFI, including the PCM transport views.
use super::*;
use crate::test_alloc::COUNT;

const FRAMES: usize = 17;

struct Renderer(*mut RenderLive);

impl Renderer {
    fn new(triple_balance: bool, layout: &[u8]) -> Self {
        let mut handle = ptr::null_mut();
        let code = if triple_balance {
            let request = RenderLiveTripleBalanceRequest {
                layout_id: Text::bytes(layout),
                geometry: 0,
                spread: 0,
                has_sofa: 0,
                sample_rate: 48000,
                split_power: 0,
                smoothing_frames: 0,
            };
            unsafe {
                mradm_render_live_triple_balance_create(
                    &request,
                    &mut handle,
                    ptr::null_mut(),
                    ptr::null_mut(),
                )
            }
        } else {
            let request = RenderLiveVbapRequest {
                layout_id: Text::bytes(layout),
                geometry: 0,
                spread: 0,
                split_power: 0,
                smoothing_frames: 0,
            };
            unsafe {
                mradm_render_live_vbap_create(
                    &request,
                    &mut handle,
                    ptr::null_mut(),
                    ptr::null_mut(),
                )
            }
        };
        assert_eq!(code, 0, "{triple_balance} {layout:?}");
        let elements = [1, 2].map(|element_id| RenderLiveElement {
            element_id,
            role: 0,
            speaker_label: Text::bytes(b""),
            object_id: Text::bytes(b""),
            has_position: 0,
            position: [0., 1., 0.],
        });
        assert_eq!(
            unsafe {
                mradm_render_live_configure(
                    handle,
                    1,
                    elements.as_ptr(),
                    elements.len(),
                    ptr::null_mut(),
                    ptr::null_mut(),
                    ptr::null_mut(),
                    ptr::null_mut(),
                    ptr::null_mut(),
                    ptr::null_mut(),
                )
            },
            0
        );
        Self(handle)
    }

    unsafe fn render(&self, frame: &RenderLiveFrame, output: &mut [f32]) -> i32 {
        let mut diagnostics = ptr::null();
        let mut count = 0;
        unsafe {
            mradm_render_live_render(
                self.0,
                frame,
                output.as_mut_ptr(),
                output.len(),
                &mut diagnostics,
                &mut count,
                ptr::null_mut(),
                ptr::null_mut(),
                ptr::null_mut(),
                ptr::null_mut(),
            )
        }
    }
}

impl Drop for Renderer {
    fn drop(&mut self) {
        unsafe { mradm_render_live_destroy(self.0) };
    }
}

fn frame(planes: &[RenderLivePlane]) -> RenderLiveFrame {
    RenderLiveFrame {
        generation: 1,
        duration: FRAMES as u32,
        planes: planes.as_ptr(),
        planes_len: planes.len(),
        initial: ptr::null(),
        initial_len: 0,
        updates: ptr::null(),
        updates_len: 0,
        trace: Text::bytes(b""),
    }
}

#[test]
fn warmed_scene_ffi_pcm_views_and_reset_do_not_allocate() {
    for triple_balance in [true, false] {
        for layout in [b"4+7+0".as_slice(), b"9.1.6", b"9+10+3"] {
            let renderer = Renderer::new(triple_balance, layout);
            let channels = unsafe { mradm_render_live_channels(renderer.0) } as usize;
            let positive = [0.125; FRAMES];
            let negative = [-0.125; FRAMES];
            let mut planes = [
                RenderLivePlane {
                    element_id: 1,
                    has_signal: 1,
                    samples: positive.as_ptr(),
                    samples_len: FRAMES,
                },
                // Silent planes never borrow their samples.
                RenderLivePlane {
                    element_id: 2,
                    has_signal: 0,
                    samples: ptr::null(),
                    samples_len: usize::MAX,
                },
            ];
            let mut input = frame(&planes);
            let mut output = vec![19.; FRAMES * channels + 1];
            assert_eq!(unsafe { renderer.render(&input, &mut output) }, 0);
            let expected = output[..FRAMES * channels].to_vec();
            assert!(expected.iter().any(|&v| v != 0.));
            COUNT.with(|c| c.set(Some(0)));
            for i in 0..16 {
                planes[0].samples = if i % 2 == 0 {
                    positive.as_ptr()
                } else {
                    negative.as_ptr()
                };
                input.planes = planes.as_ptr();
                input.planes_len = if i % 3 == 0 { 1 } else { 2 };
                assert_eq!(unsafe { renderer.render(&input, &mut output) }, 0);
                for (actual, expected) in output.iter().zip(&expected) {
                    assert_eq!(*actual, if i % 2 == 0 { *expected } else { -*expected });
                }
                assert_eq!(output[FRAMES * channels], 19.);
            }
            unsafe { mradm_render_live_reset(renderer.0) };
            let allocations = COUNT.with(|c| c.replace(None).unwrap());
            assert_eq!(allocations, 0, "{triple_balance} {layout:?}");
        }
    }
}

#[test]
fn borrowed_scene_ffi_pcm_ranges_are_checked_before_rendering() {
    for triple_balance in [true, false] {
        let renderer = Renderer::new(triple_balance, b"4+7+0");
        let channels = unsafe { mradm_render_live_channels(renderer.0) } as usize;
        let samples = [0.125f32; FRAMES];
        let mut plane = RenderLivePlane {
            element_id: 1,
            has_signal: 1,
            samples: samples.as_ptr(),
            samples_len: FRAMES,
        };
        let mut input = frame(std::slice::from_ref(&plane));
        let mut output = vec![19.; FRAMES * channels + 1];
        assert_eq!(unsafe { renderer.render(&input, &mut output) }, 0);
        let expected = output.clone();
        let unaligned = samples.as_ptr().cast::<u8>().wrapping_add(1).cast::<f32>();
        for (pointer, length) in [
            (ptr::null(), FRAMES),
            (unaligned, FRAMES),
            (samples.as_ptr(), usize::MAX),
            (output.as_ptr(), FRAMES),
        ] {
            plane.samples = pointer;
            plane.samples_len = length;
            input.planes = ptr::from_ref(&plane);
            output.fill(19.);
            assert_eq!(unsafe { renderer.render(&input, &mut output) }, 1);
            assert!(output.iter().all(|&v| v == 19.));
        }
        plane.samples = samples.as_ptr();
        plane.samples_len = FRAMES;
        input.planes = ptr::from_ref(&plane);
        assert_eq!(unsafe { renderer.render(&input, &mut output) }, 0);
        assert_eq!(
            output, expected,
            "rejections leave the numerical history intact"
        );

        plane.has_signal = 0;
        plane.samples = unaligned;
        plane.samples_len = usize::MAX;
        input.planes = ptr::from_ref(&plane);
        assert_eq!(unsafe { renderer.render(&input, &mut output) }, 0);
        assert!(output[..FRAMES * channels].iter().all(|&v| v == 0.));
        plane.has_signal = 1;
        plane.samples = ptr::null();
        plane.samples_len = 0;
        input.planes = ptr::from_ref(&plane);
        input.duration = 0;
        assert_eq!(unsafe { renderer.render(&input, &mut []) }, 0);
    }
}
