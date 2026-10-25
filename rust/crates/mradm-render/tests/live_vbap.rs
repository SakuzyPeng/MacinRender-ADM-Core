//! The Scene stream VBAP renderer: generation and frame checks, the atomic frame commit, the
//! one-per-renderer warnings, DirectSpeakers and LFE routing, and the object panning against
//! the panner and scene math it is built from.
use mradm_dsp::scene_math::{self as math, CloudPoint, Speaker as SceneSpeaker};
use mradm_dsp::vbap::{Panner, PannerSource};
use mradm_render::live::{
    Diagnostic, Element, Entry, Frame, Plane, Role, State, Update, code, field,
};
use mradm_render::live_vbap::{self, Config, Renderer};
use mradm_render::routing::LFE_SPLIT_POWER_GAIN;
use mradm_scene::layouts::{self, Geometry};

fn config(layout: &str) -> Config<'_> {
    Config {
        layout_id: layout.as_bytes(),
        geometry: Geometry::Standard,
        spread: true,
        split_power: false,
        smoothing_frames: 17,
    }
}

fn renderer(layout: &str) -> Renderer {
    Renderer::new(&config(layout)).expect("layout")
}

fn element(id: u64, role: Role, label: &'static str) -> Element<'static> {
    Element {
        id,
        role: Some(role),
        speaker_label: label.as_bytes(),
        object_id: b"",
        has_position: false,
        position: [0., 1., 0.],
    }
}

fn channel(layout: &str, label: &str) -> usize {
    layouts::find(layout, Geometry::Standard)
        .unwrap()
        .speakers
        .iter()
        .position(|s| s.label == label)
        .unwrap()
}

fn state(gain: f32) -> State {
    State {
        valid_fields: field::LINEAR_GAIN,
        linear_gain: gain,
        ..State::default()
    }
}

fn update(element_id: u64, offset: u32, changed: u64, state: State) -> Update {
    Update {
        element_id,
        offset,
        ramp: 0,
        jump: false,
        changed,
        cleared: 0,
        state: State {
            valid_fields: state.valid_fields | changed,
            ..state
        },
    }
}

/// Renders `frames` frames of a constant 1.0 on every element's plane; returns the output.
fn render(
    r: &mut Renderer,
    ids: &[u64],
    frames: u32,
    initial: &[Entry],
    updates: &[Update],
) -> Result<Vec<f32>, mradm_render::routing::Error> {
    let ones = vec![1.0f32; frames as usize];
    let planes: Vec<Plane> = ids
        .iter()
        .map(|&element_id| Plane {
            element_id,
            has_signal: true,
            samples: &ones,
        })
        .collect();
    let mut output = vec![17.0f32; frames as usize * r.output_channels() as usize];
    r.render(
        &Frame {
            generation: 0,
            duration: frames,
            planes: &planes,
            initial,
            updates,
            trace: b"",
        },
        &mut output,
    )?;
    Ok(output)
}

fn frame_at(output: &[f32], channels: usize, frame: usize) -> &[f32] {
    &output[frame * channels..(frame + 1) * channels]
}

fn text(e: &mradm_render::routing::Error) -> (i32, String, String) {
    (
        e.code,
        String::from_utf8_lossy(&e.message).into_owned(),
        String::from_utf8_lossy(&e.context).into_owned(),
    )
}

#[test]
fn layout_errors_keep_the_cxx_texts() {
    let e = Renderer::new(&config("7.7.7")).err().unwrap();
    assert_eq!(
        text(&e),
        (
            2,
            "unsupported live VBAP output layout '7.7.7'".into(),
            String::new()
        )
    );
    assert_eq!(renderer("0+5+0").output_channels(), 6);
}

#[test]
fn generation_errors_keep_the_previous_generation() {
    let mut r = renderer("0+5+0");
    r.configure(0, &[element(1, Role::Object, "")]).unwrap();
    let invalid = |elements: &[Element]| {
        let mut r = renderer("0+5+0");
        text(&r.configure(1, elements).unwrap_err()).1
    };
    let duplicate = [element(1, Role::Object, ""), element(1, Role::Lfe, "")];
    assert_eq!(
        invalid(&duplicate),
        "Live VBAP has an invalid or duplicate element"
    );
    let unknown_role = Element {
        role: None,
        ..element(1, Role::Object, "")
    };
    assert_eq!(
        invalid(&[unknown_role]),
        "Live VBAP has an invalid or duplicate element"
    );
    let bad_position = Element {
        has_position: true,
        position: [f32::NAN, 1., 0.],
        ..element(1, Role::DirectSpeaker, "M+030")
    };
    assert_eq!(
        invalid(&[bad_position]),
        "Live VBAP element position is invalid"
    );
    // A rejected generation leaves generation 0 rendering.
    assert!(r.configure(1, &duplicate).is_err());
    render(&mut r, &[1], 4, &[], &[]).unwrap();
}

#[test]
fn frame_errors_change_nothing() {
    let mut r = renderer("0+5+0");
    let mut control = renderer("0+5+0");
    let elements = [element(1, Role::Object, ""), element(2, Role::Lfe, "")];
    r.configure(0, &elements).unwrap();
    control.configure(0, &elements).unwrap();
    let seed = [update(1, 1, field::LINEAR_GAIN, state(0.1))];
    assert_eq!(
        render(&mut r, &[1, 2], 3, &[], &seed).unwrap(),
        render(&mut control, &[1, 2], 3, &[], &seed).unwrap()
    );

    let ones = [1.0f32; 3];
    let mut output = vec![17.0f32; 18];
    let attempt = |r: &mut Renderer, frame: Frame, output: &mut [f32]| {
        let e = r.render(&frame, output).unwrap_err();
        assert!(
            output.iter().all(|&v| v == 17.),
            "failed frame wrote output"
        );
        text(&e).1
    };
    let base = Frame {
        generation: 0,
        duration: 3,
        planes: &[],
        initial: &[],
        updates: &[],
        trace: b"",
    };
    assert_eq!(
        attempt(&mut r, base, &mut output[..17]),
        "live VBAP output buffer is too small"
    );
    assert_eq!(
        attempt(
            &mut r,
            Frame {
                generation: 1,
                ..base
            },
            &mut output
        ),
        "live VBAP frame uses an unconfigured generation"
    );
    let short = [Plane {
        element_id: 1,
        has_signal: true,
        samples: &ones[..2],
    }];
    let twice = [
        Plane {
            element_id: 1,
            has_signal: false,
            samples: &[],
        },
        Plane {
            element_id: 1,
            has_signal: false,
            samples: &[],
        },
    ];
    for planes in [&short[..], &twice[..]] {
        assert_eq!(
            attempt(&mut r, Frame { planes, ..base }, &mut output),
            "Live VBAP PCM plane has an invalid ID or length"
        );
    }
    let nan = State {
        valid_fields: field::POSITION,
        position: [f32::NAN, 0., 0.],
        ..State::default()
    };
    for initial in [
        vec![Entry {
            element_id: 9,
            state: state(1.),
        }],
        vec![Entry {
            element_id: 1,
            state: nan,
        }],
        vec![
            Entry {
                element_id: 1,
                state: state(1.),
            },
            Entry {
                element_id: 1,
                state: state(1.),
            },
        ],
    ] {
        assert_eq!(
            attempt(
                &mut r,
                Frame {
                    initial: &initial,
                    ..base
                },
                &mut output
            ),
            "Live VBAP initial state is invalid"
        );
    }
    let mut unknown_field = update(1, 0, field::LINEAR_GAIN, state(0.5));
    unknown_field.changed |= 1 << 20;
    let mut unset = update(1, 0, field::LINEAR_GAIN, state(0.5));
    unset.state.valid_fields = 0;
    for updates in [
        vec![update(9, 0, field::LINEAR_GAIN, state(0.5))],
        vec![update(1, 3, field::LINEAR_GAIN, state(0.5))],
        vec![
            update(1, 2, field::LINEAR_GAIN, state(0.5)),
            update(1, 1, field::LINEAR_GAIN, state(0.5)),
        ],
        vec![unknown_field],
        vec![unset],
        vec![update(1, 0, field::LINEAR_GAIN, state(-1.))],
        // A valid update before a late invalid one is not applied either.
        vec![
            update(1, 0, field::LINEAR_GAIN, state(0.9)),
            update(9, 2, field::LINEAR_GAIN, state(0.5)),
        ],
    ] {
        assert_eq!(
            attempt(
                &mut r,
                Frame {
                    updates: &updates,
                    ..base
                },
                &mut output
            ),
            "Live VBAP metadata update is invalid"
        );
    }
    assert_eq!(
        render(&mut r, &[1, 2], 3, &[], &[]).unwrap(),
        render(&mut control, &[1, 2], 3, &[], &[]).unwrap()
    );
}

#[test]
fn early_frame_errors_clear_previous_diagnostics() {
    for wrong_generation in [false, true] {
        let mut r = renderer("0+2+0");
        r.configure(0, &[element(1, Role::Lfe, "LFE1")]).unwrap();
        render(&mut r, &[1], 2, &[], &[]).unwrap();
        assert_eq!(r.diagnostics().len(), 1);
        assert_eq!(r.diagnostics()[0].code, code::MISSING_LFE_OUTPUT);

        let mut output = [17.; 4];
        let error = r
            .render(
                &Frame {
                    generation: u64::from(wrong_generation),
                    duration: 2,
                    planes: &[],
                    initial: &[],
                    updates: &[],
                    trace: b"",
                },
                if wrong_generation {
                    &mut output
                } else {
                    &mut output[..3]
                },
            )
            .unwrap_err();
        assert_eq!(
            text(&error).1,
            if wrong_generation {
                "live VBAP frame uses an unconfigured generation"
            } else {
                "live VBAP output buffer is too small"
            }
        );
        assert_eq!(output, [17.; 4]);
        assert!(
            r.diagnostics().is_empty(),
            "failed frame retained a warning"
        );
        // Clearing the published list must not reset the generation's warning keys.
        render(&mut r, &[1], 2, &[], &[]).unwrap();
        assert!(r.diagnostics().is_empty());
    }
}

#[test]
fn unroutable_direct_speakers_fail_without_publishing_warnings() {
    let mut r = renderer("0+5+0");
    let elements = [
        element(91, Role::Object, ""),
        element(7, Role::DirectSpeaker, "unknown-label"),
    ];
    r.configure(0, &elements).unwrap();
    let diffuse = State {
        valid_fields: field::DIFFUSE,
        diffuse: 0.5,
        ..State::default()
    };
    let initial = [Entry {
        element_id: 91,
        state: diffuse,
    }];
    let e = render(&mut r, &[91, 7], 3, &initial, &[]).unwrap_err();
    assert_eq!(
        text(&e),
        (
            2,
            "DirectSpeakers element 7 has neither a routable label nor position".into(),
            String::new()
        )
    );
    assert!(r.diagnostics().is_empty());
    // The DirectSpeakers element routes once it has a position; the object's warning was not
    // consumed by the failed frame.
    let positioned = State {
        valid_fields: field::POSITION,
        position: [0.2, 0.9, 0.],
        ..State::default()
    };
    let initial = [
        Entry {
            element_id: 7,
            state: positioned,
        },
        Entry {
            element_id: 91,
            state: diffuse,
        },
    ];
    render(&mut r, &[91, 7], 3, &initial, &[]).unwrap();
    assert_eq!(
        r.diagnostics(),
        [
            Diagnostic {
                code: code::DIRECT_SPEAKER_FALLBACK,
                element_id: 7,
                field: 0,
                message: live_vbap::FALLBACK_WARNING,
            },
            Diagnostic {
                code: code::SEMANTIC_DEGRADED,
                element_id: 91,
                field: field::DIFFUSE,
                message: live_vbap::DIFFUSE_WARNING,
            },
        ]
    );
}

#[test]
fn warnings_are_once_per_field_for_the_generation() {
    let mut r = renderer("4+5+0");
    let elements = [element(1, Role::Object, ""), element(2, Role::Object, "")];
    r.configure(0, &elements).unwrap();
    let all = State {
        valid_fields: field::DIFFUSE | field::SCREEN_REFERENCE | field::HEAD_LOCKED,
        diffuse: 0.5,
        screen_reference: true,
        head_locked: true,
        ..State::default()
    };
    let initial = [
        Entry {
            element_id: 1,
            state: all,
        },
        Entry {
            element_id: 2,
            state: all,
        },
    ];
    render(&mut r, &[1, 2], 2, &initial, &[]).unwrap();
    let messages: Vec<_> = r
        .diagnostics()
        .iter()
        .map(|d| (d.element_id, d.message))
        .collect();
    assert_eq!(
        messages,
        [
            (1, live_vbap::DIFFUSE_WARNING),
            (1, live_vbap::SCREEN_WARNING),
            (1, live_vbap::HEAD_LOCKED_WARNING),
        ]
    );
    render(&mut r, &[1, 2], 2, &initial, &[]).unwrap();
    assert!(r.diagnostics().is_empty());
    // A new generation warns again.
    r.configure(0, &elements).unwrap();
    render(&mut r, &[1, 2], 2, &initial, &[]).unwrap();
    assert_eq!(r.diagnostics().len(), 3);
    // A diffuse value at the threshold does not warn.
    let mut r = renderer("4+5+0");
    r.configure(0, &elements[..1]).unwrap();
    let quiet = [Entry {
        element_id: 1,
        state: State {
            diffuse: 1.0e-4,
            ..all
        },
    }];
    render(&mut r, &[1], 2, &quiet, &[]).unwrap();
    assert!(
        r.diagnostics()
            .iter()
            .all(|d| d.message != live_vbap::DIFFUSE_WARNING)
    );
}

#[test]
fn direct_speakers_route_by_label_before_their_position() {
    let layout = "0+5+0";
    let channels = 6;
    let mut r = renderer(layout);
    let elements = [
        element(1, Role::DirectSpeaker, "M+030"),
        Element {
            has_position: true,
            position: [0.2, 0.9, 0.],
            ..element(2, Role::DirectSpeaker, "unknown")
        },
        element(3, Role::DirectSpeaker, "U+030"),
        // An LFE label on a DirectSpeakers element is not routed to the LFE output.
        Element {
            has_position: true,
            position: [-0.5, 0.5, 0.],
            ..element(4, Role::DirectSpeaker, "LFE1")
        },
    ];
    r.configure(0, &elements[..1]).unwrap();
    let out = render(&mut r, &[1], 2, &[], &[]).unwrap();
    let mut expected = vec![0.; channels];
    expected[channel(layout, "M+030")] = 1.;
    assert_eq!(frame_at(&out, channels, 1), expected);
    assert!(r.diagnostics().is_empty());

    let panner = Panner::new(&directions(layout), false).unwrap();
    let pan = |p: PannerSource| scatter(layout, &sources(&panner, &[p], false, false));
    for (e, source) in [
        (
            elements[1],
            PannerSource {
                cartesian: 1,
                position: [0.2, 0.9, 0.],
                gain: 1.,
                ..PannerSource::default()
            },
        ),
        (
            elements[2],
            PannerSource {
                position: [30., 30., 1.],
                gain: 1.,
                ..PannerSource::default()
            },
        ),
        (
            elements[3],
            PannerSource {
                cartesian: 1,
                position: [-0.5, 0.5, 0.],
                gain: 1.,
                ..PannerSource::default()
            },
        ),
    ] {
        let mut r = renderer(layout);
        r.configure(0, &[e]).unwrap();
        let out = render(&mut r, &[e.id], 2, &[], &[]).unwrap();
        assert_eq!(frame_at(&out, channels, 1), pan(source));
        assert_eq!(r.diagnostics()[0].code, code::DIRECT_SPEAKER_FALLBACK);
    }
    // A position update overrides the label route; clearing it restores the route.
    let mut r = renderer(layout);
    r.configure(0, &elements[..1]).unwrap();
    let moved = State {
        valid_fields: field::POSITION,
        position: [0.2, 0.9, 0.],
        ..State::default()
    };
    let mut jump = update(1, 0, field::POSITION, moved);
    jump.jump = true;
    let out = render(&mut r, &[1], 2, &[], &[jump]).unwrap();
    assert_eq!(
        frame_at(&out, channels, 1),
        pan(PannerSource {
            cartesian: 1,
            position: [0.2, 0.9, 0.],
            gain: 1.,
            ..PannerSource::default()
        })
    );
    let mut clear = update(1, 0, 0, State::default());
    clear.cleared = field::POSITION;
    clear.jump = true;
    let out = render(&mut r, &[1], 2, &[], &[clear]).unwrap();
    assert_eq!(frame_at(&out, channels, 1), expected);
}

#[test]
fn lfe_elements_follow_the_output_lfe_channels() {
    let id = |r: &mut Renderer, label: &'static str| {
        r.configure(0, &[element(5, Role::Lfe, label)]).unwrap();
        render(r, &[5], 2, &[], &[]).unwrap()
    };
    let layout = "9+10+3";
    let (lfe1, lfe2) = (channel(layout, "LFE1"), channel(layout, "LFE2"));
    let channels = 24;
    let mut direct = renderer(layout);
    let mut gains = |label| frame_at(&id(&mut direct, label), channels, 1).to_vec();
    assert_eq!(gains("LFE2")[lfe2], 1.);
    assert_eq!(gains("LFE2").iter().sum::<f32>(), 1.);
    assert_eq!(gains("LFER")[lfe2], 1.);
    assert_eq!(gains("unknown")[lfe1], 1.);
    assert_eq!(gains("")[lfe1], 1.);
    let mut split = Renderer::new(&Config {
        split_power: true,
        ..config(layout)
    })
    .unwrap();
    let out = id(&mut split, "LFE2");
    let frame = frame_at(&out, channels, 1);
    assert_eq!(
        (frame[lfe1], frame[lfe2]),
        (LFE_SPLIT_POWER_GAIN, LFE_SPLIT_POWER_GAIN)
    );
    // Split power needs two LFE outputs.
    let mut single = Renderer::new(&Config {
        split_power: true,
        ..config("0+5+0")
    })
    .unwrap();
    let out = id(&mut single, "LFE2");
    assert_eq!(frame_at(&out, 6, 1)[channel("0+5+0", "LFE1")], 1.);
    let mut none = renderer("0+2+0");
    let out = id(&mut none, "LFE1");
    assert!(out.iter().all(|&v| v == 0.));
    assert_eq!(
        none.diagnostics(),
        [Diagnostic {
            code: code::MISSING_LFE_OUTPUT,
            element_id: 5,
            field: 0,
            message: live_vbap::NO_LFE_WARNING,
        }]
    );
}

