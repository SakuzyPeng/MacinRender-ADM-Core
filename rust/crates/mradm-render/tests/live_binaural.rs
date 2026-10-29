//! The Scene stream binaural renderer (PCM consistency 4.7 D): the C++ checks and texts, frames
//! that commit only when they render, the DirectSpeakers direction rules and their warning, the
//! update durations, the once-per-generation warnings, reset and the tail.
use mradm_dsp::dataset::Dataset;
use mradm_render::binaural_hrtf;
use mradm_render::live::{
    Diagnostic, Element, Entry, Frame, Plane, Role, State, Update, code, field,
};
use mradm_render::live_binaural::{Config, Renderer};
use mradm_render::routing::Error;
use std::sync::Arc;

const FRAMES: u32 = 1024;

fn renderer(spread: u32, smoothing_frames: u32) -> Renderer {
    let hrtf = Arc::new(binaural_hrtf::build(Dataset::kemar(), 1024, false).unwrap());
    Renderer::new(
        hrtf,
        &Config {
            sample_rate: 48000,
            spread,
            smoothing_frames,
        },
    )
    .unwrap()
}

fn element(
    id: u64,
    role: Role,
    label: &'static str,
    position: Option<[f32; 3]>,
) -> Element<'static> {
    Element {
        id,
        role: Some(role),
        speaker_label: label.as_bytes(),
        object_id: b"",
        has_position: position.is_some(),
        position: position.unwrap_or([0., 1., 0.]),
    }
}

fn signal() -> Vec<f32> {
    (0..FRAMES).map(|i| 0.5 * (i as f32 * 0.05).sin()).collect()
}

fn frame<'a>(
    generation: u64,
    planes: &'a [Plane<'a>],
    initial: &'a [Entry],
    updates: &'a [Update],
) -> Frame<'a> {
    Frame {
        generation,
        duration: FRAMES,
        planes,
        initial,
        updates,
        trace: b"",
    }
}

fn plane(element_id: u64, samples: &[f32]) -> Plane<'_> {
    Plane {
        element_id,
        has_signal: true,
        samples,
    }
}

fn state(valid_fields: u64) -> State {
    State {
        valid_fields,
        ..State::default()
    }
}

fn positioned(position: [f32; 3]) -> State {
    State {
        valid_fields: field::POSITION,
        position,
        ..State::default()
    }
}

fn update(element_id: u64, offset: u32, state: State) -> Update {
    Update {
        element_id,
        offset,
        ramp: 0,
        jump: false,
        changed: state.valid_fields,
        cleared: 0,
        state,
    }
}

fn render(r: &mut Renderer, f: &Frame) -> Result<Vec<u32>, Error> {
    let mut out = vec![0.; 2 * FRAMES as usize];
    r.render(f, &mut out)?;
    Ok(out.iter().map(|v| v.to_bits()).collect())
}

fn text(e: Error) -> (i32, String, String) {
    (
        e.code,
        String::from_utf8(e.message).unwrap(),
        String::from_utf8(e.context).unwrap(),
    )
}

fn plain(code: i32, message: &str) -> (i32, String, String) {
    (code, message.to_owned(), String::new())
}

