//! The built `mradm` executable, run as a user would (formerly `tests/unit/adm_cli_smoke_test.cpp`):
//! discovery output, help, option parsing and its exit codes, DirectSpeakers matrices, renders to
//! WAV and FLAC, Apple speaker geometry, inspect, semantic policy templates, export and room
//! inspection. ctest's `mr_adm_cli_smoke_tests` runs it with `MRADM_EXE_PATH` naming the executable;
//! without it (a plain `cargo test`) every test returns at once.
use mradm_testkit::adm::*;
use mradm_testkit::fixtures::wire_channel;
use mradm_testkit::wave::{AudioId, Wave};
use std::path::{Path, PathBuf};
use std::process::Command;
use std::sync::atomic::{AtomicUsize, Ordering};

fn exe() -> Option<PathBuf> {
    std::env::var_os("MRADM_EXE_PATH")
        .filter(|p| !p.is_empty())
        .map(PathBuf::from)
}

fn run_with_streams(exe: &Path, args: &[&str]) -> (i32, String, String) {
    let output = Command::new(exe).args(args).output().unwrap();
    (
        output.status.code().unwrap_or(-1),
        String::from_utf8_lossy(&output.stdout).into_owned(),
        String::from_utf8_lossy(&output.stderr).into_owned(),
    )
}

/// Combined output for checks that only match a diagnostic or a progress line.
fn run(exe: &Path, args: &[&str]) -> (i32, String) {
    let (code, mut stdout, stderr) = run_with_streams(exe, args);
    stdout.push_str(&stderr);
    (code, stdout)
}

fn success_stdout(exe: &Path, args: &[&str]) -> String {
    let (code, stdout, stderr) = run_with_streams(exe, args);
    assert_eq!(code, 0, "{args:?}:\n{stdout}{stderr}");
    assert!(stderr.is_empty(), "{args:?} stderr:\n{stderr}");
    stdout
}

fn assert_parse_error(exe: &Path, args: &[&str], expected: i32) {
    let (code, stdout, stderr) = run_with_streams(exe, args);
    assert_eq!(code, expected, "{args:?}:\n{stdout}{stderr}");
    assert!(stdout.is_empty(), "{args:?} stdout:\n{stdout}");
    assert!(
        !stderr.is_empty(),
        "{args:?} must report the error on stderr"
    );
}

/// A directory unique to this call, removed with its contents on drop.
struct Dir(PathBuf);

impl Dir {
    fn new(tag: &str) -> Self {
        static NEXT: AtomicUsize = AtomicUsize::new(0);
        let path = std::env::temp_dir().join(format!(
            "mradm-cli-{tag}-{}-{}",
            std::process::id(),
            NEXT.fetch_add(1, Ordering::Relaxed)
        ));
        let _ = std::fs::remove_dir_all(&path);
        std::fs::create_dir_all(&path).unwrap();
        Self(path)
    }

    fn file(&self, name: &str) -> String {
        self.0.join(name).to_str().unwrap().to_owned()
    }
}

impl Drop for Dir {
    fn drop(&mut self) {
        let _ = std::fs::remove_dir_all(&self.0);
    }
}

/// One channel of `ty` with `block` under `prefix`Object / Content / Programme, `frames` of `value`.
#[allow(clippy::too_many_arguments)]
fn write_input(
    path: &str,
    ty: Type,
    kind: BlockKind,
    block: Properties,
    prefix: &str,
    names: (&str, &str, &str),
    frames: usize,
    value: f32,
) {
    let mut doc = Document::new();
    let channel = doc.channel(ty, [channel_name(format!("{prefix}CF"))]);
    doc.block(channel, kind, block);
    doc.add(channel);
    let pack = doc.pack(ty, [pack_name(format!("{prefix}PF"))]);
    doc.reference(pack, channel);
    doc.add(pack);
    let uid = wire_channel(&mut doc, channel, pack, prefix);
    let object = doc.object([object_name(names.0)]);
    doc.reference(object, uid);
    doc.add(object);
    let content = doc.content([content_name(names.1)]);
    doc.reference(content, object);
    doc.add(content);
    let programme = doc.programme([programme_name(names.2)]);
    doc.reference(programme, content);
    doc.add(programme);
    doc.reassign_ids();
    let mut wave = Wave::new(1, 48000, 24);
    wave.chna = Some(vec![AudioId::new(1, doc.id(uid), "", "")]);
    wave.axml = Some(doc.xml());
    wave.write(Path::new(path), &vec![value; frames]).unwrap();
}

