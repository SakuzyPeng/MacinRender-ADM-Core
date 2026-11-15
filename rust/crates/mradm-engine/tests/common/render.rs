//! Whole renders for the tests ported from the C++ SDK render tests (R3 C): ADM WAVE inputs
//! authored with `mradm-testkit`, the offline render service on [`TestHost`] with no islands, and
//! the output read back. Only WAV / CAF outputs are rendered here; an island request fails the
//! render, so nothing in these tests reaches an encoder.
use super::backend::TestHost;
use mradm_dsp::room::screen::RoomScreenGeometry;
use mradm_engine::backend::{BinauralSpreadMode, Screens, SpeakerSpread};
use mradm_engine::render::{Event, Outcome, PathText, Render, Request, Reuse, RoomOptions};
use mradm_engine::request::{BitDepth, DsRouting, Level, Selection};
use mradm_testkit::adm::*;
use mradm_testkit::fixtures::wire_channel;
use mradm_testkit::wave::{AudioId, Wave};
use std::fs::File;
use std::path::{Path, PathBuf};
use std::sync::atomic::{AtomicUsize, Ordering};

pub use super::backend::Dir;

/// A test directory unique to this call (tests run in parallel in one process).
pub fn scratch(tag: &str) -> Dir {
    static NEXT: AtomicUsize = AtomicUsize::new(0);
    Dir::new(&format!("{tag}-{}", NEXT.fetch_add(1, Ordering::Relaxed)))
}

/// A document of one object over one channel, before [`Single::built`] reassigns the identifiers.
pub struct Single {
    pub doc: Document,
    pub object: Id,
    pub channel: Id,
    pub uid: Id,
}

impl Single {
    /// One channel of `ty` with `blocks`, its pack, stream, track, UID, object, content and programme.
    pub fn new(ty: Type, blocks: Vec<Properties>) -> Self {
        let mut single = Self::bare(ty, blocks);
        let object = single.object;
        single.wrap(object);
        single
    }

    /// [`Single::new`] without the content and programme (see [`Single::wrap`]).
    pub fn bare(ty: Type, blocks: Vec<Properties>) -> Self {
        let kind = match ty {
            Type::DirectSpeakers => BlockKind::DirectSpeakers,
            Type::Hoa => BlockKind::Hoa,
            _ => BlockKind::Objects,
        };
        let mut doc = Document::new();
        let channel = doc.channel(ty, [channel_name("TestCF")]);
        for block in blocks {
            doc.block(channel, kind, block);
        }
        doc.add(channel);
        let pack = doc.pack(ty, [pack_name("TestPF")]);
        doc.reference(pack, channel);
        doc.add(pack);
        let uid = wire_channel(&mut doc, channel, pack, "Test");
        let object = doc.object([object_name("TestObject")]);
        doc.reference(object, uid);
        doc.add(object);
        Self {
            doc,
            object,
            channel,
            uid,
        }
    }

    /// A content and programme over the object `top`.
    pub fn wrap(&mut self, top: Id) {
        let content = self.doc.content([content_name("TestContent")]);
        self.doc.reference(content, top);
        self.doc.add(content);
        let programme = self.doc.programme([programme_name("TestProgramme")]);
        self.doc.reference(programme, content);
        self.doc.add(programme);
    }

    /// One Objects channel with `blocks`.
    pub fn objects(blocks: Vec<Properties>) -> Self {
        Self::new(Type::Objects, blocks)
    }

    /// One DirectSpeakers channel: `label`, and a polar position when given.
    pub fn speaker(label: &str, position: Option<(f32, f32)>) -> Self {
        let mut block = direct_speakers_block([]);
        if let Some((az, el)) = position {
            block.set(spherical([azimuth(az), elevation(el), distance(1.0f32)]));
        }
        block.add(speaker_label(label));
        Self::new(Type::DirectSpeakers, vec![block])
    }

    /// The AXML and the UID string.
    pub fn built(mut self) -> (String, String) {
        self.doc.reassign_ids();
        (self.doc.xml(), self.doc.id(self.uid).to_owned())
    }
}