#[test]
fn configure_and_frame_errors_keep_the_cpp_texts() {
    let mut r = renderer(0, 0);
    let duplicate = [
        element(1, Role::Object, "", None),
        element(1, Role::Object, "", None),
    ];
    assert_eq!(
        text(r.configure(1, &duplicate).unwrap_err()),
        plain(1, "Live binaural has an invalid or duplicate element")
    );
    let mut bad_role = element(1, Role::Object, "", None);
    bad_role.role = None;
    assert_eq!(
        text(r.configure(1, &[bad_role]).unwrap_err()),
        plain(1, "Live binaural has an invalid or duplicate element")
    );
    assert_eq!(
        text(
            r.configure(1, &[element(1, Role::Object, "", Some([f32::NAN, 1., 0.]))])
                .unwrap_err()
        ),
        plain(1, "Live binaural element position is invalid")
    );
    let elements = [
        element(1, Role::Object, "", None),
        element(2, Role::DirectSpeaker, "M+030", None),
    ];
    r.configure(1, &elements).unwrap();
    let s = signal();
    let ok = [plane(1, &s), plane(2, &s)];
    assert_eq!(
        text(render(&mut r, &frame(2, &ok, &[], &[])).unwrap_err()),
        plain(1, "Live binaural output or generation is invalid")
    );
    let mut short = vec![0.; 2 * FRAMES as usize - 1];
    assert_eq!(
        text(r.render(&frame(1, &ok, &[], &[]), &mut short).unwrap_err()),
        plain(1, "Live binaural output or generation is invalid")
    );
    let initial = |id| Entry {
        element_id: id,
        state: state(field::ACTIVE),
    };
    for entries in [vec![initial(9)], vec![initial(1), initial(1)]] {
        assert_eq!(
            text(render(&mut r, &frame(1, &ok, &entries, &[])).unwrap_err()),
            plain(1, "Live binaural initial state is invalid or duplicated")
        );
    }
    let truncated = [plane(1, &s[..10])];
    for planes in [
        &[plane(9, &s)][..],
        &[plane(1, &s), plane(1, &s)],
        &truncated,
    ] {
        assert_eq!(
            text(render(&mut r, &frame(1, planes, &[], &[])).unwrap_err()),
            plain(1, "Live binaural PCM view is invalid or duplicated")
        );
    }
    let mut unknown = update(1, 0, state(field::LINEAR_GAIN));
    unknown.changed |= 1 << 20;
    let mut outside = update(1, 0, state(field::LINEAR_GAIN));
    outside.changed |= field::POSITION;
    for updates in [
        vec![
            update(1, 10, state(field::ACTIVE)),
            update(1, 5, state(field::ACTIVE)),
        ],
        vec![update(1, FRAMES, state(field::ACTIVE))],
        vec![update(9, 0, state(field::ACTIVE))],
        vec![unknown],
        vec![outside],
    ] {
        assert_eq!(
            text(render(&mut r, &frame(1, &ok, &[], &updates)).unwrap_err()),
            plain(1, "Live binaural metadata event is invalid or unordered")
        );
    }
    r.configure(2, &[element(7, Role::DirectSpeaker, "", None)])
        .unwrap();
    assert_eq!(
        text(render(&mut r, &frame(2, &[], &[], &[])).unwrap_err()),
        plain(
            2,
            "DirectSpeakers element 7 has neither a known label nor a position"
        )
    );
    // Reset clears the generation: the old one is rejected, generation 0 renders silence.
    r.reset();
    assert_eq!(
        text(render(&mut r, &frame(2, &[], &[], &[])).unwrap_err()),
        plain(1, "Live binaural output or generation is invalid")
    );
    let silent = render(&mut r, &frame(0, &[], &[], &[])).unwrap();
    assert!(silent.iter().all(|&v| v == 0));
    // A pose the session rejects is its error, with the DSP context.
    r.set_pose([0., f32::INFINITY, 0.]);
    assert_eq!(
        text(render(&mut r, &frame(0, &[], &[], &[])).unwrap_err()),
        (
            1,
            "Invalid head orientation".to_owned(),
            "Live binaural DSP".to_owned()
        )
    );
}