/// One Object straight ahead, 16 silent frames (the C++ `write_fixture`).
fn object_input(dir: &Dir) -> String {
    let path = dir.file("object.wav");
    let block = objects_block([spherical([azimuth(0.0f32), elevation(0.0f32)])]);
    let names = ("CliTestObj", "CliTestContent", "CliTestProg");
    write_input(
        &path,
        Type::Objects,
        BlockKind::Objects,
        block,
        "",
        names,
        16,
        0.0,
    );
    path
}

/// One DirectSpeakers channel M+000, 4096 frames of 0.25, and a 5.1 matrix routing C to L and R.
fn matrix_inputs(dir: &Dir) -> (String, String) {
    let path = dir.file("direct-speakers.wav");
    let mut block = direct_speakers_block([spherical([
        azimuth(0.0f32),
        elevation(0.0f32),
        distance(1.0f32),
    ])]);
    block.add(speaker_label("M+000"));
    let names = ("CliMatrixObject", "CliMatrixContent", "CliMatrixProgramme");
    write_input(
        &path,
        Type::DirectSpeakers,
        BlockKind::DirectSpeakers,
        block,
        "CliMatrix",
        names,
        4096,
        0.25,
    );
    let matrix = dir.file("matrix.json");
    std::fs::write(
        &matrix,
        r#"{"schema":"mradm.direct-speakers-matrix.v1","output_layout":"5.1","routes":[{"source_label":"C","targets":[{"label":"L","weight":1},{"label":"R","weight":1}]}]}"#,
    )
    .unwrap();
    (path, matrix)
}

fn has_all(text: &str, needles: &[&str]) {
    for needle in needles {
        assert!(text.contains(needle), "missing {needle:?} in:\n{text}");
    }
}

#[test]
fn discovery_lists_backends_formats_layouts_and_input_layouts() {
    let Some(exe) = exe() else { return };
    let out = success_stdout(&exe, &["backends"]);
    has_all(
        &out,
        &[
            "Backend: triple-balance",
            "libear",
            "saf-binaural-hrtf",
            "binaural",
            "7.1",
            "ChannelLock:    yes",
            "Divergence:     yes",
            "ScreenRef:      no",
            "Diffuse:        yes",
            "HRTF sources:   built-in",
        ],
    );
    for absent in ["stereo", "0+7+0", "4+7+0"] {
        assert!(!out.contains(absent), "{absent}");
    }
    let out = success_stdout(&exe, &["formats"]);
    has_all(
        &out,
        &[
            "Build features:",
            "wav",
            "flac",
            "opus_mka",
            "apac",
            "iamf",
            "iamf_mp4",
            "channels: up to 8",
            "bitrate (per channel): 6-320 kbps",
        ],
    );
    assert_parse_error(&exe, &["formats", "extra"], 109);

    assert_ne!(run(&exe, &["layouts"]).0, 0, "layouts needs --format");
    let out = success_stdout(&exe, &["layouts", "--format", "apac", "--layout", "7.1"]);
    has_all(&out, &["AudioUnit_7_1", "L R C LFE Ls Rs Rls Rrs"]);
    let out = success_stdout(&exe, &["layouts", "--format", "wav", "--layout", "wav71"]);
    has_all(&out, &["L R C LFE Rls Rrs Ls Rs"]);
    let out = success_stdout(&exe, &["layouts", "--format", "flac", "--renderer", "saf"]);
    assert!(out.contains("Renderer: saf-vbap") && !out.contains("7.1.4"));
    let out = success_stdout(&exe, &["layouts", "--format", "flac", "--renderer", "ear"]);
    assert!(!out.contains("5.1.2"));
    let out = success_stdout(&exe, &["layouts", "--format", "wav", "--renderer", "ear"]);
    has_all(&out, &["Renderer: libear", "9.1.4", "9.1.6"]);
    let (code, out) = run(
        &exe,
        &[
            "layouts",
            "--format",
            "apac",
            "--renderer",
            "saf",
            "--layout",
            "5.1.2",
        ],
    );
    assert!(code != 0 && out.contains("not supported"));
    let out = success_stdout(
        &exe,
        &["layouts", "--format", "wav", "--renderer", "triple-balance"],
    );
    has_all(&out, &["triple-balance", "7.1.4", "9.1.6", "22.2"]);
    let layout_51 = [
        "layouts",
        "--format",
        "wav",
        "--renderer",
        "triple-balance",
        "--layout",
        "5.1",
    ];
    assert_ne!(
        run(&exe, &layout_51).0,
        0,
        "Triple Balance does not inherit SAF's 5.1"
    );

    let out = success_stdout(&exe, &["input-layouts"]);
    has_all(
        &out,
        &[
            "azimuth +left / -right",
            "L/FL=M+030 (+30",
            "U+110/U-110 require @30 or @45",
            "file order: M+030 M-030 M+000 LFE1 M+135 M-135 M+090 M-090",
            "22.2 (24 channels",
        ],
    );
    let out = success_stdout(&exe, &["input-layouts", "--format", "json"]);
    has_all(
        &out,
        &[
            "\"schema\": \"mradm.input-layouts\"",
            "\"U+110@45\"",
            "\"wave_mask_channel_order\"",
        ],
    );
    assert_ne!(
        run(&exe, &["input-layouts", "--layout", "unknown_xyz"]).0,
        0
    );
}