/// Several channels of `(type, blocks)`, each with its own pack and track, under one object
/// (`one_object`) or one object each, in one content and programme: the AXML and the UIDs in
/// track order.
pub fn tracks(specs: Vec<(Type, Vec<Properties>)>, one_object: bool) -> (String, Vec<String>) {
    let mut doc = Document::new();
    let mut uids = Vec::new();
    let mut objects = Vec::new();
    for (i, (ty, blocks)) in specs.into_iter().enumerate() {
        let kind = match ty {
            Type::DirectSpeakers => BlockKind::DirectSpeakers,
            Type::Hoa => BlockKind::Hoa,
            _ => BlockKind::Objects,
        };
        let channel = doc.channel(ty, [channel_name(format!("CF{i}"))]);
        for block in blocks {
            doc.block(channel, kind, block);
        }
        doc.add(channel);
        let pack = doc.pack(ty, [pack_name(format!("PF{i}"))]);
        doc.reference(pack, channel);
        doc.add(pack);
        let uid = wire_channel(&mut doc, channel, pack, &i.to_string());
        uids.push(uid);
        if !one_object || i == 0 {
            let object = doc.object([object_name(format!("Object{i}"))]);
            doc.add(object);
            objects.push(object);
        }
        let object = *objects.last().unwrap();
        doc.reference(object, uid);
    }
    let content = doc.content([content_name("TestContent")]);
    for &o in &objects {
        doc.reference(content, o);
    }
    doc.add(content);
    let programme = doc.programme([programme_name("TestProgramme")]);
    doc.reference(programme, content);
    doc.add(programme);
    doc.reassign_ids();
    let uids = uids.iter().map(|&u| doc.id(u).to_owned()).collect();
    (doc.xml(), uids)
}

/// A DirectSpeakers block for `label` at a polar position.
pub fn speaker_block(label: &str, az: f32, el: f32) -> Properties {
    let mut block =
        direct_speakers_block([spherical([azimuth(az), elevation(el), distance(1.0f32)])]);
    block.add(speaker_label(label));
    block
}

/// An input whose channel `c` holds `values[c]` in every frame.
pub fn multi_input(
    dir: &Dir,
    axml: &str,
    uids: &[String],
    values: &[f32],
    rate: u32,
    frames: usize,
) -> PathBuf {
    let path = dir.file("multi.wav");
    let mut wave = Wave::new(uids.len() as u16, rate, 24);
    wave.chna = Some(
        uids.iter()
            .enumerate()
            .map(|(i, uid)| AudioId::new(i as u16 + 1, uid, "", ""))
            .collect(),
    );
    wave.axml = Some(axml.to_owned());
    let samples: Vec<f32> = (0..frames).flat_map(|_| values.iter().copied()).collect();
    wave.write(&path, &samples).unwrap();
    path
}

/// A polar Objects block at (`az`, `el`).
pub fn polar(az: f32, el: f32, rest: impl IntoIterator<Item = Param>) -> Properties {
    let mut block = objects_block([spherical([azimuth(az), elevation(el)])]);
    for p in rest {
        block.set(p);
    }
    block
}

pub const MS: i64 = 1_000_000;

/// An ADM WAVE input: 24-bit PCM, one CHNA entry per UID, every sample `value`.
pub fn input(path: &Path, axml: &str, uids: &[&str], rate: u32, frames: usize, value: f32) {
    let mut wave = Wave::new(uids.len() as u16, rate, 24);
    wave.chna = Some(
        uids.iter()
            .enumerate()
            .map(|(i, uid)| AudioId::new(i as u16 + 1, uid, "", ""))
            .collect(),
    );
    wave.axml = Some(axml.to_owned());
    wave.write(path, &vec![value; uids.len() * frames]).unwrap();
}

/// A one-object input at `rate` with `frames` frames of 0.5 (the C++ `write_input_fixture`).
pub fn single_input(dir: &Dir, single: Single, rate: u32, frames: usize) -> PathBuf {
    static NEXT: AtomicUsize = AtomicUsize::new(0);
    let path = dir.file(&format!("in-{}.wav", NEXT.fetch_add(1, Ordering::Relaxed)));
    let (axml, uid) = single.built();
    input(&path, &axml, &[&uid], rate, frames, 0.5);
    path
}

pub fn text(path: &Path) -> PathText {
    let utf8 = path.to_string_lossy().into_owned();
    PathText {
        display: utf8.clone(),
        utf8,
    }
}

