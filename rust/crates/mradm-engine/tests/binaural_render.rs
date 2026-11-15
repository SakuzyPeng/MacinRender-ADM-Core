//! Whole binaural renders through the render service (formerly the C++
//! `binaural_render_fixture_test`, R3 C2): interaural differences of an off-centre source, block
//! timing, the LFE's HRTF bypass, the CAF binaural layout tag and the layout rejection, a missing
//! SOFA file, channelLock, divergence (also a divergence block after a point block), extent, the
//! SAF spreader's length compensation, the diffuse bus, and the offline listener orientation with
//! ADM head lock, including a head-locked block after a free one and the spreader. The file stream
//! (pull sizes, seeks, live gains and topology, head tracking) is `mradm-render`'s
//! `binaural_stream.rs`; HRTF lookup, grid identity, the magnitude hull and head shadow are
//! `mradm-dsp`'s `hrtf_kemar.rs`.
mod common;
use common::render::*;
use mradm_engine::backend::BinauralSpreadMode;
use mradm_engine::render::{Request, SofaPath};
use mradm_engine::request::Selection;
use mradm_testkit::adm::*;

const FRAMES: usize = 4096;

fn jump() -> Param {
    jump_position([Param::text(true)])
}

/// A 440 Hz tone of amplitude 0.25 (the C++ fixture signal).
fn tone(frames: usize, hz: f32) -> Vec<f32> {
    (0..frames)
        .map(|i| 0.25 * (2. * std::f32::consts::PI * hz * i as f32 / 48000.).sin())
        .collect()
}

/// An object over `rtime`..`rtime + duration` milliseconds at `az`, with `rest` on its block.
fn object(az: f32, rtime_ms: i64, duration_ms: i64, rest: Vec<Param>) -> Single {
    let mut block = polar(
        az,
        0.,
        [
            gain_linear(1.0),
            rtime(rtime_ms * MS),
            duration(duration_ms * MS),
            jump(),
        ],
    );
    for p in rest {
        block.set(p);
    }
    Single::objects(vec![block])
}

fn write(dir: &Dir, single: Single, samples: &[f32]) -> std::path::PathBuf {
    let path = dir.file("in.wav");
    let (axml, uid) = single.built();
    let mut wave = mradm_testkit::wave::Wave::new(1, 48000, 24);
    wave.chna = Some(vec![mradm_testkit::wave::AudioId::new(1, &uid, "", "")]);
    wave.axml = Some(axml);
    wave.write(&path, samples).unwrap();
    path
}

fn binaural(
    renderer: Selection,
    edit: impl FnOnce(&mut Request),
    single: Single,
    frames: usize,
) -> Audio {
    let dir = scratch("binaural");
    let input = write(&dir, single, &tone(frames, 440.));
    let mut r = request(&input, &dir.file("out.wav"), renderer, "binaural");
    r.peak_limit = false;
    edit(&mut r);
    render(r).1
}

fn saf(single: Single) -> Audio {
    binaural(Selection::SafBinaural, |_| {}, single, FRAMES)
}

fn energy(audio: &Audio, channel: usize, range: std::ops::Range<usize>) -> f64 {
    range
        .map(|f| f64::from(audio.samples[f * 2 + channel]).powi(2))
        .sum()
}

fn ears(audio: &Audio) -> (f64, f64) {
    (
        energy(audio, 0, 0..audio.frames),
        energy(audio, 1, 0..audio.frames),
    )
}

fn difference(a: &Audio, b: &Audio) -> f64 {
    a.samples
        .iter()
        .zip(&b.samples)
        .map(|(x, y)| f64::from(x - y).powi(2))
        .sum()
}

#[test]
fn an_off_centre_source_is_stereo_and_lateralised() {
    let audio = saf(object(90., 0, 80, vec![]));
    assert_eq!(audio.channels, 2);
    let (l, r) = ears(&audio);
    assert!(l + r > 1e-5 && (l - r).abs() > 1e-7, "{l} {r}");
}

#[test]
fn nothing_leaks_before_the_block_starts() {
    let audio = saf(object(0., 10, 30, vec![]));
    assert!(energy(&audio, 0, 0..400) + energy(&audio, 1, 0..400) < 1e-10);
    assert!(energy(&audio, 0, 800..1400) + energy(&audio, 1, 800..1400) > 1e-5);
}