#[test]
fn render_help_documents_every_option() {
    let Some(exe) = exe() else { return };
    let (code, out) = run(&exe, &["render", "--help"]);
    assert_eq!(code, 0);
    has_all(
        &out,
        &[
            "--triple-balance-mode",
            "--peak-normalize-to-limit",
            "--final-gain-db",
            "--semantic-policy",
            "--write-semantic-report",
            "--speaker-spread-mode",
            "--speaker-geometry",
            "--direct-speakers-routing",
            "--direct-speakers-matrix",
            "spatialize label/nominal direction",
            "--binaural-spread-mode",
            "--lfe-routing",
            "saf-binaural",
            "--apple-spatial-preset",
            "--apple-speaker-rendering-flags",
            "--listener-yaw",
            "--listener-pitch",
            "--listener-roll",
            "--iamf-layers",
            "--input-layout",
            "--input-channels",
            "L/FL=+30deg left",
            "binaural (default)",
            "WAVE mask or ADM AXML/CHNA",
            "PCM BW64 ADM",
        ],
    );
}

/// Each enumerated option takes its documented values (the request reaches the engine) and rejects
/// others with CLI11's validation exit code 105, naming the option; the listener angles take their
/// range ends and reject one past them. (`--help` stops before validation, so it cannot show this.)
#[test]
fn render_options_accept_their_values_and_reject_others() {
    let Some(exe) = exe() else { return };
    let missing = "/nonexistent-mradm-cli-option.wav";
    let accepted = |option: &str, value: &str| {
        let (code, out) = run(&exe, &["render", "-i", missing, option, value]);
        assert!(
            code == 1 && out.contains("render request:"),
            "{option} {value}:\n{out}"
        );
    };
    let rejected = |option: &str, value: &str| {
        let (code, out) = run(&exe, &["render", "-i", missing, option, value]);
        assert!(
            code == 105 && out.contains(&format!("{option}:")),
            "{option} {value}:\n{out}"
        );
    };
    let enumerated: [(&str, &[&str]); 6] = [
        ("--speaker-spread-mode", &["auto", "none", "mdap"]),
        ("--speaker-geometry", &["standard", "apple"]),
        (
            "--direct-speakers-routing",
            &["auto", "label", "position", "matrix"],
        ),
        (
            "--binaural-spread-mode",
            &["auto", "none", "cloud", "saf-spreader"],
        ),
        ("--lfe-routing", &["direct", "split-power"]),
        (
            "--apple-spatial-preset",
            &["off", "headphone-default", "headphone-movie"],
        ),
    ];
    for (option, values) in enumerated {
        for value in values {
            accepted(option, value);
        }
        rejected(option, "invalid_xyz");
    }
    for (option, limit) in [
        ("--listener-yaw", 180),
        ("--listener-roll", 180),
        ("--listener-pitch", 90),
    ] {
        for value in [-limit, limit] {
            accepted(option, &value.to_string());
        }
        for value in [-limit - 1, limit + 1] {
            rejected(option, &value.to_string());
        }
    }
    accepted("--iamf-layers", "5.1,5.1.2,5.1.4,7.1.4");
}