/// The defaults of the C++ `RenderOptions`, with float32 output so the checks read exact values.
pub fn request(input: &Path, output: &Path, renderer: Selection, layout: &str) -> Request {
    Request {
        input: text(input),
        output: Some(text(output)),
        renderer: renderer as u32,
        output_layout: layout.to_owned(),
        input_layout: None,
        input_channel_labels: Vec::new(),
        measure_loudness: false,
        loudness_target_lufs: -23.,
        peak_limit: true,
        peak_limit_dbtp: -1.,
        peak_normalize_to_limit: false,
        final_gain_db: 0.,
        output_bit_depth: BitDepth::F32 as u32,
        opus_bitrate_per_ch_kbps: 0,
        apac_bitrate_kbps: 0,
        apac_drc_music: true,
        apac_caf: false,
        sofa: None,
        semantic_policy_path: None,
        semantic_policy_json: None,
        matrix_path: None,
        matrix_json: None,
        semantic_report_path: None,
        capture_semantic_report: false,
        default_interp_ms: 5,
        object_smoothing_frames: 0,
        apple_geometry: false,
        routing: DsRouting::Automatic as u32,
        speaker_spread: SpeakerSpread::Automatic as u32,
        triple_balance_mode: 0,
        binaural_spread: BinauralSpreadMode::Automatic as u32,
        split_power: false,
        apple_spatial_preset: 0,
        apple_speaker_rendering_flags: false,
        listener_orientation: [0.; 3],
        render_start_sec: 0.,
        render_end_sec: None,
        room: RoomOptions {
            package: None,
            hrir: Vec::new(),
            metres: 0.,
            quality: 0,
        },
        screens: Screens {
            has_reference: false,
            reference: RoomScreenGeometry::default(),
            has_reproduction: false,
            reproduction: RoomScreenGeometry::default(),
        },
        internal_allow_speaker_stereo: layout == "0+2+0",
        iamf_mp4: false,
        iamf_layers: Vec::new(),
    }
}

/// The platform of WAV / CAF renders: no Apple backend, no user SOFA sets.
pub fn platform() -> mradm_engine::catalog::Platform {
    mradm_engine::catalog::Platform::default()
}

/// A finished render: the service's outcome and the log lines it emitted.
pub struct Run {
    pub outcome: Outcome,
    /// `(level, module, message)`.
    pub logs: Vec<(Level, String, String)>,
}

impl Run {
    pub fn ok(&self) -> bool {
        self.outcome.error.is_none()
    }
    pub fn code(&self) -> i32 {
        self.outcome.error.as_ref().map_or(0, |e| e.code)
    }
    pub fn warned(&self, module: &str, needle: &str) -> bool {
        self.logs
            .iter()
            .any(|(l, m, t)| *l == Level::Warning && m == module && t.contains(needle))
    }
    pub fn warnings(&self, module: &str) -> usize {
        self.logs
            .iter()
            .filter(|(l, m, _)| *l == Level::Warning && m == module)
            .count()
    }
}

/// Runs the render service to the end.
pub fn run(r: Request) -> Run {
    let mut render = Render::new(r, platform(), Reuse::default());
    let mut logs = Vec::new();
    loop {
        match render.next(false, &TestHost) {
            Event::Log(l) => logs.push((l.level, l.module.to_owned(), l.message)),
            Event::Progress(_) | Event::Continue => {}
            Event::Island(i) => panic!("unexpected island {i:?}"),
            Event::Done => break,
        }
    }
    let outcome = render.outcome().unwrap().clone();
    Run { outcome, logs }
}

/// A WAVE file read back.
#[derive(Clone, Debug)]
pub struct Audio {
    pub channels: usize,
    pub rate: u32,
    pub frames: usize,
    pub samples: Vec<f32>,
}

impl Audio {
    pub fn read(path: &Path) -> Self {
        let mut reader =
            mradm_wav::Reader::new(std::io::BufReader::new(File::open(path).unwrap())).unwrap();
        let info = reader.info();
        let channels = usize::from(info.format.channels);
        let mut samples = vec![0f32; info.frames as usize * channels];
        reader.read_frames(&mut samples).unwrap();
        Self {
            channels,
            rate: info.format.sample_rate,
            frames: info.frames as usize,
            samples,
        }
    }
    /// Σ|x| per channel over frames `range`.
    pub fn sums_in(&self, range: std::ops::Range<usize>) -> Vec<f64> {
        let mut sums = vec![0f64; self.channels];
        for frame in range.start..range.end.min(self.frames) {
            for (c, sum) in sums.iter_mut().enumerate() {
                *sum += f64::from(self.samples[frame * self.channels + c].abs());
            }
        }
        sums
    }
    pub fn sums(&self) -> Vec<f64> {
        self.sums_in(0..self.frames)
    }
    pub fn energy_in(&self, range: std::ops::Range<usize>) -> f64 {
        self.sums_in(range).iter().sum()
    }
    pub fn energy(&self) -> f64 {
        self.energy_in(0..self.frames)
    }
}

/// Renders `input` and reads the output, panicking with the service's error on failure.
pub fn render(r: Request) -> (Run, Audio) {
    let output = PathBuf::from(&r.output.as_ref().unwrap().utf8);
    let run = run(r);
    if let Some(e) = &run.outcome.error {
        panic!("render failed: {} ({})", e.message, e.context);
    }
    let audio = Audio::read(&output);
    (run, audio)
}
