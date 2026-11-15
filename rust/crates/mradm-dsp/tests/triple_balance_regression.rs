//! Triple Balance regressions formerly in the C++ `triple_balance_222_test`,
//! `triple_balance_point_test`, `triple_balance_semantics_test` and
//! `triple_balance_size_processor_test` (R3 B), against the Rust kernels directly:
//! - the 22.2 room panner over a dense grid (symmetry, power, radius, continuity) and its size
//!   integral against an independent 3D midpoint integration;
//! - held-out reference gains of the point panner;
//! - the size object against captured Renderer PCM (late first event, rapid size reset), the
//!   decorrelator against its capture, and the captured spatial gains;
//! - object state: partition / reset / instance independence and the event checks.
use mradm_dsp::triple_balance::{
    Event, Layout, Position, panner,
    processor::{Decorrelator, FilteredFrame, Processor, Track},
};
use std::path::{Path, PathBuf};
use std::sync::Arc;

fn fixture(name: &str) -> PathBuf {
    Path::new(env!("CARGO_MANIFEST_DIR"))
        .join("../../../tests/fixtures")
        .join(name)
}

fn floats(path: &Path) -> Vec<f32> {
    let bytes = std::fs::read(path).unwrap();
    bytes
        .as_chunks::<4>()
        .0
        .iter()
        .map(|b| f32::from_le_bytes(*b))
        .collect()
}

fn p(x: f32, y: f32, z: f32) -> Position {
    Position { x, y, z }
}

fn event(start: u64, position: Position, size: f32) -> Event {
    Event {
        start,
        position,
        size,
    }
}

fn unit(state: &mut u32) -> f32 {
    *state = state.wrapping_mul(1664525).wrapping_add(1013904223);
    (*state >> 8) as f32 / 16_777_216.
}

fn object(events: &[Event], layout: Layout) -> Processor {
    Processor::new(Arc::new(
        Track::new(events.to_vec(), layout, 48000).unwrap(),
    ))
}

/// Pushes `input` in `chunk`s, then finishes (the former C++ `push` / `finish`).
fn render(processor: &mut Processor, input: &[f32], chunk: usize) -> Vec<f32> {
    let mut out = Vec::new();
    for block in input.chunks(chunk) {
        let mut part = vec![0.; processor.required(block.len(), false).unwrap()];
        processor.process(block, &mut part, false).unwrap();
        out.extend(part);
    }
    let mut tail = vec![0.; processor.required(0, true).unwrap()];
    processor.process(&[], &mut tail, true).unwrap();
    out.extend(tail);
    out
}

const MIRROR: [usize; 24] = [
    1, 0, 2, 3, 5, 4, 7, 6, 8, 9, 11, 10, 13, 12, 14, 15, 17, 16, 19, 18, 20, 21, 23, 22,
];

#[test]
fn room_222_gains_are_symmetric_power_preserving_and_continuous() {
    let mut worst = 0f64;
    for x in -4..=4 {
        for y in -4..=4 {
            for z in -4..=4 {
                for size in [0., 0.00001, 0.01, 0.19999, 0.2, 0.75, 1.] {
                    let at = p(x as f32 / 4., y as f32 / 4., z as f32 / 4.);
                    let gains = panner::room(at, size).unwrap();
                    let mirror = panner::room(p(-at.x, at.y, at.z), size).unwrap();
                    let power: f64 = gains.iter().map(|g| f64::from(*g) * f64::from(*g)).sum();
                    worst = worst.max((power - 1.).abs());
                    for (c, g) in gains.iter().enumerate() {
                        assert!(g.is_finite() && *g >= 0.);
                        assert!((g - mirror[MIRROR[c]]).abs() < 2e-6, "{at:?} {size} {c}");
                    }
                    assert_eq!((gains[3], gains[9]), (0., 0.), "Objects never feed an LFE");
                }
            }
        }
    }
    assert!(worst < 3e-6, "{worst}");
    // Full XYZ keeps the radius dependence along one direction.
    assert_ne!(
        panner::room(p(0., 0.1, 0.), 0.).unwrap(),
        panner::room(p(0., 1., 0.), 0.).unwrap()
    );
    // Layers and vertices join continuously.
    for node in panner::NODES {
        for size in [0., 0.01, 0.2, 1.] {
            let at = node.position;
            let below = panner::room(p(at.x, at.y, (at.z - 1e-5).clamp(-1., 1.)), size).unwrap();
            let above = panner::room(p(at.x, at.y, (at.z + 1e-5).clamp(-1., 1.)), size).unwrap();
            for (a, b) in below.iter().zip(above) {
                assert!((a - b).abs() < 2e-4, "node {} size {size}", node.channel);
            }
        }
    }
    assert!(panner::room(p(0., 0., f32::NAN), 0.).is_err());
    assert!(panner::room(p(0., 0., -1.01), 0.).is_err());
}