/// Triple Balance D needs its renderer; renderer names reach input validation, removed names and
/// aliases are rejected by the parser.
#[test]
fn renderer_names_and_triple_balance_modes_are_checked() {
    let Some(exe) = exe() else { return };
    let missing = "/nonexistent-triple-balance.wav";
    let (code, out) = run(
        &exe,
        &[
            "render",
            "--renderer",
            "saf",
            "--triple-balance-mode",
            "d",
            "-i",
            "missing.wav",
        ],
    );
    assert!(code != 0 && out.contains("D mode requires"), "{out}");
    assert_ne!(
        run(
            &exe,
            &[
                "render",
                "--triple-balance-mode",
                "unknown",
                "-i",
                "missing.wav"
            ]
        )
        .0,
        0
    );
    let args = [
        "render",
        "-i",
        missing,
        "--renderer",
        "triple-balance",
        "--output-layout",
        "7.1.4",
    ];
    let (code, out) = run(&exe, &args);
    assert!(
        code != 0 && !out.contains("expected one of") && !out.contains("not expected"),
        "{out}"
    );
    let args = [
        "render",
        "-i",
        missing,
        "--renderer",
        "saf",
        "--speaker-panner",
        "room-compat",
    ];
    let (code, out) = run(&exe, &args);
    assert!(code != 0 && out.contains("not expected"), "{out}");
    let (code, out) = run(
        &exe,
        &["render", "-i", missing, "--renderer", "room-compat"],
    );
    assert!(code != 0 && out.contains("expected one of"), "{out}");
}

#[test]
fn direct_speaker_matrices_render_and_are_validated() {
    let Some(exe) = exe() else { return };
    let dir = Dir::new("matrix");
    let (input, matrix) = matrix_inputs(&dir);
    let out = dir.file("out.wav");
    let render = |extra: &[&str]| {
        let mut args = vec!["render"];
        args.extend_from_slice(extra);
        args.extend_from_slice(&["--no-peak-limit", "-o", &out, "-i", &input]);
        run(&exe, &args).0
    };
    let matrix_mode = [
        "--direct-speakers-routing",
        "matrix",
        "--direct-speakers-matrix",
        &matrix,
    ];
    for renderer in ["saf", "ear"] {
        let mut args = vec!["--renderer", renderer, "--output-layout", "5.1"];
        args.extend_from_slice(&matrix_mode);
        assert_eq!(render(&args), 0, "{renderer}");
    }
    let mut automatic = vec!["--output-layout", "5.1"];
    automatic.extend_from_slice(&matrix_mode);
    assert_eq!(
        render(&automatic),
        0,
        "the automatic renderer resolves to EAR"
    );
    let no_profile = [
        "--renderer",
        "saf",
        "--output-layout",
        "5.1",
        "--direct-speakers-routing",
        "matrix",
    ];
    assert_ne!(render(&no_profile), 0);
    let stray = [
        "--renderer",
        "saf",
        "--output-layout",
        "5.1",
        "--direct-speakers-matrix",
        &matrix,
    ];
    assert_ne!(render(&stray), 0);
    let mut binaural = vec!["--renderer", "saf-binaural", "--output-layout", "binaural"];
    binaural.extend_from_slice(&matrix_mode);
    assert_ne!(render(&binaural), 0);
}

