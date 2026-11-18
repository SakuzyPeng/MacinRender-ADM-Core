//! 22.2 LFE routing through whole EAR and VBAP renders of labelled channel beds (from the C++
//! `lfe_routing_test`, R3 F2): direct LFE1 / LFE2 on channels 3 / 9 only, split-power at sqrt(0.5)
//! on both, a native dual LFE without crosstalk, split-power refusing both LFEs before any output,
//! non-LFE routing untouched by the mode, and the warning on other layouts.
mod common;
use common::render::*;
use mradm_engine::request::Selection;
use mradm_wav::{Format, SampleFormat, Writer, WriterOptions};
use std::fs::File;
use std::path::PathBuf;

const FRAMES: usize = 4800;
const SILENCE: f64 = 1e-7;

/// One channel of the bed: amplitude and frequency of its sine.
#[derive(Clone, Copy)]
struct Tone(f32, f32);

fn bed(dir: &Dir, tones: &[Tone]) -> PathBuf {
    let path = dir.file("bed.wav");
    let format = Format {
        channels: tones.len() as u16,
        sample_rate: 48000,
        sample_format: SampleFormat::Float32,
        channel_mask: None,
    };
    let mut writer = Writer::new(
        File::create(&path).unwrap(),
        format,
        WriterOptions::default(),
    )
    .unwrap();
    let samples: Vec<f32> = (0..FRAMES)
        .flat_map(|f| {
            tones.iter().map(move |Tone(amplitude, frequency)| {
                amplitude * (std::f32::consts::TAU * frequency * f as f32 / 48000.).sin()
            })
        })
        .collect();
    writer.write_frames(&samples).unwrap();
    writer.finish().unwrap();
    path
}

/// The render of a bed labelled `labels`, with its RMS per output channel when it succeeded.
struct Case {
    run: Run,
    rms: Vec<f64>,
    output_exists: bool,
}

fn render_case(
    renderer: Selection,
    labels: &[&str],
    tones: &[Tone],
    split_power: bool,
    layout: &str,
) -> Case {
    let dir = scratch("lfe");
    let input = bed(&dir, tones);
    let output = dir.file("out.wav");
    let mut r = request(&input, &output, renderer, layout);
    r.peak_limit = false;
    r.input_channel_labels = labels.iter().map(|l| (*l).to_owned()).collect();
    r.split_power = split_power;
    let run = run(r);
    let output_exists = output.exists();
    let rms = if run.ok() {
        let audio = Audio::read(&output);
        (0..audio.channels)
            .map(|c| {
                ((0..audio.frames)
                    .map(|f| f64::from(audio.samples[f * audio.channels + c]).powi(2))
                    .sum::<f64>()
                    / audio.frames as f64)
                    .sqrt()
            })
            .collect()
    } else {
        Vec::new()
    };
    Case {
        run,
        rms,
        output_exists,
    }
}

fn near_ratio(value: f64, reference: f64, ratio: f64) -> bool {
    reference > 1e-8 && (value / reference - ratio).abs() <= 0.025
}