#[test]
fn room_222_size_integration_matches_a_midpoint_integral() {
    const STEPS: usize = 24;
    let mut random = 0x0222_0927u32;
    let mut worst = 0f64;
    for _ in 0..32 {
        let center = p(
            unit(&mut random) * 2. - 1.,
            unit(&mut random) * 2. - 1.,
            unit(&mut random) * 2. - 1.,
        );
        let size = 0.01 + unit(&mut random) * 0.99;
        let exact = panner::room(center, size).unwrap();
        let c = [center.x, center.y, center.z];
        let lo = c.map(|v| (-1f32).max(v - size));
        let step: Vec<f32> = (0..3)
            .map(|i| ((1f32).min(c[i] + size) - lo[i]) / STEPS as f32)
            .collect();
        let mut measured = [0f64; 24];
        for zi in 0..STEPS {
            for yi in 0..STEPS {
                for xi in 0..STEPS {
                    let at = p(
                        lo[0] + (xi as f32 + 0.5) * step[0],
                        lo[1] + (yi as f32 + 0.5) * step[1],
                        lo[2] + (zi as f32 + 0.5) * step[2],
                    );
                    for (m, g) in measured.iter_mut().zip(panner::room(at, 0.).unwrap()) {
                        *m += f64::from(g) * f64::from(g);
                    }
                }
            }
        }
        for (m, e) in measured.iter().zip(exact) {
            let mean = m / (STEPS * STEPS * STEPS) as f64;
            worst = worst.max((mean - f64::from(e) * f64::from(e)).abs());
        }
    }
    assert!(worst < 0.0015, "{worst}");
}

#[test]
#[allow(clippy::approx_constant)] // 0.4342 is a measured coordinate, not log10(e).
fn point_gains_match_held_out_reference_measurements() {
    // Measured from a held-out direct-ADM reference bank, in multi-mono speaker order; these
    // coordinates were not used to fit the room grid.
    let cases: [(Layout, [f32; 3], &[f32]); 2] = [
        (
            Layout::Seven,
            [0.3395, 0.4342, 0.9594],
            &[
                0.,
                0.020_182_73,
                0.033891,
                0.,
                0.024_148_26,
                0.042_488_26,
                0.,
                0.,
                0.259_347_14,
                0.955_344_8,
                0.033_253_71,
                0.122_495_15,
            ],
        ),
        (
            Layout::Nine,
            [-0.8526, -0.7582, 0.4199],
            &[
                0.,
                0.,
                0.,
                0.,
                0.289_758_39,
                0.033_921_62,
                0.736_156_9,
                0.086_180_87,
                0.,
                0.,
                0.,
                0.,
                0.,
                0.,
                0.604595,
                0.,
            ],
        ),
    ];
    for (layout, [x, y, z], reference) in cases {
        let gains = panner::point(p(x, y, z), 1., layout).unwrap();
        let (mut error, mut power) = (0f64, 0f64);
        for (g, r) in gains[..reference.len()].iter().zip(reference) {
            assert!(g.is_finite());
            error += (f64::from(*g) - f64::from(*r)).powi(2);
            power += f64::from(*r).powi(2);
        }
        assert_eq!(gains[3], 0.);
        assert!((error / power).sqrt() <= 0.01, "{layout:?}");
    }
    for layout in [Layout::Seven, Layout::Room222] {
        assert!(panner::point(p(1.1, 0., 0.), 1., layout).is_err());
    }
}