#[test]
fn the_lfe_bypasses_the_hrtf() {
    let mut block = direct_speakers_block([
        spherical([azimuth(45.0f32), elevation(-35.0f32), distance(1.0f32)]),
        gain_linear(1.0),
        rtime(0),
        duration(80 * MS),
    ]);
    block.add(speaker_label("RC_LFE"));
    let dir = scratch("binaural-lfe");
    let input = write(
        &dir,
        Single::new(Type::DirectSpeakers, vec![block]),
        &tone(FRAMES, 80.),
    );
    let mut r = request(
        &input,
        &dir.file("out.wav"),
        Selection::SafBinaural,
        "binaural",
    );
    r.peak_limit = false;
    let (l, r) = ears(&render(r).1);
    assert!(l > 1e-5 && (l - r).abs() < l * 1e-6, "{l} {r}");
}

#[test]
fn caf_outputs_carry_the_binaural_tag_and_other_layouts_are_rejected() {
    let dir = scratch("binaural-caf");
    let input = write(&dir, object(0., 0, 80, vec![]), &tone(FRAMES, 440.));
    let out = dir.file("out.caf");
    let mut r = request(&input, &out, Selection::SafBinaural, "binaural");
    r.peak_limit = false;
    assert!(run(r.clone()).ok());
    let reader = mradm_wav::caf::Reader::open(&out).unwrap();
    assert_eq!((reader.channels(), reader.sample_rate()), (2, 48000));
    assert_eq!(
        reader.layout_tag(),
        Some((106 << 16) | 2),
        "kAudioChannelLayoutTag_Binaural"
    );
    r.output_layout = "wav71".to_owned();
    r.output = Some(text(&dir.file("invalid.caf")));
    let rejected = run(r);
    assert_eq!(rejected.code(), 2);
    assert!(
        rejected
            .outcome
            .error
            .unwrap()
            .message
            .contains("does not support output layout")
    );
}

#[test]
fn a_missing_sofa_file_fails_explicitly() {
    let dir = scratch("binaural-sofa");
    let input = write(&dir, object(0., 0, 80, vec![]), &tone(FRAMES, 440.));
    let mut r = request(
        &input,
        &dir.file("out.wav"),
        Selection::SafBinaural,
        "binaural",
    );
    let missing = dir.file("missing.sofa");
    r.sofa = Some(SofaPath {
        path: text(&missing),
        file_name: b"missing.sofa".to_vec(),
    });
    let code = run(r).code();
    assert!(code == 3 || code == 2, "{code}");
}

fn spatial(rest: Vec<Param>) -> Audio {
    binaural(Selection::Binaural, |_| {}, object(0., 0, 80, rest), FRAMES)
}

#[test]
fn channel_lock_divergence_and_extent_change_the_output() {
    let free = spatial(vec![]);
    let locked = spatial(vec![channel_lock([Param::text(true)])]);
    let (fl, fr) = ears(&free);
    let (ll, lr) = ears(&locked);
    assert!(
        ll + lr > 1e-5 && (ll - lr).abs() > (fl - fr).abs() * 2.,
        "channelLock reaches an off-centre slot"
    );

    let point_energy = fl + fr;
    let diverged = spatial(vec![object_divergence([
        Param::text(1.0f32),
        azimuth_range(60.0f32),
    ])]);
    let (dl, dr) = ears(&diverged);
    assert!(dl + dr > 1e-5 && difference(&free, &diverged) > point_energy * 1e-3);
    assert!(difference(&free, &spatial(vec![width(1.0f32)])) > point_energy * 1e-3);
    assert!(difference(&free, &spatial(vec![height(1.0f32)])) > point_energy * 1e-4);
    assert!(difference(&free, &spatial(vec![depth(1.0f32)])) > point_energy * 1e-4);

    let diffused = spatial(vec![diffuse(1.0f32)]);
    let (xl, xr) = ears(&diffused);
    let (hl, hr) = ears(&spatial(vec![diffuse(0.5f32)]));
    assert!(xl + xr > point_energy * 0.01 && hl + hr > point_energy * 0.01);
    assert!(difference(&free, &diffused) > point_energy * 1e-3);
}