fn directions(layout: &str) -> Vec<f32> {
    layouts::find(layout, Geometry::Standard)
        .unwrap()
        .speakers
        .iter()
        .filter(|s| !s.is_lfe)
        .flat_map(|s| [s.azimuth, s.elevation])
        .collect()
}

fn sources(panner: &Panner, sources: &[PannerSource], spread: bool, sum: bool) -> Vec<f32> {
    let mut scratch = vec![0.; panner.real_speakers];
    let mut table = vec![0.; panner.real_speakers];
    panner
        .source_gains(sources, spread, sum, &mut scratch, &mut table)
        .unwrap();
    table
}

fn scatter(layout: &str, table: &[f32]) -> Vec<f32> {
    let mut panned = table.iter();
    layouts::find(layout, Geometry::Standard)
        .unwrap()
        .speakers
        .iter()
        .map(|s| {
            if s.is_lfe {
                0.
            } else {
                *panned.next().unwrap()
            }
        })
        .collect()
}

#[test]
fn objects_pan_after_channel_lock_and_divergence() {
    let layout = "4+5+0";
    let channels = 10;
    let speakers: Vec<SceneSpeaker> = layouts::find(layout, Geometry::Standard)
        .unwrap()
        .speakers
        .iter()
        .map(|s| SceneSpeaker {
            azimuth: s.azimuth,
            elevation: s.elevation,
            is_lfe: u32::from(s.is_lfe),
        })
        .collect();
    let panner = Panner::new(&directions(layout), true).unwrap();
    let object = State {
        valid_fields: field::KNOWN,
        position: [0.35, 0.8, 0.2],
        width: 20.,
        height: 10.,
        depth: 0.1,
        divergence: 0.4,
        divergence_azimuth_range: 30.,
        channel_lock: true,
        channel_lock_max_distance: Some(2.),
        ..State::default()
    };
    let locked = math::channel_lock(object.position, true, Some(2.), &speakers).unwrap();
    let mut points = [CloudPoint::default(); 3];
    assert_eq!(
        math::divergence(locked, false, 0.4, 30., 0., 1., &mut points),
        3
    );
    let expected: Vec<PannerSource> = points
        .iter()
        .map(|p| PannerSource {
            cartesian: 0,
            position: [p.azimuth, p.elevation, locked[2]],
            gain: p.weight,
            width: 20.,
            height: 10.,
            depth: 0.1,
        })
        .collect();
    for spread in [true, false] {
        let mut r = Renderer::new(&Config {
            spread,
            ..config(layout)
        })
        .unwrap();
        r.configure(0, &[element(1, Role::Object, "")]).unwrap();
        let initial = [Entry {
            element_id: 1,
            state: object,
        }];
        let out = render(&mut r, &[1], 2, &initial, &[]).unwrap();
        assert_eq!(
            frame_at(&out, channels, 1),
            scatter(layout, &sources(&panner, &expected, spread, true))
        );
    }
    // A negligible divergence keeps the one Cartesian source; without the lock it stays where it is.
    let plain = State {
        divergence: 0.,
        channel_lock: false,
        ..object
    };
    let mut r = renderer(layout);
    r.configure(0, &[element(1, Role::Object, "")]).unwrap();
    let initial = [Entry {
        element_id: 1,
        state: plain,
    }];
    let out = render(&mut r, &[1], 2, &initial, &[]).unwrap();
    let source = PannerSource {
        cartesian: 1,
        position: plain.position,
        gain: 1.,
        width: 20.,
        height: 10.,
        depth: 0.1,
    };
    assert_eq!(
        frame_at(&out, channels, 1),
        scatter(layout, &sources(&panner, &[source], true, true))
    );
}