#[test]
fn a_late_first_event_matches_the_captured_renderer() {
    let dir = fixture("room_compat_semantics");
    let input = floats(&dir.join("input.f32"));
    assert_eq!(input.len(), 52224);
    // The only event is at 48000 (size 0.25): the object starts at the default origin point.
    let events = [
        event(0, p(0., 0., 0.).internal(), 0.),
        event(48000, p(0.23, 0.51, 0.37).internal(), 0.25),
    ];
    for (layout, name) in [(Layout::Seven, "7.1.4.f32"), (Layout::Nine, "9.1.6.f32")] {
        let expected = floats(&dir.join(name));
        let channels = layout.channels();
        assert_eq!(expected.len(), 5120 * channels);
        let mut processor = object(&events, layout);
        let baseline = render(&mut processor, &input, 1);
        for chunk in [31, 32, 257, 511, 512, 513, 1024] {
            processor.reset();
            assert_eq!(
                render(&mut processor, &input, chunk),
                baseline,
                "chunk {chunk}"
            );
        }
        let error = expected
            .iter()
            .zip(&baseline[47104 * channels..])
            .fold(0f32, |m, (e, a)| m.max((e - a).abs()));
        assert!(error < 1e-4, "{layout:?}: {error}");
    }
}

#[test]
fn size_kernels_match_their_captures() {
    let dir = fixture("room_compat_size");
    let reference = mradm_json::parse(&std::fs::read(dir.join("reference.json")).unwrap()).unwrap();
    let gains = mradm_json::at(&reference, "gains")
        .unwrap()
        .as_array()
        .unwrap();
    assert_eq!(gains.len(), 24);
    for item in gains {
        let q: Vec<i32> = mradm_json::at(item, "parameters")
            .unwrap()
            .as_array()
            .unwrap()
            .iter()
            .map(|v| v.as_i32().unwrap())
            .collect();
        let actual = panner::raw([q[0], q[1], q[2], q[3]]).unwrap();
        let expected = mradm_json::at(item, "expected")
            .unwrap()
            .as_array()
            .unwrap();
        for (a, e) in actual.iter().zip(expected) {
            assert!((a - e.as_f32().unwrap()).abs() <= 2e-6, "{q:?}");
        }
    }
    assert!(panner::quantize(p(0., 0., 0.), f32::NAN).is_err());

    // The four recursive filter outputs against the Renderer capture.
    let input = floats(&dir.join("filter-input.f32"));
    let expected = floats(&dir.join("filter-expected.f32"));
    assert_eq!((input.len(), expected.len()), (2048, 2048));
    let mut filter = Decorrelator::default();
    let mut filtered = Vec::new();
    for block in input.as_chunks::<32>().0 {
        let mut out = [FilteredFrame::default(); 32];
        filter.process(block, &mut out);
        filtered.extend(out.iter().flatten());
    }
    let error = expected
        .iter()
        .zip(&filtered[1536 * 4..])
        .fold(0f32, |m, (e, a): (&f32, &f32)| m.max((e - a).abs()));
    assert!(error <= 2e-6, "{error}");

    // A zero-size block completes its ramp and resets before the size returns.
    let input = floats(&dir.join("rapid-input.f32"));
    let expected = floats(&dir.join("rapid-expected.f32"));
    let at = p(0.625, 0.375, 0.75);
    let events = [
        event(0, at, 0.25),
        event(24576, at, 0.),
        event(28672, at, 0.25),
    ];
    let output = render(&mut object(&events, Layout::Nine), &input, 257);
    assert_eq!((output.len(), expected.len()), (32000 * 16, 2048 * 16));
    let error = expected
        .iter()
        .zip(&output[28672 * 16..])
        .fold(0f32, |m, (e, a)| m.max((e - a).abs()));
    assert!(error < 1e-4, "{error}");
}

fn noise(frames: usize, active: usize, seed: u32) -> Vec<f32> {
    let mut s = seed;
    (0..frames)
        .map(|i| {
            s = s.wrapping_mul(1664525).wrapping_add(1013904223);
            if i < active {
                ((s >> 8) as i32 - 8_388_608) as f32 / 67_108_864.
            } else {
                0.
            }
        })
        .collect()
}

