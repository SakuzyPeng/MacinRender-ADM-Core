//! Whole HOA encodes through the render service (formerly the C++ `hoa_encode_fixture_test`,
//! R3 C1): first-order components of front, left and offset sources, object mute and gain, jump and
//! interpolated block changes, diffuse, extent and divergence, DirectSpeakers positions and the
//! RC_LFE label, and the LFE's separate True Peak with no loudness. Backend-internal rules (same-
//! start blocks, live bindings, label fallback, errors, premature input end, windows and file
//! streaming against the offline render, buffer rules and live gains) are `mradm-render`'s
//! `hoa.rs`; the stream's prepared / input check is `stream.rs`.
mod common;
use common::render::*;
use mradm_engine::request::Selection;
use mradm_testkit::adm::*;

const FRAMES: usize = 1000;

fn jump() -> Param {
    jump_position([Param::text(true)])
}

fn hoa_request(dir: &Dir, input: &std::path::Path) -> mradm_engine::render::Request {
    request(input, &dir.file("out.wav"), Selection::Hoa, "hoa3")
}

/// The C++ fixture: every sample 1.0, the block gain 0.5.
fn encode(single: Single, edit: impl FnOnce(&mut mradm_engine::render::Request)) -> (Run, Audio) {
    let dir = scratch("hoa");
    let path = dir.file("in.wav");
    let (axml, uid) = single.built();
    input(&path, &axml, &[&uid], 48000, FRAMES, 1.0);
    let mut r = hoa_request(&dir, &path);
    edit(&mut r);
    render(r)
}

fn encode_samples(single: Single, samples: &[f32]) -> Audio {
    let dir = scratch("hoa-noise");
    let path = dir.file("in.wav");
    let (axml, uid) = single.built();
    let mut wave = mradm_testkit::wave::Wave::new(1, 48000, 24);
    wave.chna = Some(vec![mradm_testkit::wave::AudioId::new(1, &uid, "", "")]);
    wave.axml = Some(axml);
    wave.write(&path, samples).unwrap();
    let mut r = hoa_request(&dir, &path);
    r.peak_limit = false;
    render(r).1
}

fn noise(frames: usize) -> Vec<f32> {
    let mut state = 0x5a17_c0deu32;
    (0..frames)
        .map(|_| {
            state = state.wrapping_mul(1_664_525).wrapping_add(1_013_904_223);
            (((state >> 8) & 0xffff) as f32 / 32768. - 1.) * 0.5
        })
        .collect()
}

fn rms_in(audio: &Audio, range: std::ops::Range<usize>) -> Vec<f64> {
    let mut sums = vec![0f64; audio.channels];
    for f in range.clone() {
        for (c, s) in sums.iter_mut().enumerate() {
            *s += f64::from(audio.samples[f * audio.channels + c]).powi(2);
        }
    }
    sums.iter()
        .map(|s| (s / range.len() as f64).sqrt())
        .collect()
}

fn rms(audio: &Audio) -> Vec<f64> {
    rms_in(audio, 0..audio.frames)
}

fn source(az: f32, rest: Vec<Param>) -> Single {
    let mut block = polar(az, 0., [gain_linear(0.5)]);
    for p in rest {
        block.set(p);
    }
    Single::objects(vec![block])
}

#[test]
fn point_sources_encode_their_direction() {
    let (_, audio) = encode(source(0., vec![]), |_| {});
    assert_eq!(
        (audio.channels, audio.rate, audio.frames),
        (16, 48000, FRAMES)
    );
    let r = rms(&audio);
    // ACN order: W, Y, Z, X.
    assert!(
        (0.4..0.6).contains(&r[0]) && (0.4..0.6).contains(&r[3]),
        "{r:?}"
    );
    assert!(r[1] < 1e-4 && r[2] < 1e-4);
    let r = rms(&encode(source(90., vec![]), |_| {}).1);
    assert!(
        (0.4..0.6).contains(&r[0]) && (0.4..0.6).contains(&r[1]),
        "{r:?}"
    );
    assert!(r[2] < 1e-4 && r[3] < 1e-4);

    let mut offset = source(0., vec![]);
    offset
        .doc
        .set(offset.object, spherical_offset([azimuth(90.0f32)]));
    let r = rms(&encode(offset, |_| {}).1);
    assert!(r[1] > 0.3 && r[3] < 0.05, "{r:?}");
}

#[test]
fn object_mute_and_gain_apply() {
    let mut muted = source(0., vec![]);
    muted.doc.set(muted.object, mute(true));
    assert!(rms(&encode(muted, |_| {}).1).iter().all(|r| *r < 1e-6));
    let mut half = source(0., vec![]);
    half.doc.set(half.object, gain_linear(0.5));
    let w = rms(&encode(half, |_| {}).1)[0];
    assert!((0.15..0.35).contains(&w), "{w}");
}