#[test]
fn a_divergence_block_after_a_point_block_keeps_its_slots_balanced() {
    let reference = spatial(vec![object_divergence([
        Param::text(1.0f32),
        azimuth_range(60.0f32),
    ])]);
    let mixed = binaural(
        Selection::Binaural,
        |_| {},
        Single::objects(vec![
            polar(
                0.,
                0.,
                [gain_linear(1.0), rtime(0), duration(40 * MS), jump()],
            ),
            polar(
                0.,
                0.,
                [
                    gain_linear(1.0),
                    rtime(40 * MS),
                    duration(40 * MS),
                    jump_position([Param::text(false)]),
                    object_divergence([Param::text(1.0f32), azimuth_range(60.0f32)]),
                ],
            ),
        ]),
        FRAMES,
    );
    let range = 1920..3840;
    let delta = |a: &Audio| (energy(a, 0, range.clone()) - energy(a, 1, range.clone())).abs();
    let mixed_energy = energy(&mixed, 0, range.clone()) + energy(&mixed, 1, range.clone());
    assert!(mixed_energy > 1e-5);
    assert!(delta(&mixed) < delta(&reference) + mixed_energy * 0.05);
}

#[test]
fn the_saf_spreader_keeps_the_input_length() {
    let spreader = |r: &mut Request| r.binaural_spread = BinauralSpreadMode::SafSpreader as u32;
    let wide = binaural(
        Selection::Binaural,
        spreader,
        object(0., 0, 80, vec![width(1.0f32)]),
        FRAMES,
    );
    assert_eq!((wide.channels, wide.frames), (2, FRAMES));
    let (l, r) = ears(&wide);
    assert!(l + r > 1e-5);
    let cloud = spatial(vec![width(1.0f32)]);
    assert!(difference(&wide, &cloud) > (l + r) * 1e-3);
    let point = binaural(
        Selection::Binaural,
        spreader,
        object(0., 0, 80, vec![]),
        FRAMES,
    );
    assert!(point.frames == FRAMES && point.energy() > 0.);
    let odd = binaural(
        Selection::Binaural,
        spreader,
        object(0., 0, 80, vec![width(1.0f32)]),
        FRAMES + 123,
    );
    assert!(odd.frames == FRAMES + 123 && odd.energy() > 0.);
}

#[test]
fn the_offline_orientation_respects_adm_head_lock() {
    let left = |r: &mut Request| r.listener_orientation = [90., 0., 0.];
    let balanced = |(l, r): (f64, f64)| l > 1e-3 && (l - r).abs() < l * 0.5;
    let turned = binaural(Selection::Binaural, left, object(0., 0, 120, vec![]), 8192);
    let (l, r) = ears(&turned);
    assert!(r > l * 2., "a front source moves to the right ear");

    let mut locked = object(0., 0, 120, vec![]);
    locked.doc.set(locked.object, head_locked(true));
    assert!(balanced(ears(&binaural(
        Selection::Binaural,
        left,
        locked,
        8192
    ))));

    let spreader = |r: &mut Request| {
        r.listener_orientation = [90., 0., 0.];
        r.binaural_spread = BinauralSpreadMode::SafSpreader as u32;
    };
    let mut wide = object(0., 0, 120, vec![width(0.35f32)]);
    wide.doc.set(wide.object, head_locked(true));
    let (l, r) = ears(&binaural(Selection::Binaural, spreader, wide, 8192));
    assert!(l > 1e-3 && (l - r).abs() < l * 0.7);

    // A free block, then a head-locked one.
    let timeline = Single::objects(vec![
        polar(
            0.,
            0.,
            [rtime(0), duration(200 * MS), jump(), head_locked(false)],
        ),
        polar(
            0.,
            0.,
            [
                rtime(200 * MS),
                duration(200 * MS),
                jump(),
                head_locked(true),
            ],
        ),
    ]);
    let audio = binaural(Selection::Binaural, left, timeline, 19200);
    let (first, second) = (2400..7200, 12000..16800);
    assert!(energy(&audio, 1, first.clone()) > energy(&audio, 0, first) * 2.);
    let (sl, sr) = (energy(&audio, 0, second.clone()), energy(&audio, 1, second));
    assert!((sl - sr).abs() < sl * 0.5);
}