fn check_object(events: &[Event], layout: Layout, input: &[f32]) -> Vec<f32> {
    let channels = layout.channels();
    let mut base = object(events, layout);
    let wanted = render(&mut base, input, 512);
    assert_eq!(
        wanted.len(),
        input.len() * channels,
        "the output keeps the input length"
    );
    assert!(wanted.iter().all(|x| x.is_finite()));
    for frame in wanted.chunks_exact(channels) {
        assert_eq!(frame[3], 0., "Objects never write the LFE");
        if layout == Layout::Room222 {
            assert_eq!(frame[9], 0.);
        }
    }
    for chunk in [1, 31, 32, 257, 511, 512, 513, 1024] {
        base.reset();
        assert_eq!(render(&mut base, input, chunk), wanted, "chunk {chunk}");
    }
    // Interleaved instances share no state.
    let mut second = object(events, layout);
    base.reset();
    let (mut a, mut b) = (Vec::new(), Vec::new());
    for block in input.chunks(257) {
        for (p, out) in [(&mut base, &mut a), (&mut second, &mut b)] {
            let mut part = vec![0.; p.required(block.len(), false).unwrap()];
            p.process(block, &mut part, false).unwrap();
            out.extend(part);
        }
    }
    for (p, out) in [(&mut base, &mut a), (&mut second, &mut b)] {
        let mut tail = vec![0.; p.required(0, true).unwrap()];
        p.process(&[], &mut tail, true).unwrap();
        out.extend(tail);
    }
    assert_eq!((&a, &b), (&wanted, &wanted));
    assert!(
        base.process(&[], &mut [], false).is_err(),
        "a finished object needs a reset"
    );
    wanted
}

#[test]
fn size_objects_are_independent_of_partitions_resets_and_instances() {
    let events = [
        event(0, p(0.5, 0., 0.), 0.01),
        event(600, p(0.25, 0.6, 0.5), 0.25),
        event(2111, p(0.8, 0.2, 0.75), 1.),
        event(4096, p(0.5, 0.5, 1.), 0.),
        event(6200, p(0.5, 0.5, 0.), 0.1),
    ];
    let signal = noise(8457, 7600, 0x7369_7A65);
    for layout in [Layout::Seven, Layout::Nine] {
        check_object(&events, layout, &signal);
    }
    assert!(
        Track::new(events.to_vec(), Layout::Nine, 44100).is_err(),
        "unverified rate"
    );
    // Dense initial metadata uses the last target without delaying later events.
    let mut dense = events;
    dense[1].start = 1;
    let mut selected = events[1..].to_vec();
    selected[0].start = 0;
    assert_eq!(
        render(&mut object(&dense, Layout::Seven), &signal, 257),
        render(&mut object(&selected, Layout::Seven), &signal, 512)
    );
    dense[2].start = 0;
    assert!(
        Track::new(dense.to_vec(), Layout::Seven, 48000).is_err(),
        "backwards metadata"
    );

    // The 22.2 room: no LFE leakage, and the silent tail resets within the file.
    let events = [
        event(0, p(0.5, 0.5, 0.), 0.),
        event(513, p(0.2, 0.8, -0.7), 0.01),
        event(2048, p(0.5, 0.5, 1.), 0.2),
        event(4095, p(0.8, 0.2, 0.2), 1.),
        event(6144, p(0.5, 0.5, -1.), 0.),
        event(8193, p(0.25, 0.25, 0.), 0.25),
    ];
    let mut seed = 0x222A;
    let mut input = vec![0f32; 14473];
    for x in &mut input[..10240] {
        *x = (unit(&mut seed) - 0.5) * 0.25;
    }
    let wanted = check_object(&events, Layout::Room222, &input);
    assert!(wanted[(input.len() - 511) * 24..].iter().all(|x| *x == 0.));
    // A different object interleaved with the first shares neither filters nor caches.
    let other_events = events.map(|mut e| {
        e.position.x = 1. - e.position.x;
        e.position.z = -e.position.z;
        e
    });
    let other_input: Vec<f32> = input.iter().map(|x| x * 0.37).collect();
    let other_expected = render(
        &mut object(&other_events, Layout::Room222),
        &other_input,
        512,
    );
    let (mut first, mut other) = (
        object(&events, Layout::Room222),
        object(&other_events, Layout::Room222),
    );
    let (mut a, mut b) = (Vec::new(), Vec::new());
    for (x, y) in input.chunks(257).zip(other_input.chunks(257)) {
        for (p, block, out) in [(&mut first, x, &mut a), (&mut other, y, &mut b)] {
            let mut part = vec![0.; p.required(block.len(), false).unwrap()];
            p.process(block, &mut part, false).unwrap();
            out.extend(part);
        }
    }
    for (p, out) in [(&mut first, &mut a), (&mut other, &mut b)] {
        let mut tail = vec![0.; p.required(0, true).unwrap()];
        p.process(&[], &mut tail, true).unwrap();
        out.extend(tail);
    }
    assert_eq!((a, b), (wanted, other_expected));
}