#[test]
fn two_dimensional_layouts_ignore_the_extent() {
    let wide = State {
        valid_fields: field::EXTENT,
        width: 90.,
        height: 40.,
        ..State::default()
    };
    let gains = |layout: &str, state: State| {
        let mut r = renderer(layout);
        r.configure(0, &[element(1, Role::Object, "")]).unwrap();
        let initial = [Entry {
            element_id: 1,
            state,
        }];
        render(&mut r, &[1], 1, &initial, &[]).unwrap()
    };
    assert_eq!(gains("0+5+0", wide), gains("0+5+0", State::default()));
    assert_ne!(gains("4+5+0", wide), gains("4+5+0", State::default()));
}

#[test]
fn level_ramps_follow_jump_then_ramp_then_smoothing() {
    let mut r = renderer("0+2+0");
    r.configure(0, &[element(1, Role::DirectSpeaker, "M+030")])
        .unwrap();
    render(&mut r, &[1], 4, &[], &[]).unwrap();
    let left = channel("0+2+0", "M+030");
    let level = |out: &[f32], frame: usize| frame_at(out, 2, frame)[left];
    let mut jump = update(1, 1, field::LINEAR_GAIN, state(0.25));
    jump.jump = true;
    jump.ramp = 100;
    let out = render(&mut r, &[1], 4, &[], &[jump]).unwrap();
    assert_eq!((level(&out, 0), level(&out, 1)), (1., 0.25));
    let mut ramp = update(1, 0, field::LINEAR_GAIN, state(1.));
    ramp.ramp = 2;
    let out = render(&mut r, &[1], 4, &[], &[ramp]).unwrap();
    assert_eq!(level(&out, 3), 1.);
    assert!(level(&out, 0) < 1.);
    // Without either, the configured smoothing (17 frames) applies.
    let smooth = update(1, 0, field::LINEAR_GAIN, state(0.));
    let out = render(&mut r, &[1], 4, &[], &[smooth]).unwrap();
    assert!(level(&out, 3) > 0.);
}