#[test]
fn block_changes_jump_or_ramp() {
    let blocks = |jumped: bool| {
        let first = polar(0., 0., [gain_linear(0.5), duration(10 * MS)]);
        let mut second = polar(90., 0., [gain_linear(0.5), rtime(10 * MS)]);
        if jumped {
            second.set(jump());
        }
        Single::objects(vec![first, second])
    };
    let r = rms(&encode(blocks(true), |_| {}).1);
    assert!(r[0] > 0.3 && r[1] > 0.1 && r[3] > 0.1, "{r:?}");
    let (_, audio) = encode(blocks(false), |r| r.object_smoothing_frames = 0);
    let at = |frame: usize, channel: usize| audio.samples[frame * 16 + channel];
    assert!(
        at(480, 3) > 0.1 && at(480, 1) < 0.01,
        "the ramp starts at the earlier block"
    );
    assert!(
        at(720, 3) < 0.01 && at(720, 1) > 0.3,
        "and ends at the later one"
    );
}

#[test]
fn diffuse_extent_and_divergence_reduce_directionality() {
    let samples = noise(4096);
    let direct = rms(&encode_samples(source(0., vec![]), &samples));
    let diffused = rms(&encode_samples(source(0., vec![diffuse(1.0f32)]), &samples));
    assert!(direct[1] < 1e-5 && direct[2] < 1e-5);
    assert!(diffused[0] > 0.01 && diffused[1] < 1e-4 && diffused[2] < 1e-4);
    assert!(diffused[3] < direct[3] * 0.9);

    let changing = Single::objects(vec![
        polar(
            0.,
            0.,
            [gain_linear(0.5), diffuse(0.0f32), duration(10 * MS)],
        ),
        polar(0., 0., [gain_linear(0.5), diffuse(1.0f32), rtime(10 * MS)]),
    ]);
    let audio = encode_samples(changing, &samples);
    let (start, end) = (rms_in(&audio, 0..256), rms_in(&audio, 4096 - 256..4096));
    assert!(
        start[3] > 0.05 && end[3] < start[3] * 0.8,
        "{start:?} {end:?}"
    );
    assert!(end[1] < 1e-4 && end[2] < 1e-4);

    let point = rms(&encode(source(0., vec![]), |r| r.peak_limit = false).1);
    let extent = |p: Param| rms(&encode(source(0., vec![p]), |r| r.peak_limit = false).1);
    let w = extent(width(1.0f32));
    assert!(
        (0.4..0.6).contains(&w[0]) && w[3] < point[3] * 0.9 && w[1] < 1e-4,
        "{w:?}"
    );
    let h = extent(height(1.0f32));
    assert!(
        (0.4..0.6).contains(&h[0]) && h[3] < point[3] * 0.95 && h[2] < 1e-4,
        "{h:?}"
    );
    let d = extent(depth(1.0f32));
    assert!(d[3] < point[3] * 0.98, "{d:?}");

    let divergence = || object_divergence([Param::text(1.0f32), azimuth_range(60.0f32)]);
    let v = extent(divergence());
    assert!(
        (0.4..0.6).contains(&v[0]) && v[1] < 1e-4 && v[3] < point[3] * 0.7,
        "{v:?}"
    );
    let plain = rms(&encode(source(0., vec![diffuse(1.0f32)]), |r| r.peak_limit = false).1);
    let both = rms(
        &encode(source(0., vec![diffuse(1.0f32), divergence()]), |r| {
            r.peak_limit = false
        })
        .1,
    );
    assert!(
        both[0] > 0.01 && both[1] < 1e-4 && both[3] < plain[3] * 0.8,
        "{both:?}"
    );
}

#[test]
fn direct_speakers_encode_their_position() {
    let ds = |label: &str, az: f32, el: f32| {
        rms(&encode(Single::speaker(label, Some((az, el))), |r| {
            r.peak_limit = false
        })
        .1)
    };
    let r = ds("M+000", 0., 0.);
    assert!(
        r[0] > 0.9 && r[1] < 1e-3 && r[2] < 1e-3 && r[3] > 0.9,
        "{r:?}"
    );
    let r = ds("M+030", 30., 0.);
    assert!(
        r[0] > 0.9 && (0.3..0.7).contains(&r[1]) && r[2] < 1e-3 && (0.7..0.95).contains(&r[3]),
        "{r:?}"
    );
    // An LFE label is omnidirectional and not low-passed.
    let r = ds("RC_LFE", 45., -30.);
    assert!(r[0] > 0.9 && r[1..].iter().all(|v| *v < 1e-3), "{r:?}");
}

#[test]
fn lfe_only_scenes_measure_true_peak_without_loudness() {
    let dir = scratch("hoa-lfe");
    let path = dir.file("in.wav");
    let (axml, uid) = Single::speaker("RC_LFE", Some((0., 0.))).built();
    input(&path, &axml, &[&uid], 48000, 48000, 0.8);
    let mut r = hoa_request(&dir, &path);
    r.peak_limit = false;
    let (run, _) = render(r);
    let metrics = run.outcome.metrics.unwrap();
    // The LFE's own tracker sees 0.8; the AllRAD reference alone would give about -12 dBTP.
    assert!(metrics.peak_dbtp.unwrap() > -6.);
    assert!(metrics.lufs.is_none());
}