#[test]
fn a_failed_frame_commits_nothing() {
    let elements = [element(1, Role::Object, "", None)];
    let s = signal();
    let planes = [plane(1, &s)];
    let moved = [update(1, 0, positioned([0.7, 0.7, 0.]))];
    let mut a = renderer(0, 256);
    let mut b = renderer(0, 256);
    a.configure(1, &elements).unwrap();
    b.configure(1, &elements).unwrap();
    render(&mut a, &frame(1, &planes, &[], &[])).unwrap();
    render(&mut b, &frame(1, &planes, &[], &[])).unwrap();
    // The first event stages a move the second one invalidates.
    let failing = [
        update(1, 0, positioned([0.7, 0.7, 0.])),
        update(1, 512, positioned([f32::NAN, 0., 0.])),
    ];
    assert!(render(&mut a, &frame(1, &planes, &[], &failing)).is_err());
    let next = [update(1, 0, state(field::LINEAR_GAIN))];
    assert_eq!(
        render(&mut a, &frame(1, &planes, &[], &next)).unwrap(),
        render(&mut b, &frame(1, &planes, &[], &next)).unwrap()
    );
    assert_ne!(
        render(&mut a, &frame(1, &planes, &[], &moved)).unwrap(),
        render(&mut b, &frame(1, &planes, &[], &next)).unwrap()
    );
    // A frame the session rejects (here for its pose) keeps the staged move uncommitted too.
    render(&mut b, &frame(1, &planes, &[], &moved)).unwrap();
    let away = [update(1, 0, positioned([-0.6, 0.3, 0.5]))];
    a.set_pose([f32::NAN, 0., 0.]);
    assert!(render(&mut a, &frame(1, &planes, &[], &away)).is_err());
    a.set_pose([0.; 3]);
    assert_eq!(
        render(&mut a, &frame(1, &planes, &[], &next)).unwrap(),
        render(&mut b, &frame(1, &planes, &[], &next)).unwrap()
    );
    // A rejected first frame leaves the elements uninitialized, so the next one starts them.
    let start = [Entry {
        element_id: 1,
        state: positioned([0.3, 0.9, -0.2]),
    }];
    let mut c = renderer(0, 256);
    let mut fresh = renderer(0, 256);
    c.configure(1, &elements).unwrap();
    fresh.configure(1, &elements).unwrap();
    c.set_pose([f32::NAN, 0., 0.]);
    assert!(render(&mut c, &frame(1, &planes, &start, &[])).is_err());
    c.set_pose([0.; 3]);
    assert_eq!(
        render(&mut c, &frame(1, &planes, &[], &[])).unwrap(),
        render(&mut fresh, &frame(1, &planes, &[], &[])).unwrap()
    );
}

/// One DirectSpeakers element alone, rendered for two frames.
fn direct(
    label: &'static str,
    position: Option<[f32; 3]>,
    initial: Option<State>,
) -> (Vec<u32>, Vec<Diagnostic>) {
    let mut r = renderer(0, 0);
    r.configure(1, &[element(5, Role::DirectSpeaker, label, position)])
        .unwrap();
    let s = signal();
    let planes = [plane(5, &s)];
    let entries: Vec<_> = initial
        .map(|state| Entry {
            element_id: 5,
            state,
        })
        .into_iter()
        .collect();
    let mut out = render(&mut r, &frame(1, &planes, &entries, &[])).unwrap();
    let mut warnings = r.diagnostics().to_vec();
    out.extend(render(&mut r, &frame(1, &planes, &[], &[])).unwrap());
    warnings.extend_from_slice(r.diagnostics());
    (out, warnings)
}

#[test]
fn direct_speakers_take_the_state_then_the_label_then_the_producer_position() {
    let left = [-0.5, 0.86, 0.];
    let right = [0.5, 0.86, 0.];
    // A known label wins over the producer position.
    assert_eq!(
        direct("M+030", Some(right), None).0,
        direct("M+030", None, None).0
    );
    assert_ne!(
        direct("M+030", None, None).0,
        direct("", Some(right), None).0
    );
    assert_ne!(direct("M+030", None, None).0, direct("M-030", None, None).0);
    assert_ne!(direct("U+030", None, None).0, direct("M+030", None, None).0);
    // The table direction is exact: (-1, 0, 0) converts to azimuth 90, elevation 0.
    assert_eq!(
        direct("M+090", None, None).0,
        direct("", Some([-1., 0., 0.]), None).0
    );
    assert_eq!(
        direct("M-090", None, None).0,
        direct("", Some([1., 0., 0.]), None).0
    );
    // An unknown label falls back to the producer position and warns once.
    let (fallback, warnings) = direct("X+999", Some(left), None);
    assert_eq!(fallback, direct("", Some(left), None).0);
    assert_eq!(
        warnings,
        [Diagnostic {
            code: code::DIRECT_SPEAKER_FALLBACK,
            element_id: 5,
            field: 0,
            message: "DirectSpeakers label is unknown; using its canonical fixed position",
        }]
    );
    assert!(direct("", Some(left), None).1.is_empty());
    // A state position wins over both.
    let state = Some(positioned(left));
    assert_eq!(
        direct("M+030", Some(right), state).0,
        direct("", Some(right), state).0
    );
    assert_eq!(
        direct("X+999", None, state).0,
        direct("", Some(left), None).0
    );
    assert!(direct("X+999", None, state).1.is_empty());
}

