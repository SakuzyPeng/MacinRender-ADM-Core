//! The binaural Scene renderer under real HRTF motion (formerly the C++
//! `live_binaural_continuity_test`, R3 B): head-locked audio ignores head movement, head / object /
//! gain motion keeps a 100 Hz tone free of broadband discontinuities, and convolution at an
//! off-grid direction is independent of the frame partition. The convolution, OLA, diffuse delay
//! and lookup contracts are `mradm-dsp` `binaural_convolution.rs` and `hrtf_filters.rs`.
mod common;
use common::backend::TestHost;
use mradm_dsp::room::screen::RoomScreenGeometry;
use mradm_engine::backend::{BinauralSpreadMode, RoomRequest, Screens, SpeakerSpread};
use mradm_engine::request::Selection;
use mradm_engine::scene::renderer::{Config, create};
use mradm_render::live::{Element, Entry, Frame, Plane, Role, State, Update, field};

const OMEGA: f64 = 2. * std::f64::consts::PI * 100. / 48000.;
const TOTAL: u32 = 192_000;
const START: u32 = 48_384;
const END: u32 = 144_384;
const CONTROL: u32 = 768;

#[derive(Clone, Copy, PartialEq)]
enum Motion {
    Fixed,
    Head,
    Object,
    Gain,
}

fn config() -> Config<'static> {
    Config {
        selection: Selection::SafBinaural,
        layout_id: b"binaural",
        apple_geometry: false,
        speaker_spread: SpeakerSpread::Automatic,
        binaural_spread: BinauralSpreadMode::Automatic,
        split_power: false,
        smoothing_frames: 0,
        sample_rate: 48000,
        sofa: None,
        screens: Screens {
            has_reference: false,
            reference: RoomScreenGeometry::default(),
            has_reproduction: false,
            reproduction: RoomScreenGeometry::default(),
        },
        room: RoomRequest {
            package: None,
            hrir: b"",
            metres: 0.,
            quality: 0,
        },
        sink: true,
    }
}

fn progress(sample: u32) -> f32 {
    ((sample as f32 - START as f32) / (END - START) as f32).clamp(0., 1.)
}

fn render(motion: Motion, block: u32, locked: bool, fixed_yaw: f32) -> Vec<f32> {
    let mut renderer = create(&config(), &TestHost)
        .unwrap_or_else(|e| panic!("{}", String::from_utf8_lossy(&e.message)))
        .renderer;
    let element = Element {
        id: 1,
        role: Some(Role::Object),
        speaker_label: b"",
        object_id: b"",
        has_position: true,
        position: [0., 1., 0.],
    };
    renderer.configure(1, &[element]).unwrap();
    let mut output = vec![0f32; TOTAL as usize * 2];
    let mut cursor = 0;
    while cursor < TOTAL {
        let frames = block.min(TOTAL - cursor).min(CONTROL - cursor % CONTROL);
        let yaw = if motion == Motion::Head {
            90. * progress(cursor - cursor % CONTROL)
        } else {
            fixed_yaw
        };
        renderer.pose([yaw, 0., 0.]);
        let initial = [Entry {
            element_id: 1,
            state: State {
                valid_fields: field::KNOWN,
                head_locked: locked,
                ..State::default()
            },
        }];
        let mut updates = Vec::new();
        if matches!(motion, Motion::Object | Motion::Gain)
            && (START..=END).contains(&cursor)
            && cursor % CONTROL == 0
        {
            let p = progress(cursor + CONTROL);
            let angle = p * std::f32::consts::FRAC_PI_2;
            let changed = if motion == Motion::Gain {
                field::LINEAR_GAIN
            } else {
                field::POSITION
            };
            updates.push(Update {
                element_id: 1,
                offset: 0,
                ramp: CONTROL,
                jump: false,
                changed,
                cleared: 0,
                state: State {
                    valid_fields: changed,
                    position: [angle.sin(), angle.cos(), 0.],
                    linear_gain: 1. - 0.5 * p,
                    ..State::default()
                },
            });
        }
        let samples: Vec<f32> = (0..frames)
            .map(|i| 0.1 * (OMEGA * f64::from(cursor + i)).sin() as f32)
            .collect();
        let planes = [Plane {
            element_id: 1,
            has_signal: true,
            samples: &samples,
        }];
        let frame = Frame {
            generation: 1,
            duration: frames,
            planes: &planes[..],
            initial: if cursor == 0 { &initial } else { &[] },
            updates: &updates,
            trace: b"",
        };
        let start = cursor as usize * 2;
        renderer
            .render(&frame, &mut output[start..start + frames as usize * 2])
            .unwrap();
        cursor += frames;
    }
    output
}

/// A stationary 100 Hz tone obeys x[n] = 2cos(w) x[n-2] - x[n-4] per channel; motion adds slow
/// modulation but must not add the broadband impulses an output-tail crossfade produced (~1e-2).
fn peak_tone_residual(audio: &[f32]) -> f64 {
    let c = 2. * OMEGA.cos();
    (96_000..300_000)
        .map(|i| {
            let r = f64::from(audio[i]) - c * f64::from(audio[i - 2]) + f64::from(audio[i - 4]);
            if r.is_finite() { r.abs() } else { 1. }
        })
        .fold(0., f64::max)
}

#[test]
fn head_locked_audio_ignores_head_movement() {
    assert_eq!(
        render(Motion::Fixed, 256, false, 0.),
        render(Motion::Head, 256, true, 0.)
    );
}

#[test]
fn motion_is_audible_and_free_of_discontinuities() {
    let fixed = render(Motion::Fixed, 256, false, 0.);
    for motion in [Motion::Head, Motion::Object, Motion::Gain] {
        let audio = render(motion, 256, false, 0.);
        let change = audio[96_000..]
            .iter()
            .zip(&fixed[96_000..])
            .fold(0f32, |m, (a, b)| m.max((a - b).abs()));
        let peak = audio[96_000..].iter().fold(0f32, |m, a| m.max(a.abs()));
        assert!(
            change > 1e-3 && peak > 1e-3,
            "motion must change, not freeze or mute"
        );
        let residual = peak_tone_residual(&audio);
        assert!(residual < 5e-5, "residual {residual}");
    }
}

#[test]
fn off_grid_convolution_is_independent_of_the_partition() {
    let regular = render(Motion::Fixed, 512, false, 37.);
    let irregular = render(Motion::Fixed, 137, false, 37.);
    let error = regular
        .iter()
        .zip(&irregular)
        .fold(0f32, |m, (a, b)| m.max((a - b).abs()));
    assert!(error < 2e-7, "{error}");
}