/// Renders through the real islands: FLAC with no speaker or binaural spread, the default binaural
/// output, a 7.1.4 alias, HOA with loudness normalisation, and the rejected speaker stereo.
#[test]
fn renders_write_wav_and_flac() {
    let Some(exe) = exe() else { return };
    let dir = Dir::new("render");
    let input = object_input(&dir);
    let flac = dir.file("out.flac");
    let wav = dir.file("out.wav");
    let ok = |args: &[&str]| {
        let (code, out) = run(&exe, args);
        assert_eq!(code, 0, "{args:?}:\n{out}");
        out
    };
    ok(&[
        "render",
        "--renderer",
        "saf",
        "--output-layout",
        "5.1",
        "--speaker-spread-mode",
        "none",
        "-o",
        &flac,
        "-i",
        &input,
    ]);
    ok(&[
        "render",
        "--renderer",
        "saf-binaural",
        "--binaural-spread-mode",
        "none",
        "-o",
        &flac,
        "-i",
        &input,
    ]);
    ok(&["render", "-o", &wav, "-i", &input]);
    ok(&[
        "render",
        "--renderer",
        "saf",
        "--output-layout",
        "7.1.4",
        "-o",
        &wav,
        "-i",
        &input,
    ]);
    let out = ok(&[
        "render",
        "--renderer",
        "hoa",
        "--output-layout",
        "hoa3",
        "--loudness-target",
        "-23",
        "-o",
        &wav,
        "-i",
        &input,
    ]);
    assert!(!out.contains("loudness normalization skipped"));
    let stereo = [
        "render",
        "--renderer",
        "saf",
        "--output-layout",
        "stereo",
        "-o",
        &wav,
        "-i",
        &input,
    ];
    assert_ne!(run(&exe, &stereo).0, 0);
    let (code, out) = run(
        &exe,
        &[
            "render",
            "--renderer",
            "binaural",
            "--binaural-spread-mode",
            "none",
            "-o",
            &flac,
            "-i",
            &input,
        ],
    );
    assert!(
        code != 0 && out.contains("expected one of"),
        "binaural is not a backend name"
    );
    let (code, formats) = run(&exe, &["formats"]);
    assert_eq!(code, 0, "{formats}");
    let expected_error = if formats.contains("sofa=yes") {
        "SOFA file could not be opened"
    } else {
        assert!(formats.contains("sofa=no"), "{formats}");
        "does not support user SOFA HRTFs"
    };
    let missing_sofa = dir.file("missing.sofa");
    let (code, out) = run(
        &exe,
        &[
            "render",
            "--renderer",
            "saf-binaural",
            "--sofa",
            &missing_sofa,
            "-o",
            &wav,
            "-i",
            &input,
        ],
    );
    assert_eq!(code, 1, "{out}");
    assert!(out.contains(expected_error), "{out}");
    let (code, out) = run(
        &exe,
        &[
            "render",
            "--input-channels",
            "L,,R",
            "--no-peak-limit",
            "-o",
            &wav,
            "-i",
            &input,
        ],
    );
    assert!(
        code != 0 && out.contains("empty label"),
        "the CSV keeps an empty item:\n{out}"
    );
}

/// Both software speaker renderers take every embedded CoreAudio geometry and log it; 9.1.4 has
/// none and is rejected rather than falling back to the standard geometry.
#[test]
fn apple_speaker_geometry_renders_or_is_rejected() {
    let Some(exe) = exe() else { return };
    let dir = Dir::new("geometry");
    let input = object_input(&dir);
    let out = dir.file("out.wav");
    for renderer in ["saf", "ear"] {
        for layout in [
            "5.1", "7.1", "5.1.2", "5.1.4", "7.1.4", "9.1.6", "22.2", "9.1.4",
        ] {
            let args = [
                "render",
                "--renderer",
                renderer,
                "--output-layout",
                layout,
                "--speaker-geometry",
                "apple",
                "--no-peak-limit",
                "-o",
                &out,
                "-i",
                &input,
            ];
            let (code, text) = run(&exe, &args);
            if layout == "9.1.4" {
                assert!(
                    code != 0 && (text.contains("unsupported") || text.contains("unavailable")),
                    "{text}"
                );
            } else {
                assert!(
                    code == 0 && text.contains("speaker geometry: apple"),
                    "{renderer} {layout}:\n{text}"
                );
            }
        }
    }
}

