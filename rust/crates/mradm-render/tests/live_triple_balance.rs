//! The Scene stream Triple Balance renderer (PCM consistency 4.6 C): the C++ checks, texts and
//! contexts, atomic frames, the bed lanes and the spread=none warnings in element order.
use mradm_dsp::triple_balance::{Layout, bed};
use mradm_render::live::{
    Diagnostic, Element, Entry, Frame, Plane, Role, State, Update, code, field,
};
use mradm_render::live_triple_balance::{Config, Renderer, SPREAD_NONE_WARNING, Spread};

const BED: [&str; 10] = [
    "M+030", "M-030", "M+000", "LFE1", "M+090", "M-090", "M+135", "M-135", "U+090", "U-090",
];

fn config(layout: &'static str) -> Config<'static> {
    Config {
        layout_id: layout.as_bytes(),
        standard_geometry: true,
        spread: Spread::Automatic,
        has_sofa: false,
        sample_rate: 48000,
        split_power: false,
        smoothing_frames: 0,
    }
}

fn object(id: u64) -> Element<'static> {
    Element {
        id,
        role: Some(Role::Object),
        speaker_label: b"",
        object_id: b"",
        has_position: false,
        position: [0., 1., 0.],
    }
}

fn bed_element(id: u64, label: &'static str, object_id: &'static [u8]) -> Element<'static> {
    Element {
        id,
        role: Some(if label == "LFE1" {
            Role::Lfe
        } else {
            Role::DirectSpeaker
        }),
        speaker_label: label.as_bytes(),
        object_id,
        has_position: false,
        position: [0., 1., 0.],
    }
}

