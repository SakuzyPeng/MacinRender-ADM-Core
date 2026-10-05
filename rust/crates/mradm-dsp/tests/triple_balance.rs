use mradm_dsp::triple_balance::{
    Event, Layout, Position, panner,
    processor::{Processor, Track},
    session::{Plan, RowInput, Session},
};
use std::sync::Arc;
fn events() -> Vec<Event> {
    vec![
        Event {
            start: 0,
            position: Position {
                x: 0.25,
                y: 0.4,
                z: 0.5,
            },
            size: 0.25,
        },
        Event {
            start: 600,
            position: Position {
                x: 0.8,
                y: 0.2,
                z: 0.1,
            },
            size: 0.,
        },
        Event {
            start: 2111,
            position: Position {
                x: 0.5,
                y: 0.7,
                z: 0.8,
            },
            size: 1.,
        },
    ]
}
fn plan(layout: Layout) -> Arc<Plan> {
    let rows = [RowInput {
        input: 1,
        event_offset: 0,
        event_count: 3,
        bed_offset: 0,
        size_index: 0,
        kind: 2,
        gain: 0.75,
    }];
    Arc::new(Plan::new(2, layout, 48000, 10000, &rows, &events(), &[]).unwrap())
}
fn samples(n: usize) -> Vec<f32> {
    (0..n).map(|i| ((i * 19 % 71) as f32 - 35.) / 64.).collect()
}
fn object(layout: Layout) -> Processor {
    Processor::new(Arc::new(Track::new(events(), layout, 48000).unwrap()))
}
#[test]
fn independent_quantization_routes_and_geometry() {
    assert_eq!(
        panner::quantize(
            Position {
                x: 0.,
                y: 0.5,
                z: 1.
            },
            1.
        )
        .unwrap(),
        [0, 16384, 32767, 32767]
    );
    for c in [0, 1, 100, 16383, 32766] {
        let x = (c as f32 + 0.5) / 32768.;
        let below = f32::from_bits(x.to_bits() - 1);
        let q = panner::quantize(
            Position {
                x: below,
                y: x,
                z: x,
            },
            0.,
        )
        .unwrap();
        assert_eq!(q[0], ((below * 32768. + 0.5).floor() as i32).min(32767));
        assert_eq!(q[1], c + 1);
    }
    for layout in [Layout::Seven, Layout::Nine] {
        let g = panner::point(
            Position {
                x: -1.,
                y: 1.,
                z: 0.,
            },
            1.,
            layout,
        )
        .unwrap();
        assert_eq!(g[0], 1.);
        assert_eq!(g[3], 0.);
        for (c, &v) in g.iter().enumerate() {
            if c != 0 {
                assert_eq!(v, 0.);
            }
        }
    }
    for n in panner::NODES {
        let g = panner::room(n.position, 0.).unwrap();
        for (c, v) in g.iter().enumerate() {
            assert!((v - if c == n.channel { 1. } else { 0. }).abs() < 1e-6);
        }
        assert_eq!(g[3], 0.);
        assert_eq!(g[9], 0.);
    }
    for z in [-1., -0.01, 0., 0.2, 1.] {
        for size in [0., 0.00001, 0.01, 0.2, 1.] {
            let g = panner::room(
                Position {
                    x: 0.37,
                    y: -0.21,
                    z,
                },
                size,
            )
            .unwrap();
            let power: f64 = g.iter().map(|v| f64::from(*v) * f64::from(*v)).sum();
            assert!((power - 1.).abs() < 3e-6);
        }
    }
}
#[test]
fn object_chunks_finish_reset_and_snapshot() {
    let input = samples(3207);
    for layout in [Layout::Seven, Layout::Nine, Layout::Room222] {
        let channels = layout.channels();
        let mut reference = object(layout);
        let mut expected = vec![0.; input.len() * channels];
        assert_eq!(
            reference.process(&input, &mut expected, true).unwrap(),
            input.len()
        );
        for chunk in [1, 31, 32, 33, 257, 511, 512, 513, 1023, 1024] {
            let mut p = object(layout);
            let mut actual = Vec::new();
            for s in input.chunks(chunk) {
                let n = p.required(s.len(), false).unwrap();
                let mut out = vec![0.; n];
                p.process(s, &mut out, false).unwrap();
                actual.extend(out);
            }
            let mut tail = vec![0.; p.required(0, true).unwrap()];
            p.process(&[], &mut tail, true).unwrap();
            actual.extend(tail);
            assert_eq!(actual, expected);
            assert!(p.process(&[], &mut [], true).is_err());
            p.reset();
            let mut again = vec![0.; expected.len()];
            p.process(&input, &mut again, true).unwrap();
            assert_eq!(again, expected);
        }
        let mut p = object(layout);
        p.process(&input[..257], &mut [], false).unwrap();
        let snap = p.snapshot();
        let mut a = vec![0.; 1024 * channels];
        p.process(&input[257..1024], &mut a, false).unwrap();
        p.restore(&snap).unwrap();
        let mut b = a.clone();
        p.process(&input[257..1024], &mut b, false).unwrap();
        assert_eq!(a, b);
        let other = object(layout).snapshot();
        assert!(p.restore(&other).is_err());
    }
}
#[test]
fn object_errors_are_atomic_and_zero_frames_do_nothing() {
    let track = Arc::new(Track::new(events(), Layout::Nine, 48000).unwrap());
    let mut a = Processor::new(Arc::clone(&track));
    let mut b = Processor::new(track);
    let input = samples(1024);
    let mut x = vec![0.125; 1024 * 16];
    let mut y = x.clone();
    a.process(&[], &mut [], false).unwrap();
    assert!(a.set_scale(f32::NAN).is_err());
    let mut bad = input.clone();
    bad[999] = f32::NAN;
    assert!(a.process(&bad, &mut x, false).is_err());
    assert!(a.process(&input, &mut x[..10], false).is_err());
    assert_eq!(x, y);
    a.process(&input, &mut x, false).unwrap();
    b.process(&input, &mut y, false).unwrap();
    assert_eq!(x, y);
}
#[test]
fn shared_sessions_controls_snapshots_and_errors() {
    for layout in [Layout::Seven, Layout::Nine, Layout::Room222] {
        let shared_plan = plan(layout);
        let mut a = Session::new(Arc::clone(&shared_plan), 1024, 960, true).unwrap();
        let mut b = Session::new(Arc::clone(&shared_plan), 1024, 960, true).unwrap();
        let source = samples(2048);
        let mut x = vec![0.125; 1024 * layout.channels()];
        let mut y = x.clone();
        a.prepare_points(0, 1024).unwrap();
        b.prepare_points(0, 1024).unwrap();
        a.process(&source, &mut x, &[], 0, 1024, false).unwrap();
        b.process(&source, &mut y, &[], 0, 1024, false).unwrap();
        assert_eq!(x, y);
        let mut snap = a.snapshot();
        a.capture(&mut snap).unwrap();
        a.prepare_points(1024, 1024).unwrap();
        b.prepare_points(1024, 1024).unwrap();
        x.fill(0.125);
        y.fill(0.125);
        assert!(a.set_scales(&[f32::NAN], false).is_err());
        assert!(a.set_scales(&[], false).is_err());
        assert!(a.set_matrix(&[-1.]).is_err());
        assert!(a.prepare_points(5, 1024).is_err());
        let mut bad = source.clone();
        bad[1983] = f32::INFINITY;
        assert!(a.process(&bad, &mut x, &[], 1024, 1024, false).is_err());
        assert!(
            a.process(&source, &mut x[..17], &[], 1024, 1024, false)
                .is_err()
        );
        assert!(
            a.process(&source, &mut x, &[1.], 1024, 1024, false)
                .is_err()
        );
        assert!(a.process(&source, &mut x, &[], 1025, 1024, false).is_err());
        assert_eq!(x, y);
        a.process(&source, &mut x, &[], 1024, 1024, false).unwrap();
        b.process(&source, &mut y, &[], 1024, 1024, false).unwrap();
        assert_eq!(x, y);
        a.restore(&snap).unwrap();
        a.prepare_points(1024, 1024).unwrap();
        let mut z = vec![0.125; x.len()];
        a.process(&source, &mut z, &[], 1024, 1024, false).unwrap();
        assert_eq!(x, z);
        let other = Session::new(plan(layout), 1024, 960, true)
            .unwrap()
            .snapshot();
        assert!(a.restore(&other).is_err());
        assert!(
            a.capture(
                &mut Session::new(Arc::clone(&shared_plan), 1024, 960, false)
                    .unwrap()
                    .snapshot()
            )
            .is_err()
        );
        a.reset();
        a.process(&[], &mut [], &[], 0, 0, false).unwrap();
        a.prepare_points(0, 0).unwrap();
        a.prepare_points(0, 1024).unwrap();
        let mut reset = vec![0.125; x.len()];
        a.process(&source, &mut reset, &[], 0, 1024, false).unwrap();
        b.reset();
        b.prepare_points(0, 1024).unwrap();
        y.fill(0.125);
        b.process(&source, &mut y, &[], 0, 1024, false).unwrap();
        assert_eq!(reset, y);
    }
}
#[test]
fn preparation_rejects_bad_events_and_capacity() {
    let mut e = events();
    e[1].start = 1;
    assert!(Track::new(e, Layout::Seven, 48000).is_err());
    assert!(Track::new(events(), Layout::Seven, 44100).is_err());
    assert!(Track::new(vec![], Layout::Seven, 48000).is_err());
    assert!(Session::new(plan(Layout::Seven), 0, 960, true).is_err());
    assert!(Session::new(plan(Layout::Seven), usize::MAX, 960, true).is_err());
    let p = plan(Layout::Seven);
    let mut s = Session::new(p, 1024, 960, true).unwrap();
    assert!(s.prepare_points(u64::MAX, 1024).is_err());
    assert!(s.prepare_points(0, 1025).is_err());
    s.prepare_points(0, 1024).unwrap();
}