#[test]
fn inspect_reads_the_scene_and_writes_a_policy_template() {
    let Some(exe) = exe() else { return };
    let dir = Dir::new("inspect");
    let input = object_input(&dir);
    let out = success_stdout(&exe, &["inspect", &input]);
    has_all(&out, &["48000", "Objects", "CliTestObj"]);
    let out = success_stdout(&exe, &["inspect", "--xml", &input]);
    has_all(&out, &["<?xml", "audioObject", "CliTestObj"]);
    assert_ne!(
        run(&exe, &["inspect", "/nonexistent_mr_cli_test_xyz.wav"]).0,
        0
    );
    let template = dir.file("template.json");
    assert_eq!(
        run(
            &exe,
            &[
                "inspect",
                &input,
                "--write-semantic-policy-template",
                &template
            ]
        )
        .0,
        0
    );
    let json = std::fs::read_to_string(&template).unwrap();
    has_all(
        &json,
        &[
            "\"schema\": \"mradm.semantic-policy.v1\"",
            "\"global\"",
            "\"CliTestObj\"",
            "\"diffuse\"",
        ],
    );
}

#[test]
fn export_applies_the_policy_and_reports_failures() {
    let Some(exe) = exe() else { return };
    let dir = Dir::new("export");
    let input = object_input(&dir);
    let output = dir.file("export.wav");
    let policy = dir.file("policy.json");
    std::fs::write(
        &policy,
        r#"{"schema":"mradm.semantic-policy.v1","global":{"gain":{"scale":0.5}}}"#,
    )
    .unwrap();
    let out = success_stdout(
        &exe,
        &[
            "export",
            "-i",
            &input,
            "-o",
            &output,
            "--semantic-policy",
            &policy,
        ],
    );
    assert_eq!(out.trim_end(), format!("[info] wrote {output}"));
    let axml = success_stdout(&exe, &["inspect", "--xml", &output]);
    has_all(&axml, &["CliTestObj", ">0.5</gain>"]);

    let missing = dir.file("missing.wav");
    let failed_output = dir.file("failed.wav");
    let (code, stdout, stderr) =
        run_with_streams(&exe, &["export", "-i", &missing, "-o", &failed_output]);
    assert_eq!(code, 1, "{stdout}{stderr}");
    assert!(stdout.starts_with("[error] "), "{stdout}");
    assert!(stderr.is_empty(), "{stderr}");
    assert!(!Path::new(&failed_output).exists());
    assert_parse_error(&exe, &["export", "-i", &input], 106);
}

#[test]
fn room_check_prints_a_json_report_and_reports_failures() {
    let Some(exe) = exe() else { return };
    let manifest = concat!(
        env!("CARGO_MANIFEST_DIR"),
        "/../../../tests/fixtures/room/synthetic/room.json"
    );
    for (args, quality, policy) in [
        (&["room-check", manifest][..], "exact", "room-exact-v1"),
        (
            &["room-check", manifest, "--quality", "realtime"][..],
            "realtime",
            "room-realtime-v2",
        ),
    ] {
        let stdout = success_stdout(&exe, args);
        let report = mradm_json::parse(stdout.as_bytes()).unwrap();
        let report = report.as_object().unwrap();
        assert_eq!(report["schema"].as_str(), Some("mradm.room-prepared"));
        assert_eq!(report["id"].as_str(), Some("synthetic"));
        assert_eq!(report["quality"].as_str(), Some(quality));
        assert_eq!(report["processing_policy"].as_str(), Some(policy));
    }

    let dir = Dir::new("room-check");
    let missing = dir.file("missing.json");
    let (code, stdout, stderr) = run_with_streams(&exe, &["room-check", &missing]);
    assert_eq!(code, 1, "{stdout}{stderr}");
    assert_eq!(stdout.trim_end(), "[error] Cannot inspect room resource");
    assert!(stderr.is_empty(), "{stderr}");
    assert_parse_error(&exe, &["room-check", manifest, "--quality", "fast"], 105);
}