fn backend(renderer: Selection, name: &str) {
    const LFE1: Tone = Tone(0.2, 80.);
    const LFE2: Tone = Tone(0.1, 160.);
    const FULL: Tone = Tone(0.12, 1000.);
    let case = |labels: &[&str], tones: &[Tone], split| {
        render_case(renderer, labels, tones, split, "22.2")
    };
    let direct_lfe1 = case(&["LFE1"], &[LFE1], false);
    let direct_lfe2 = case(&["LFE2"], &[LFE2], false);
    let split_lfe1 = case(&["LFE1"], &[LFE1], true);
    let split_lfe2 = case(&["LFE2"], &[LFE2], true);
    let direct_dual = case(&["LFE1", "LFE2"], &[LFE1, LFE2], false);
    let split_dual = case(&["LFE1", "LFE2"], &[LFE1, LFE2], true);
    let full_direct = case(&["M+000"], &[FULL], false);
    let full_split = case(&["M+000"], &[FULL], true);
    for (label, c) in [
        ("direct LFE1", &direct_lfe1),
        ("direct LFE2", &direct_lfe2),
        ("split LFE1", &split_lfe1),
        ("split LFE2", &split_lfe2),
        ("direct dual", &direct_dual),
        ("full-range direct", &full_direct),
        ("full-range split", &full_split),
    ] {
        assert!(
            c.run.ok() && c.rms.len() == 24,
            "{name}: {label} renders 24 channels"
        );
    }
    let only_lfe_slots = |c: &Case| {
        c.rms
            .iter()
            .enumerate()
            .all(|(ch, v)| ch == 3 || ch == 9 || *v < SILENCE)
    };
    assert!(
        direct_lfe1.rms[3] > 1e-4 && direct_lfe1.rms[9] < SILENCE,
        "{name}: direct LFE1 is unity on ch3 only"
    );
    assert!(
        direct_lfe2.rms[3] < SILENCE && direct_lfe2.rms[9] > 1e-4,
        "{name}: direct LFE2 is unity on ch9 only"
    );
    for c in [
        &direct_lfe1,
        &direct_lfe2,
        &split_lfe1,
        &split_lfe2,
        &direct_dual,
    ] {
        assert!(
            only_lfe_slots(c),
            "{name}: LFE never leaks into a full-range channel"
        );
    }
    let half = std::f64::consts::FRAC_1_SQRT_2;
    assert!(
        near_ratio(split_lfe1.rms[3], direct_lfe1.rms[3], half)
            && near_ratio(split_lfe1.rms[9], direct_lfe1.rms[3], half)
            && (split_lfe1.rms[3] - split_lfe1.rms[9]).abs() < 1e-6,
        "{name}: split LFE1 reaches ch3 and ch9 equally at sqrt(0.5)"
    );
    assert!(
        near_ratio(split_lfe2.rms[3], direct_lfe2.rms[9], half)
            && near_ratio(split_lfe2.rms[9], direct_lfe2.rms[9], half)
            && (split_lfe2.rms[3] - split_lfe2.rms[9]).abs() < 1e-6,
        "{name}: split LFE2 reaches ch3 and ch9 equally at sqrt(0.5)"
    );
    assert!(
        near_ratio(direct_dual.rms[3], direct_lfe1.rms[3], 1.)
            && near_ratio(direct_dual.rms[9], direct_lfe2.rms[9], 1.),
        "{name}: a native dual LFE stays independent"
    );
    let error = split_dual.run.outcome.error.as_ref().unwrap();
    assert!(
        error.code == 1
            && error.message.contains("both LFE1 and LFE2")
            && !split_dual.output_exists,
        "{name}: split-power refuses both LFEs before writing an output"
    );
    let energy: f64 = full_direct.rms.iter().map(|v| v * v).sum();
    let delta = full_direct
        .rms
        .iter()
        .zip(&full_split.rms)
        .map(|(a, b)| (a - b).abs())
        .fold(0., f64::max);
    assert!(
        energy > 1e-8 && delta < 1e-7,
        "{name}: split-power leaves non-LFE routing unchanged"
    );
}

#[test]
fn ear_routes_the_22_2_lfe_pair() {
    backend(Selection::Ear, "EAR");
}

#[test]
fn vbap_routes_the_22_2_lfe_pair() {
    backend(Selection::Saf, "VBAP");
}

#[test]
fn split_power_on_another_layout_renders_and_warns() {
    let case = render_case(Selection::Ear, &["LFE1"], &[Tone(0.2, 80.)], true, "5.1");
    assert!(case.run.ok() && case.rms.len() == 6);
    assert!(
        case.run.warned("ear", "only applies to 22.2"),
        "the mode is ignored with a warning"
    );
}