#[test]
fn saved_prepared_curve_is_rebuilt_before_repeated_accumulation() {
    let mut s = Session::new(plan(Layout::Nine), 1024, 960, true).unwrap();
    s.prepare_points(0, 1024).unwrap();
    let snapshot = s.snapshot();
    let input = samples(2048);
    let mut a = vec![0.; 1024 * 16];
    s.point(0, &input, &mut a, &[], 0, 1024, true).unwrap();
    s.process(&input, &mut a, &[], 0, 1024, false).unwrap();
    s.restore(&snapshot).unwrap();
    let mut b = vec![0.; 1024 * 16];
    s.point(0, &input, &mut b, &[], 0, 1024, true).unwrap();
    s.process(&input, &mut b, &[], 0, 1024, false).unwrap();
    assert_eq!(a, b);
}

#[test]
fn exact_simple_gain_delay_and_multi_lane_rejection() {
    use mradm_dsp::triple_balance::processor::Decorrelator;
    let event = Event {
        start: 0,
        position: Position {
            x: -1.,
            y: 1.,
            z: 0.,
        },
        size: 0.,
    };
    let row = RowInput {
        input: 0,
        event_offset: 0,
        event_count: 1,
        bed_offset: 0,
        size_index: 0,
        kind: 2,
        gain: 0.5,
    };
    let prepared =
        Arc::new(Plan::new(1, Layout::Seven, 48000, 1000, &[row], &[event], &[]).unwrap());
    let mut session = Session::new(prepared, 1024, 960, false).unwrap();
    let input = samples(31);
    let mut output = vec![0.125; 31 * 12];
    session
        .process(&input, &mut output, &[0.5; 31], 0, 31, true)
        .unwrap();
    for (f, frame) in output.chunks(12).enumerate() {
        for (c, &v) in frame.iter().enumerate() {
            let expected = if c == 0 {
                0.125 + ((input[f] * 1.) * 0.5) * 0.5
            } else {
                0.125
            };
            assert_eq!(v.to_bits(), expected.to_bits());
        }
    }
    let mut filter = Decorrelator::default();
    let mut impulse = [0.; 32];
    impulse[31] = 1.;
    let mut filtered = [[0.; 4]; 32];
    for block in 0..4 {
        filter.process(&impulse, &mut filtered);
        for (frame, v) in filtered.iter().enumerate() {
            if block * 32 + frame < 127 {
                assert!(v.iter().all(|x| x.to_bits() == 0));
            }
        }
        impulse.fill(0.);
    }
    assert!(filtered[31].iter().all(|v| *v != 0.));
    let mut second = row;
    second.input = 1;
    second.size_index = 1;
    let plan = Arc::new(
        Plan::new(
            2,
            Layout::Seven,
            48000,
            10000,
            &[row, second],
            &[event],
            &[],
        )
        .unwrap(),
    );
    let mut a = Session::new(Arc::clone(&plan), 1024, 960, false).unwrap();
    let mut b = Session::new(plan, 1024, 960, false).unwrap();
    let mut pcm = [0.25; 2048];
    pcm[2047] = f32::NAN;
    let mut out = vec![0.125; 1024 * 12];
    let mut reference = out.clone();
    assert!(a.process(&pcm, &mut out, &[], 0, 1024, false).is_err());
    assert_eq!(out, reference);
    pcm[2047] = 0.25;
    a.process(&pcm, &mut out, &[], 0, 1024, false).unwrap();
    b.process(&pcm, &mut reference, &[], 0, 1024, false)
        .unwrap();
    assert_eq!(out, reference);
}