fn moved(smoothing: u32, jump: bool, ramp: u32) -> Vec<u32> {
    let mut r = renderer(0, smoothing);
    r.configure(1, &[element(1, Role::Object, "", None)])
        .unwrap();
    let s = signal();
    let planes = [plane(1, &s)];
    render(&mut r, &frame(1, &planes, &[], &[])).unwrap();
    let mut u = update(1, 128, positioned([0.9, 0.2, 0.3]));
    u.jump = jump;
    u.ramp = ramp;
    let mut out = render(&mut r, &frame(1, &planes, &[], &[u])).unwrap();
    out.extend(render(&mut r, &frame(1, &planes, &[], &[])).unwrap());
    out
}

#[test]
fn a_jump_wins_over_the_ramp_which_wins_over_the_smoothing() {
    let immediate = moved(0, false, 0);
    assert_eq!(moved(480, true, 300), immediate);
    assert_eq!(moved(480, false, 300), moved(0, false, 300));
    assert_eq!(moved(480, false, 0), moved(0, false, 480));
    assert_ne!(moved(480, false, 0), immediate);
    assert_ne!(moved(480, false, 300), moved(480, false, 0));
}

#[test]
fn warnings_are_raised_once_per_generation() {
    let mut r = renderer(1, 0);
    let elements = [
        element(1, Role::Object, "", None),
        element(2, Role::Object, "", None),
    ];
    let s = signal();
    let planes = [plane(1, &s), plane(2, &s)];
    let wide = State {
        valid_fields: field::EXTENT | field::SCREEN_REFERENCE,
        width: 40.,
        screen_reference: true,
        ..State::default()
    };
    let extent = |id| Diagnostic {
        code: code::SEMANTIC_DEGRADED,
        element_id: id,
        field: field::EXTENT,
        message: "binaural spread mode is none; extent is ignored",
    };
    let screen = |id| Diagnostic {
        code: code::SEMANTIC_DEGRADED,
        element_id: id,
        field: field::SCREEN_REFERENCE,
        message: "live binaural has no screen transform; canonical position is used",
    };
    for generation in [1, 2] {
        r.configure(generation, &elements).unwrap();
        let initial = [Entry {
            element_id: 2,
            state: wide,
        }];
        render(&mut r, &frame(generation, &planes, &initial, &[])).unwrap();
        let mut got = r.diagnostics().to_vec();
        got.sort_by_key(|d| d.field);
        assert_eq!(got, [extent(2), screen(2)]);
        let again = [update(1, 0, wide)];
        render(&mut r, &frame(generation, &planes, &[], &again)).unwrap();
        assert!(r.diagnostics().is_empty());
    }
    // A frame the session rejects neither warns nor uses up the warning.
    r.configure(3, &elements).unwrap();
    r.set_pose([f32::NAN, 0., 0.]);
    let initial = [Entry {
        element_id: 1,
        state: wide,
    }];
    let e = render(&mut r, &frame(3, &planes, &initial, &[])).unwrap_err();
    assert_eq!(text(e).2, "Live binaural DSP");
    assert!(r.diagnostics().is_empty());
    r.set_pose([0.; 3]);
    render(&mut r, &frame(3, &planes, &initial, &[])).unwrap();
    assert_eq!(r.diagnostics().len(), 2);
}

#[test]
fn the_tail_covers_the_convolution_and_the_diffuse_delay() {
    let hrtf = Arc::new(binaural_hrtf::build(Dataset::kemar(), 1024, false).unwrap());
    let fft = hrtf.fft_size as u64;
    let r = Renderer::new(
        hrtf,
        &Config {
            sample_rate: 48000,
            spread: 0,
            smoothing_frames: 0,
        },
    )
    .unwrap();
    assert_eq!((r.tail_frames(), r.output_channels()), (fft - 1 + 32, 2));
}