fn full_bed(object_id: &'static [u8]) -> Vec<Element<'static>> {
    BED.iter()
        .enumerate()
        .map(|(i, l)| bed_element(100 + i as u64, l, object_id))
        .collect()
}

fn frame<'a>(
    generation: u64,
    duration: u32,
    planes: &'a [Plane<'a>],
    initial: &'a [Entry],
    updates: &'a [Update],
) -> Frame<'a> {
    Frame {
        generation,
        duration,
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

fn update(element_id: u64, offset: u32, changed: u64, state: State) -> Update {
    Update {
        element_id,
        offset,
        ramp: 0,
        jump: false,
        changed,
        cleared: 0,
        state,
    }
}

fn err(r: Result<(), mradm_render::routing::Error>) -> (i32, String, String) {
    let e = r.unwrap_err();
    (
        e.code,
        String::from_utf8(e.message).unwrap(),
        String::from_utf8(e.context).unwrap(),
    )
}

fn ok_text(code: i32, message: &str, context: &str) -> (i32, String, String) {
    (code, message.to_owned(), context.to_owned())
}

fn sine(frames: usize) -> Vec<f32> {
    (0..frames)
        .map(|i| 0.5 * (i as f32 * 0.031).sin())
        .collect()
}

#[test]
fn creation_checks_keep_the_cpp_texts() {
    let new = |c: Config| {
        Renderer::new(&c)
            .err()
            .map(|e| (e.code, String::from_utf8(e.message).unwrap()))
    };
    assert_eq!(
        new(config("5.1")),
        Some((2, "Unsupported Triple Balance layout".to_owned()))
    );
    let fixed = Some((
        2,
        "Live Triple Balance requires fixed room geometry, no MDAP/SOFA and 48 kHz for 22.2"
            .to_owned(),
    ));
    assert_eq!(
        new(Config {
            standard_geometry: false,
            ..config("7.1.4")
        }),
        fixed
    );
    assert_eq!(
        new(Config {
            spread: Spread::Mdap,
            ..config("9.1.6")
        }),
        fixed
    );
    assert_eq!(
        new(Config {
            has_sofa: true,
            ..config("9.1.6")
        }),
        fixed
    );
    assert_eq!(
        new(Config {
            sample_rate: 44100,
            ..config("9+10+3")
        }),
        fixed
    );
    for (layout, channels) in [("4+7+0", 12), ("7.1.4", 12), ("9.1.6", 16), ("22.2", 24)] {
        assert_eq!(
            Renderer::new(&config(layout)).unwrap().output_channels(),
            channels
        );
    }
    assert!(
        Renderer::new(&Config {
            sample_rate: 44100,
            ..config("7.1.4")
        })
        .is_ok()
    );
}

#[test]
fn generation_checks_keep_the_cpp_texts_and_the_previous_generation() {
    let mut r = Renderer::new(&config("9.1.6")).unwrap();
    r.configure(1, &[object(1)]).unwrap();
    let invalid = ok_text(1, "Live Triple Balance invalid or duplicate element", "");
    assert_eq!(err(r.configure(2, &[object(1), object(1)])), invalid);
    let mut unknown = object(2);
    unknown.role = None;
    assert_eq!(err(r.configure(2, &[unknown])), invalid);
    let labels = |id: u64| {
        ok_text(
            2,
            "Live Triple Balance requires standard 7.1.2 bed labels",
            &format!("element={id}; field=speaker_label"),
        )
    };
    assert_eq!(
        err(r.configure(2, &[bed_element(7, "M+110", b"")])),
        labels(7)
    );
    let mut lfe2 = bed_element(8, "LFE1", b"");
    lfe2.speaker_label = b"LFE2";
    assert_eq!(err(r.configure(2, &[lfe2])), labels(8));
    let mut lfe_as_speaker = bed_element(9, "M+030", b"");
    lfe_as_speaker.role = Some(Role::Lfe);
    assert_eq!(err(r.configure(2, &[lfe_as_speaker])), labels(9));
    assert_eq!(
        err(r.configure(2, &[bed_element(3, "M+030", b""), bed_element(4, "L", b"")])),
        ok_text(
            2,
            "Live Triple Balance duplicate bed channel",
            "element=4; field=speaker_label"
        ),
        "an alias names the same channel"
    );
    let mut two = full_bed(b"AO_1001");
    two[4].object_id = b"AO_1002";
    assert_eq!(
        err(r.configure(2, &two)),
        ok_text(
            2,
            "Live Triple Balance multiple beds are unsupported",
            "element=104; field=object_id"
        )
    );
    let complete = ok_text(
        2,
        "Live Triple Balance requires one complete 48 kHz 7.1.2 bed",
        "",
    );
    assert_eq!(err(r.configure(2, &full_bed(b"")[..9])), complete);
    let mut slow = Renderer::new(&Config {
        sample_rate: 44100,
        ..config("7.1.4")
    })
    .unwrap();
    assert_eq!(err(slow.configure(2, &full_bed(b""))), complete);
    slow.configure(2, &[object(1)]).unwrap();
    // Every rejection above kept generation 1.
    let mut out = vec![0.; 16 * 8];
    assert!(r.render(&frame(1, 8, &[], &[], &[]), &mut out).is_ok());
    // Unlabelled LFE is LFE1; a bed channel without an object id does not split the bed.
    let mut mixed = full_bed(b"AO_1001");
    mixed[3].speaker_label = b"";
    mixed[5].object_id = b"";
    r.configure(3, &mixed).unwrap();
    assert!(r.render(&frame(3, 8, &[], &[], &[]), &mut out).is_ok());
}

#[test]
fn bed_lanes_route_through_the_dsp_bed_gains() {
    for (layout, code, split) in [
        ("7.1.4", Layout::Seven, false),
        ("9.1.6", Layout::Nine, false),
        ("22.2", Layout::Room222, false),
        ("22.2", Layout::Room222, true),
    ] {
        let channels = code.channels();
        let input = sine(300);
        for (i, label) in BED.iter().enumerate() {
            let mut r = Renderer::new(&Config {
                split_power: split,
                ..config(layout)
            })
            .unwrap();
            r.configure(1, &full_bed(b"")).unwrap();
            let planes = [plane(100 + i as u64, &input)];
            let mut out = vec![0.; 300 * channels];
            r.render(&frame(1, 300, &planes, &[], &[]), &mut out)
                .unwrap();
            let mut gains = vec![0.; channels];
            bed::gains(code, i, split, &mut gains).unwrap();
            for (f, x) in input.iter().enumerate() {
                for c in 0..channels {
                    assert_eq!(
                        out[f * channels + c],
                        gains[c] * x,
                        "{layout} {label} frame {f} channel {c}"
                    );
                }
            }
        }
    }
}

#[test]
fn frame_checks_keep_the_cpp_order_and_texts() {
    let mut r = Renderer::new(&config("7.1.4")).unwrap();
    let mut out = vec![7.; 12 * 64];
    let buffer = ok_text(
        1,
        "Live Triple Balance invalid generation or output buffer",
        "",
    );
    assert_eq!(
        err(r.render(&frame(0, 64, &[], &[], &[]), &mut out)),
        buffer
    );
    r.configure(4, &[object(1), object(2)]).unwrap();
    assert_eq!(
        err(r.render(&frame(5, 64, &[], &[], &[]), &mut out)),
        buffer
    );
    assert_eq!(
        err(r.render(&frame(4, 65, &[], &[], &[]), &mut out)),
        buffer
    );
    let short = [0.; 63];
    let full = [0.; 64];
    let pcm = ok_text(1, "Live Triple Balance invalid PCM plane", "");
    for planes in [
        vec![plane(3, &full)],
        vec![plane(1, &full), plane(1, &full)],
        vec![plane(1, &short)],
    ] {
        assert_eq!(
            err(r.render(&frame(4, 64, &planes, &[], &[]), &mut out)),
            pcm
        );
    }
    let silent = [Plane {
        element_id: 1,
        has_signal: false,
        samples: &[],
    }];
    assert!(r.render(&frame(4, 64, &silent, &[], &[]), &mut out).is_ok());
    let initial = ok_text(1, "Live Triple Balance invalid initial state", "");
    let entry = |id, s| Entry {
        element_id: id,
        state: s,
    };
    let mut nan = state(field::LINEAR_GAIN);
    nan.linear_gain = f32::NAN;
    for entries in [
        vec![entry(3, state(0))],
        vec![entry(1, state(0)), entry(1, state(0))],
        vec![entry(1, nan)],
    ] {
        assert_eq!(
            err(r.render(&frame(4, 64, &[], &entries, &[]), &mut out)),
            initial
        );
    }
    let metadata = ok_text(1, "Live Triple Balance invalid metadata update", "");
    let gain = state(field::LINEAR_GAIN);
    for updates in [
        vec![update(3, 0, field::LINEAR_GAIN, gain)],
        vec![update(1, 64, field::LINEAR_GAIN, gain)],
        vec![
            update(1, 9, field::LINEAR_GAIN, gain),
            update(2, 8, field::LINEAR_GAIN, gain),
        ],
        vec![update(1, 0, 1 << 11, state(1 << 11))],
        vec![update(1, 0, field::POSITION, gain)],
        vec![update(1, 0, field::LINEAR_GAIN, nan)],
    ] {
        assert_eq!(
            err(r.render(&frame(4, 64, &[], &[], &updates), &mut out)),
            metadata
        );
    }
    let modifiers = ok_text(
        2,
        "Live Triple Balance does not support these object modifiers",
        "element=2; field=divergence/channelLock/screenRef/headLocked",
    );
    for edit in [
        |s: &mut State| s.divergence = 0.5,
        |s: &mut State| s.channel_lock = true,
        |s: &mut State| s.screen_reference = true,
        |s: &mut State| s.head_locked = true,
    ] {
        let mut s = state(field::KNOWN);
        edit(&mut s);
        let entries = [entry(2, s)];
        assert_eq!(
            err(r.render(&frame(4, 64, &[], &entries, &[]), &mut out)),
            modifiers
        );
    }
    let room = ok_text(
        2,
        "Live Triple Balance position is outside the supported room",
        "element=1; field=position",
    );
    for position in [
        [1.01, 0., 0.],
        [0., -1.01, 0.],
        [0., 0., 1.01],
        [0., 0., -0.01],
    ] {
        let mut s = state(field::POSITION);
        s.position = position;
        let updates = [update(1, 3, field::POSITION, s)];
        assert_eq!(
            err(r.render(&frame(4, 64, &[], &[], &updates), &mut out)),
            room
        );
    }
    let size = ok_text(
        2,
        "Live Triple Balance requires a 48 kHz equal-size Object; diffuse must be 0 or 1 with nonzero size",
        "element=1; field=extent/diffuse",
    );
    for (w, h, d, diffuse) in [
        (0.5, 0.5, 0.25, 0.),
        (1.5, 1.5, 1.5, 0.),
        (-0.5, -0.5, -0.5, 0.),
        (0.5, 0.5, 0.5, 0.5),
        (0., 0., 0., 1.),
    ] {
        let mut s = state(field::EXTENT | field::DIFFUSE);
        (s.width, s.height, s.depth, s.diffuse) = (w, h, d, diffuse);
        let updates = [update(1, 3, field::EXTENT | field::DIFFUSE, s)];
        assert_eq!(
            err(r.render(&frame(4, 64, &[], &[], &updates), &mut out)),
            size
        );
    }
    assert!(
        out.iter().all(|&v| v == 0.),
        "only the accepted frame wrote"
    );

    let mut deep = Renderer::new(&config("22.2")).unwrap();
    deep.configure(1, &[object(1)]).unwrap();
    let mut s = state(field::POSITION);
    s.position = [0., 0.5, -1.];
    let updates = [update(1, 0, field::POSITION, s)];
    let mut out = vec![0.; 24 * 8];
    assert!(
        deep.render(&frame(1, 8, &[], &[], &updates), &mut out)
            .is_ok(),
        "22.2 reaches below"
    );

    let mut beds = Renderer::new(&config("9.1.6")).unwrap();
    beds.configure(1, &full_bed(b"")).unwrap();
    let fixed = ok_text(
        2,
        "Live Triple Balance bed routing is fixed",
        "element=100; field=position/extent/diffuse",
    );
    let mut out = vec![0.; 16 * 8];
    for (fields, edit) in [
        (field::POSITION, (|_: &mut State| {}) as fn(&mut State)),
        (field::EXTENT, |s: &mut State| s.depth = 0.1),
        (field::DIFFUSE, |s: &mut State| s.diffuse = 1.),
    ] {
        let mut s = state(fields);
        edit(&mut s);
        let updates = [update(100, 0, fields, s)];
        assert_eq!(
            err(beds.render(&frame(1, 8, &[], &[], &updates), &mut out)),
            fixed
        );
    }
}

/// Renders `frames` of `pcm` per element in slices of `pattern`, the updates at absolute frames.
fn pull(
    r: &mut Renderer,
    generation: u64,
    signals: &[(u64, &[f32])],
    updates: &[(u32, Update)],
    pattern: &[u32],
    channels: usize,
) -> Vec<f32> {
    let frames = signals[0].1.len() as u32;
    let mut out = Vec::new();
    let mut at = 0u32;
    let mut i = 0;
    while at < frames {
        let n = pattern[i % pattern.len()].min(frames - at);
        i += 1;
        let planes: Vec<Plane> = signals
            .iter()
            .map(|&(id, s)| plane(id, &s[at as usize..(at + n) as usize]))
            .collect();
        let local: Vec<Update> = updates
            .iter()
            .filter(|(f, _)| (at..at + n).contains(f))
            .map(|&(f, u)| Update {
                offset: f - at,
                ..u
            })
            .collect();
        let mut block = vec![0.; n as usize * channels];
        r.render(&frame(generation, n, &planes, &[], &local), &mut block)
            .unwrap();
        out.extend(block);
        at += n;
    }
    out
}

fn moving_updates() -> Vec<(u32, Update)> {
    let mut p = state(field::POSITION);
    p.position = [-0.5, 0.25, 0.5];
    let mut g = state(field::LINEAR_GAIN);
    g.linear_gain = 0.25;
    let mut e = state(field::EXTENT);
    (e.width, e.height, e.depth) = (0.4, 0.4, 0.4);
    vec![
        (1000, update(1, 0, field::POSITION, p)),
        (
            2500,
            Update {
                ramp: 300,
                ..update(1, 0, field::LINEAR_GAIN, g)
            },
        ),
        (3000, update(1, 0, field::EXTENT, e)),
        (
            3500,
            Update {
                jump: true,
                ..update(1, 0, field::POSITION, state(field::POSITION))
            },
        ),
    ]
}

#[test]
fn slices_are_independent_of_the_request_partition() {
    let input = sine(4096);
    for layout in ["7.1.4", "22.2"] {
        let channels = Renderer::new(&config(layout)).unwrap().output_channels() as usize;
        let render = |pattern: &[u32]| {
            let mut r = Renderer::new(&Config {
                smoothing_frames: 480,
                ..config(layout)
            })
            .unwrap();
            r.configure(1, &[object(1)]).unwrap();
            pull(
                &mut r,
                1,
                &[(1, &input)],
                &moving_updates(),
                pattern,
                channels,
            )
        };
        let whole = render(&[4096]);
        assert!(whole.iter().any(|&v| v != 0.));
        assert_eq!(
            whole,
            render(&[1, 7, 333, 512, 1000, 1024, 4097]),
            "{layout}"
        );
    }
}

#[test]
fn a_rejected_frame_changes_neither_output_nor_state_and_an_empty_one_commits_nothing() {
    let input = sine(2048);
    let fresh = || {
        let mut r = Renderer::new(&config("9.1.6")).unwrap();
        r.configure(1, &[object(1)]).unwrap();
        r
    };
    // An `active` update re-derives the level from the target gain.
    let mut updates = vec![(100, update(1, 0, field::ACTIVE, state(field::ACTIVE)))];
    updates.extend(moving_updates());
    let mut reference = fresh();
    let expected = pull(&mut reference, 1, &[(1, &input)], &updates, &[2048], 16);

    let mut r = fresh();
    let mut half = state(field::LINEAR_GAIN);
    half.linear_gain = 0.5;
    let mut outside = state(field::POSITION);
    outside.position = [2., 0., 0.];
    let rejected = [
        update(1, 0, field::LINEAR_GAIN, half),
        update(1, 1, field::POSITION, outside),
    ];
    let planes = [plane(1, &input[..64])];
    let mut out = vec![9.; 16 * 64];
    assert!(
        r.render(&frame(1, 64, &planes, &[], &rejected), &mut out)
            .is_err()
    );
    assert!(
        out.iter().all(|&v| v == 9.),
        "a rejection leaves the output"
    );
    // An empty slice validates and drives the mixer but keeps every target and initialization.
    let entries = [Entry {
        element_id: 1,
        state: half,
    }];
    assert!(r.render(&frame(1, 0, &[], &entries, &[]), &mut []).is_ok());
    assert_eq!(
        pull(&mut r, 1, &[(1, &input)], &updates, &[2048], 16),
        expected
    );
}

#[test]
fn clearing_a_field_restores_the_descriptor_default() {
    let input = sine(2048);
    let render = |second: Update| {
        let mut r = Renderer::new(&config("9.1.6")).unwrap();
        r.configure(1, &[object(1)]).unwrap();
        let mut e = state(field::EXTENT);
        (e.width, e.height, e.depth) = (0.5, 0.5, 0.5);
        let updates = [(0, update(1, 0, field::EXTENT, e)), (1024, second)];
        pull(&mut r, 1, &[(1, &input)], &updates, &[2048], 16)
    };
    let cleared = render(Update {
        cleared: field::EXTENT,
        ..update(1, 0, 0, state(0))
    });
    let zero = render(update(1, 0, field::EXTENT, state(field::EXTENT)));
    assert_eq!(cleared, zero);
    let mut kept = state(field::EXTENT);
    (kept.width, kept.height, kept.depth) = (0.5, 0.5, 0.5);
    assert_ne!(cleared, render(update(1, 0, field::EXTENT, kept)));
}

#[test]
fn spread_none_warns_new_elements_in_generation_order_and_drops_size() {
    let input = sine(256);
    let mut r = Renderer::new(&Config {
        spread: Spread::None,
        ..config("7.1.4")
    })
    .unwrap();
    let elements = [object(5), object(3), object(9), object(1)];
    r.configure(1, &elements).unwrap();
    let mut extent = state(field::EXTENT);
    (extent.width, extent.height, extent.depth) = (0.5, 0.25, 0.);
    let mut diffuse = state(field::DIFFUSE);
    diffuse.diffuse = 0.5;
    let entry = |id, s| Entry {
        element_id: id,
        state: s,
    };
    // Element order is 5, 3, 9, 1; the initial entries name them 1, 3, 5.
    let entries = [entry(1, extent), entry(3, diffuse), entry(5, extent)];
    let planes = [plane(5, &input[..64]), plane(1, &input[..64])];
    let mut out = vec![0.; 12 * 64];
    r.render(&frame(1, 64, &planes, &entries, &[]), &mut out)
        .unwrap();
    let warning = |element_id| Diagnostic {
        code: code::SEMANTIC_DEGRADED,
        element_id,
        field: field::EXTENT | field::DIFFUSE,
        message: SPREAD_NONE_WARNING,
    };
    assert_eq!(r.diagnostics(), &[warning(5), warning(3), warning(1)]);
    let mut plain = Renderer::new(&Config {
        spread: Spread::None,
        ..config("7.1.4")
    })
    .unwrap();
    plain.configure(1, &elements).unwrap();
    let mut expected = vec![0.; 12 * 64];
    plain
        .render(&frame(1, 64, &planes, &[], &[]), &mut expected)
        .unwrap();
    assert_eq!(out, expected, "spread=none renders points");
    r.render(&frame(1, 64, &[], &[], &[]), &mut out).unwrap();
    assert!(r.diagnostics().is_empty(), "once per element");
    let updates = [
        update(9, 1, field::EXTENT, extent),
        update(5, 2, field::EXTENT, extent),
    ];
    r.render(&frame(1, 64, &[], &[], &updates), &mut out)
        .unwrap();
    assert_eq!(r.diagnostics(), &[warning(9)]);
    // A rejected frame publishes nothing and does not consume the warning.
    r.configure(2, &elements).unwrap();
    let mut outside = state(field::POSITION);
    outside.position = [0., 0., 2.];
    let rejected = [
        update(5, 0, field::EXTENT, extent),
        update(3, 1, field::POSITION, outside),
    ];
    assert!(
        r.render(&frame(2, 64, &[], &[], &rejected), &mut out)
            .is_err()
    );
    assert!(r.diagnostics().is_empty());
    let accepted = [update(5, 0, field::EXTENT, extent)];
    r.render(&frame(2, 64, &[], &[], &accepted), &mut out)
        .unwrap();
    assert_eq!(
        r.diagnostics(),
        &[warning(5)],
        "a new generation warns again"
    );
}