#[test]
fn split_frames_render_as_one() {
    let elements = [element(1, Role::Object, ""), element(2, Role::Lfe, "LFE1")];
    let target = State {
        valid_fields: field::POSITION | field::LINEAR_GAIN,
        position: [-0.6, 0.4, 0.1],
        linear_gain: 0.3,
        ..State::default()
    };
    let mut moving = update(1, 5, field::POSITION | field::LINEAR_GAIN, target);
    moving.ramp = 300;
    let mut whole = renderer("4+5+0");
    whole.configure(0, &elements).unwrap();
    let expected = render(&mut whole, &[1, 2], 512, &[], &[moving]).unwrap();
    let mut split = renderer("4+5+0");
    split.configure(0, &elements).unwrap();
    let mut out = render(&mut split, &[1, 2], 256, &[], &[moving]).unwrap();
    out.extend(render(&mut split, &[1, 2], 256, &[], &[]).unwrap());
    assert_eq!(
        out.iter().map(|v| v.to_bits()).collect::<Vec<_>>(),
        expected.iter().map(|v| v.to_bits()).collect::<Vec<_>>()
    );
}

#[test]
fn reset_requires_a_new_generation() {
    let mut r = renderer("0+5+0");
    r.configure(3, &[element(1, Role::Object, "")]).unwrap();
    r.reset();
    let e = render(&mut r, &[], 2, &[], &[]).unwrap_err();
    assert_eq!(
        text(&e).1,
        "live VBAP frame uses an unconfigured generation"
    );
    r.configure(0, &[]).unwrap();
    let out = render(&mut r, &[], 3, &[], &[]).unwrap();
    assert!(out.iter().all(|&v| v == 0.));
}
