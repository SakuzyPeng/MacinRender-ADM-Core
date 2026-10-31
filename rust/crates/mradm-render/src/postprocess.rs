//! Post-processing passes over a rendered file (PCM consistency 4.8), moved from the C++
//! `audio_handles.cpp`, `wav_io.cpp` and `wav_output.cpp`: gain rewrite, trim, integer PCM
//! conversion, WAV layout finalization and AXML replacement, with their temporary files and
//! installation. Each pass is stepped; the caller checks cancellation between steps and reports
//! the progress each step returns. Messages and contexts are the established texts.
use crate::output;
use mradm_wav::{ChnaTrack, LayoutOptions, LayoutRewriter, caf};
use std::{
    collections::hash_map::RandomState,
    fs::{File, OpenOptions},
    hash::{BuildHasher, Hasher},
    io::{BufReader, BufWriter, Write},
    path::{Path, PathBuf},
};

/// An error with its code, message and location context.
#[derive(Clone, Debug, PartialEq, Eq)]
pub struct Failure {
    pub code: i32,
    pub message: String,
    pub context: String,
}
pub type Result<T> = std::result::Result<T, Failure>;
fn fail(code: i32, message: impl Into<String>, context: impl Into<String>) -> Failure {
    Failure {
        code,
        message: message.into(),
        context: context.into(),
    }
}
fn at(path: &str) -> String {
    format!("path={path}")
}
/// A file-format error located at `path`.
fn wav(error: mradm_wav::Error, path: &str) -> Failure {
    fail(error.code, error.message, at(path))
}
/// The operating system's description, as C++ `std::error_code::message` gives it.
fn os(error: &std::io::Error) -> String {
    let text = error.to_string();
    match error.raw_os_error() {
        Some(code) => text
            .strip_suffix(&format!(" (os error {code})"))
            .map_or(text.clone(), str::to_owned),
        None => text,
    }
}

const IO: i32 = 3;

/// `std::filesystem` stem and extension: a leading dot does not start an extension.
fn stem_and_extension(path: &Path) -> (String, String) {
    let name = path
        .file_name()
        .map(|n| n.to_string_lossy().into_owned())
        .unwrap_or_default();
    match name.rfind('.') {
        Some(dot) if dot > 0 && name != ".." => (name[..dot].to_owned(), name[dot..].to_owned()),
        _ => (name, String::new()),
    }
}
fn sidecar(original: &Path, purpose: &str, token: &str) -> PathBuf {
    let (stem, extension) = stem_and_extension(original);
    original
        .parent()
        .unwrap_or(Path::new(""))
        .join(format!("{stem}.{purpose}.{token}{extension}"))
}
/// `{stem}.{purpose}.{16 hex}{ext}` that does not exist yet (16 attempts, then any).
fn random_sidecar(original: &Path, purpose: &str) -> PathBuf {
    let token = || format!("{:016x}", RandomState::new().build_hasher().finish());
    for _ in 0..16 {
        let candidate = sidecar(original, purpose, &token());
        if !candidate.exists() {
            return candidate;
        }
    }
    sidecar(original, purpose, &token())
}
/// `{stem}.{purpose}.NNNN{ext}`, the first of 0..1024 known not to exist.
fn indexed_sidecar(original: &Path, purpose: &str) -> Option<PathBuf> {
    (0..1024u32)
        .map(|index| sidecar(original, purpose, &format!("{index:04}")))
        .find(|candidate| matches!(candidate.try_exists(), Ok(false)))
}
fn text(path: &Path) -> String {
    path.to_string_lossy().into_owned()
}

/// Removes its path on drop unless dismissed; `armed` false until the file is known to be ours.
struct Temporary {
    path: PathBuf,
    armed: bool,
}
impl Drop for Temporary {
    fn drop(&mut self) {
        if self.armed {
            let _ = std::fs::remove_file(&self.path);
        }
    }
}

/// Renames a finished rewrite over the original; where the rename fails (Windows cannot rename
/// over an open or existing file), removes the original and renames again.
fn install_replace(temporary: &mut Temporary, original: &str, operation: &str) -> Result<()> {
    if std::fs::rename(&temporary.path, original).is_err() {
        let _ = std::fs::remove_file(original);
        if let Err(error) = std::fs::rename(&temporary.path, original) {
            return Err(fail(
                IO,
                format!("failed to replace output after {operation}: {}", os(&error)),
                at(original),
            ));
        }
    }
    temporary.armed = false;
    Ok(())
}
/// Moves the original to a backup, installs the rewrite and restores the original if that fails.
fn install_with_backup(original: &str, rewritten: &mut Temporary) -> Result<()> {
    let Some(backup) = indexed_sidecar(Path::new(original), "layout_backup") else {
        return Err(fail(
            IO,
            "cannot allocate WAV layout backup path",
            at(original),
        ));
    };
    let mut backup = Temporary {
        path: backup,
        armed: true,
    };
    if let Err(error) = std::fs::rename(original, &backup.path) {
        return Err(fail(
            IO,
            format!(
                "cannot move original WAV before layout replacement: {}",
                os(&error)
            ),
            at(original),
        ));
    }
    if let Err(error) = std::fs::rename(&rewritten.path, original) {
        if std::fs::rename(&backup.path, original).is_err() {
            backup.armed = false;
            return Err(fail(
                IO,
                format!(
                    "cannot install layout-finalized WAV and could not restore original: {}; original preserved at {}",
                    os(&error),
                    text(&backup.path)
                ),
                at(original),
            ));
        }
        return Err(fail(
            IO,
            format!("cannot install layout-finalized WAV: {}", os(&error)),
            at(original),
        ));
    }
    rewritten.armed = false;
    Ok(())
}

/// What the caller reports after a call: `emit` asks for a progress event.
#[derive(Clone, Copy, Debug, PartialEq)]
pub struct Progress {
    pub emit: bool,
    pub fraction: f64,
    pub done: u64,
    pub total: u64,
    pub message: &'static str,
}
const fn quiet() -> Progress {
    Progress {
        emit: false,
        fraction: 0.0,
        done: 0,
        total: 0,
        message: "",
    }
}
const fn event(fraction: f64, done: u64, total: u64, message: &'static str) -> Progress {
    Progress {
        emit: true,
        fraction,
        done,
        total,
        message,
    }
}
fn ratio(done: u64, total: u64) -> f64 {
    done as f64 / total.max(1) as f64
}

/// A step result. Once `complete`, the data is written and `finish` installs the output.
#[derive(Clone, Copy, Debug, PartialEq)]
pub struct Step {
    pub progress: Progress,
    pub complete: bool,
}
const fn more(progress: Progress) -> Step {
    Step {
        progress,
        complete: false,
    }
}
/// A pass that `begin` either finished at once (skipped) or started.
pub enum Begin {
    Skipped(Progress),
    Started(Box<Pass>, Progress),
}

enum Source {
    Wav(mradm_wav::Reader<BufReader<File>>),
    Caf(caf::Reader<BufReader<File>>),
}
impl Source {
    fn open(path: &str, operation: &str) -> Result<Self> {
        let extension = Path::new(path)
            .extension()
            .map(|e| e.to_string_lossy().to_ascii_lowercase());
        match extension.as_deref() {
            Some("caf") => caf::Reader::open(Path::new(path))
                .map(Source::Caf)
                .map_err(|e| wav(e, path)),
            Some("flac") => Err(fail(
                2,
                format!("{operation} does not support FLAC input"),
                at(path),
            )),
            _ => wave(path).map(Source::Wav),
        }
    }
    fn channels(&self) -> u32 {
        match self {
            Source::Wav(r) => u32::from(r.info().format.channels),
            Source::Caf(r) => r.channels(),
        }
    }
    fn rate(&self) -> u32 {
        match self {
            Source::Wav(r) => r.info().format.sample_rate,
            Source::Caf(r) => r.sample_rate(),
        }
    }
    fn frames(&self) -> u64 {
        match self {
            Source::Wav(r) => r.info().frames,
            Source::Caf(r) => r.frame_count(),
        }
    }
    /// Frames read; 0 at the end or on a read error, as the C++ readers returned.
    fn read(&mut self, out: &mut [f32]) -> u64 {
        match self {
            Source::Wav(r) => r.read_frames(out),
            Source::Caf(r) => r.read(out),
        }
        .unwrap_or(0)
    }
    /// The CAF layout to write a rewrite with: the source's own tag when it is one of ours.
    fn layout(&self, fallback: &[u8]) -> Vec<u8> {
        match self {
            Source::Caf(r) => r
                .layout_tag()
                .and_then(|tag| caf::tag_layout(tag, r.channels()))
                .map_or(fallback.to_vec(), |id| id.as_bytes().to_vec()),
            Source::Wav(_) => fallback.to_vec(),
        }
    }
}
fn wave(path: &str) -> Result<mradm_wav::Reader<BufReader<File>>> {
    let file = File::open(path).map_err(|e| wav(e.into(), path))?;
    mradm_wav::Reader::new(BufReader::new(file)).map_err(|e| wav(e, path))
}
fn is_caf(path: &Path) -> bool {
    path.extension()
        .is_some_and(|e| e.to_string_lossy().eq_ignore_ascii_case("caf"))
}

const BLOCK: u64 = 4096;

/// A started pass. Drop removes the temporary file it owns.
pub struct Pass {
    kind: Kind,
    path: String,
    // Declared last: dropped after the reader and writer close their files.
    temporary: Temporary,
}
enum Kind {
    Gain(Rewrite),
    Trim(Rewrite),
    Bitdepth(Convert),
    Finalize(Finalize),
}
/// Gain and trim: float rewrite through the final-PCM writer (numbered, checkpointed).
struct Rewrite {
    source: Source,
    layout: Vec<u8>,
    writer: Option<output::Writer>,
    gain: f32,
    skip: u64,
    left: u64,
    done: u64,
    total: u64,
    buffer: Vec<f32>,
}
struct Convert {
    source: mradm_wav::Reader<BufReader<File>>,
    writer: Option<mradm_wav::Writer<File>>,
    bits: u16,
    left: u64,
    done: u64,
    total: u64,
    buffer: Vec<f32>,
}
struct Finalize {
    rewriter: Option<LayoutRewriter<BufReader<File>, BufWriter<File>>>,
    done: u64,
    total: u64,
}

/// `apply_gain_to_file`: unchanged within 1e-6 of unity; `cancelled` is checked after that test.
pub fn gain(path: &str, gain: f32, layout: &[u8], cancelled: bool) -> Result<Begin> {
    if (gain - 1.0).abs() < 1e-6 {
        return Ok(Begin::Skipped(event(1.0, 0, 0, "gain skipped")));
    }
    if cancelled {
        return Err(fail(5, "render cancelled", at(path)));
    }
    let temporary = Temporary {
        path: random_sidecar(Path::new(path), "gain_tmp"),
        armed: true,
    };
    let source = Source::open(path, "apply_gain_to_file")?;
    let total = source.frames();
    let start = event(0.0, 0, total, "applying gain");
    Ok(Begin::Started(
        Box::new(Pass {
            kind: Kind::Gain(Rewrite::new(source, layout, gain, 0, total)),
            path: path.to_owned(),
            temporary,
        }),
        start,
    ))
}

/// `trim_file_frames`: the range is clamped to the file; the whole file is left untouched.
pub fn trim(path: &str, start_frame: u64, frame_count: u64, layout: &[u8]) -> Result<Begin> {
    let temporary = Temporary {
        path: random_sidecar(Path::new(path), "trim_tmp"),
        armed: true,
    };
    let source = Source::open(path, "trim_file_frames")?;
    let total = source.frames();
    let start = start_frame.min(total);
    let frames = frame_count.min(total - start);
    if frames == 0 {
        return Err(fail(
            1,
            "trim range selects no audio frames",
            format!(
                "path={path} start_frame={start_frame} frame_count={frame_count} total_frames={total}"
            ),
        ));
    }
    if start == 0 && frames == total {
        return Ok(Begin::Skipped(event(1.0, frames, frames, "trim skipped")));
    }
    Ok(Begin::Started(
        Box::new(Pass {
            kind: Kind::Trim(Rewrite::new(source, layout, 1.0, start, frames)),
            path: path.to_owned(),
            temporary,
        }),
        event(0.0, 0, frames, "trimming output"),
    ))
}

/// `downconvert_to_int`: float WAVE to 16/24/32-bit integer PCM, installed with a backup.
pub fn bitdepth(path: &str, bits: u16) -> Result<Begin> {
    if !matches!(bits, 16 | 24 | 32) {
        return Err(fail(
            1,
            format!("integer PCM bit depth must be 16, 24, or 32; got {bits}"),
            at(path),
        ));
    }
    let Some(target) = indexed_sidecar(Path::new(path), "bitdepth_tmp") else {
        return Err(fail(
            IO,
            "cannot allocate integer WAVE temporary path",
            at(path),
        ));
    };
    let temporary = Temporary {
        path: target,
        armed: false,
    };
    let source = wave(path)?;
    let info = source.info();
    let total = info.frames;
    let channels = usize::from(info.format.channels);
    Ok(Begin::Started(
        Box::new(Pass {
            kind: Kind::Bitdepth(Convert {
                source,
                writer: None,
                bits,
                left: total,
                done: 0,
                total,
                buffer: vec![0.0; channels * BLOCK as usize],
            }),
            path: path.to_owned(),
            temporary,
        }),
        event(0.0, 0, total, "converting bit depth"),
    ))
}

/// How a WAV file's channel semantics are written (`WavLayoutFinalization` in C++).
#[derive(Clone, Debug, Default, PartialEq)]
pub struct Layout {
    pub channel_mask: u32,
    /// File channel -> render channel; empty means identity.
    pub permutation: Vec<u16>,
    pub axml: Vec<u8>,
    pub chna: Vec<ChnaEntry>,
    pub force_extensible: bool,
    pub prefer_riff: bool,
    pub include_pcm_fact: bool,
}
/// One CHNA track assignment.
#[derive(Clone, Debug, Default, PartialEq)]
pub struct ChnaEntry {
    /// One-based audio track.
    pub track_index: u16,
    pub uid: Vec<u8>,
    pub track_format: Vec<u8>,
    pub pack_format: Vec<u8>,
}
/// The WAV output profile: ADM semantics, or the 7.1.4 / 9.1.6 speaker re-render profile.
#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub enum Profile {
    AdmSemantics,
    SpeakerRerender,
}

fn adm(
    kind: u32,
    name: &str,
    speakers: &[mradm_adm::Speaker],
) -> std::result::Result<Layout, String> {
    let generated = mradm_adm::generate(kind, name, speakers).map_err(|e| e.message)?;
    Ok(Layout {
        axml: generated.axml.into_bytes(),
        chna: generated
            .chna
            .into_iter()
            .map(|c| ChnaEntry {
                track_index: c.track,
                uid: c.uid.into(),
                track_format: c.format.into(),
                pack_format: c.pack.into(),
            })
            .collect(),
        ..Layout::default()
    })
}
fn direct_speakers(id: &str) -> std::result::Result<Layout, String> {
    let layout = mradm_scene::layouts::find(id, mradm_scene::layouts::Geometry::Standard)
        .ok_or_else(|| format!("speaker layout '{id}' is unavailable"))?;
    let speakers: Vec<_> = layout
        .speakers
        .iter()
        .map(|s| mradm_adm::Speaker {
            label: s.label.to_owned(),
            azimuth: s.azimuth,
            elevation: s.elevation,
            lfe: s.is_lfe,
            azimuth_range: s.azimuth_range.map(|[a, b]| (a, b)),
            elevation_range: s.elevation_range.map(|[a, b]| (a, b)),
        })
        .collect();
    adm(1, layout.display_name, &speakers)
}

/// The channel mask, 7.1.4 permutation or rendered ADM for a raw output layout id.
pub fn layout_for(raw: &str, profile: Profile) -> std::result::Result<Layout, String> {
    let mut base = Layout::default();
    if profile == Profile::SpeakerRerender {
        if !matches!(raw, "4+7+0" | "7.1.4" | "9.1.6") {
            return Err("speaker re-render WAV profile requires 7.1.4 or 9.1.6".into());
        }
        base.force_extensible = true;
        base.prefer_riff = true;
        base.include_pcm_fact = true;
        // The 9.1.6 compatibility profile omits the speaker mask and rendered ADM: CHNA would
        // make CoreAudio expose an ADM/discrete 16.0 track. Samples stay in native 9.1.6 order;
        // CAF carries the explicit Atmos_9_1_6 tag for layout-aware playback.
        if raw == "9.1.6" {
            return Ok(base);
        }
    }
    let mask = |channel_mask| Layout {
        channel_mask,
        ..base.clone()
    };
    let with_adm = |layout: Layout| Layout {
        axml: layout.axml,
        chna: layout.chna,
        ..base.clone()
    };
    Ok(match raw {
        "0+2+0" => mask(0x0003),
        "0+5+0" | "5.1" => mask(0x003F),
        "2+5+0" | "5.1.2" => mask(0x503F),
        "wav71" | "7.1" => mask(0x063F),
        "4+5+0" | "5.1.4" => mask(0x2D03F),
        // Renderer/Atmos order has side L/R before rear L/R; WAVE mask order is ascending bit
        // position: rear L/R before side L/R.
        "4+7+0" | "7.1.4" => Layout {
            permutation: vec![0, 1, 2, 3, 6, 7, 4, 5, 8, 9, 10, 11],
            ..mask(0x2D63F)
        },
        "binaural" => with_adm(adm(5, "Binaural", &[])?),
        "4+5+4" | "9.1.4" => with_adm(direct_speakers("4+5+4")?),
        "9.1.6" => with_adm(direct_speakers("9.1.6")?),
        "9+10+3" | "22.2" => with_adm(direct_speakers("9+10+3")?),
        "hoa3" => with_adm(adm(4, "HOA3 ACN SN3D", &[])?),
        _ => {
            return Err(format!(
                "WAV output layout '{raw}' has no serialization definition"
            ));
        }
    })
}

/// `finalize_rendered_wav`: the layout for `raw` and `profile`, then `finalize`.
pub fn finalize_rendered(
    path: &str,
    raw: &str,
    profile: Profile,
    cancelled: bool,
) -> Result<Begin> {
    let layout = layout_for(raw, profile).map_err(|what| {
        fail(
            IO,
            format!("failed to build WAV layout metadata: {what}"),
            format!("path={path} layout={raw}"),
        )
    })?;
    if cancelled {
        return Err(fail(5, "render cancelled", at(path)));
    }
    finalize(path, &layout)
}

/// `finalize_wav_layout`: rewrites the channel order, mask, ADM chunks and container.
pub fn finalize(path: &str, layout: &Layout) -> Result<Begin> {
    let Some(target) = indexed_sidecar(Path::new(path), "layout_tmp") else {
        return Err(fail(
            IO,
            "cannot allocate WAV layout temporary path",
            at(path),
        ));
    };
    let temporary = Temporary {
        path: target,
        armed: true,
    };
    let tracks: Vec<_> = layout
        .chna
        .iter()
        .map(|entry| ChnaTrack {
            track_index: entry.track_index,
            uid: &entry.uid,
            track_format: &entry.track_format,
            pack_format: &entry.pack_format,
        })
        .collect();
    let options = LayoutOptions {
        channel_mask: layout.channel_mask,
        force_extensible: layout.force_extensible,
        include_pcm_fact: layout.include_pcm_fact,
        prefer_riff: layout.prefer_riff,
        permutation: &layout.permutation,
        axml: &layout.axml,
        chna: &tracks,
    };
    let rewriter = (|| {
        let input = BufReader::new(File::open(path)?);
        let output = OpenOptions::new()
            .write(true)
            .create_new(true)
            .open(&temporary.path)?;
        LayoutRewriter::new(input, BufWriter::new(output), &options)
    })()
    .map_err(|e| wav(e, path))?;
    let total = rewriter.frames();
    Ok(Begin::Started(
        Box::new(Pass {
            kind: Kind::Finalize(Finalize {
                rewriter: Some(rewriter),
                done: 0,
                total,
            }),
            path: path.to_owned(),
            temporary,
        }),
        event(0.0, 0, total, "finalizing WAV channel layout"),
    ))
}

impl Rewrite {
    fn new(source: Source, layout: &[u8], gain: f32, skip: u64, total: u64) -> Self {
        let channels = source.channels() as usize;
        Self {
            layout: source.layout(layout),
            source,
            writer: None,
            gain,
            skip,
            left: total,
            done: 0,
            total,
            buffer: vec![0.0; channels * BLOCK as usize],
        }
    }
    fn step(
        &mut self,
        path: &str,
        temporary: &Path,
        trim: bool,
        message: &'static str,
    ) -> Result<Step> {
        let target = text(temporary);
        let operation = if trim {
            "trim_file_frames"
        } else {
            "apply_gain_to_file"
        };
        let Some(writer) = self.writer.as_mut() else {
            let layout = is_caf(temporary).then_some(self.layout.as_slice());
            let writer = output::Writer::create(
                temporary,
                self.source.channels(),
                self.source.rate(),
                layout,
            )
            .map_err(|e| wav(e, &target))?;
            return self.finish_if_done(writer, &target);
        };
        let channels = self.source.channels() as usize;
        if self.skip > 0 {
            // The readers are forward-only here: the head is read and discarded.
            let n = self.skip.min(BLOCK) as usize;
            let got = self.source.read(&mut self.buffer[..n * channels]);
            if got == 0 {
                return Err(fail(
                    IO,
                    "short read while skipping to trim start",
                    at(path),
                ));
            }
            self.skip -= got;
            return Ok(more(quiet()));
        }
        let n = self.left.min(BLOCK) as usize;
        let got = self.source.read(&mut self.buffer[..n * channels]) as usize;
        if got == 0 {
            let what = if trim {
                "short read in trim_file_frames"
            } else {
                "short read in apply_gain_to_file"
            };
            return Err(fail(IO, what, at(path)));
        }
        let samples = &mut self.buffer[..got * channels];
        if !trim {
            mradm_dsp::post_gain::apply(samples, self.gain);
        }
        if writer.write(samples).is_err() {
            return Err(fail(IO, format!("short write in {operation}"), at(&target)));
        }
        self.left -= got as u64;
        self.done += got as u64;
        let progress = event(ratio(self.done, self.total), self.done, self.total, message);
        if self.left == 0 {
            writer.finish().map_err(|e| wav(e, &target))?;
            return Ok(Step {
                progress,
                complete: true,
            });
        }
        Ok(more(progress))
    }
    /// Installs a just-opened writer; an empty rewrite is finished at once.
    fn finish_if_done(&mut self, mut writer: output::Writer, target: &str) -> Result<Step> {
        let complete = self.left == 0 && self.skip == 0;
        if complete {
            writer.finish().map_err(|e| wav(e, target))?;
        }
        self.writer = Some(writer);
        Ok(Step {
            progress: quiet(),
            complete,
        })
    }
}

impl Pass {
    /// One block (4096 frames; 2048 for layout finalization).
    pub fn step(&mut self) -> Result<Step> {
        let path = self.path.as_str();
        match &mut self.kind {
            Kind::Gain(r) => r.step(path, &self.temporary.path, false, "applying gain"),
            Kind::Trim(r) => r.step(path, &self.temporary.path, true, "trimming output"),
            Kind::Bitdepth(c) => {
                let target = text(&self.temporary.path);
                let Some(writer) = c.writer.as_mut() else {
                    let info = c.source.info();
                    let writer = mradm_wav::create_file_writer(
                        &self.temporary.path,
                        u32::from(info.format.channels),
                        info.format.sample_rate,
                        c.bits,
                        false,
                        true,
                    )
                    .map_err(|e| wav(e, &target))?;
                    // The exclusive create established ownership; never remove a competing file.
                    self.temporary.armed = true;
                    if c.left == 0 {
                        writer.finish().map_err(|e| wav(e, &target))?;
                        return Ok(Step {
                            progress: quiet(),
                            complete: true,
                        });
                    }
                    c.writer = Some(writer);
                    return Ok(more(quiet()));
                };
                let channels = c.buffer.len() / BLOCK as usize;
                let n = c.left.min(BLOCK);
                let samples = &mut c.buffer[..n as usize * channels];
                if c.source.read_frames(samples).unwrap_or(0) != n {
                    return Err(fail(IO, "short input read while converting PCM", at(path)));
                }
                writer.write_frames(samples).map_err(|e| wav(e, &target))?;
                c.left -= n;
                c.done += n;
                let progress = event(
                    ratio(c.done, c.total),
                    c.done,
                    c.total,
                    "converting bit depth",
                );
                if c.left == 0 {
                    c.writer
                        .take()
                        .unwrap()
                        .finish()
                        .map_err(|e| wav(e, &target))?;
                    return Ok(Step {
                        progress,
                        complete: true,
                    });
                }
                Ok(more(progress))
            }
            Kind::Finalize(f) => {
                let mut progress = quiet();
                if f.done < f.total {
                    let rewriter = f
                        .rewriter
                        .as_mut()
                        .expect("finalize stepped after completion");
                    let copied = rewriter.step(2048).map_err(|e| wav(e, path))?;
                    if copied == 0 {
                        return Err(fail(
                            IO,
                            "WAV layout rewrite stopped before the last frame",
                            at(path),
                        ));
                    }
                    f.done += copied;
                    progress = event(
                        f.done as f64 / f.total as f64,
                        f.done,
                        f.total,
                        "finalizing WAV channel layout",
                    );
                    if f.done < f.total {
                        return Ok(more(progress));
                    }
                }
                // The last block is followed directly by the finish, without a cancellation check.
                let rewriter = f
                    .rewriter
                    .take()
                    .expect("finalize stepped after completion");
                (|| {
                    let mut sink = rewriter.finish()?;
                    sink.flush()?;
                    sink.into_inner()
                        .map_err(|error| mradm_wav::Error::io(error.error().to_string()))?;
                    Ok(())
                })()
                .map_err(|e| wav(e, path))?;
                Ok(Step {
                    progress,
                    complete: true,
                })
            }
        }
    }
    /// Whether the caller checks cancellation once more before `finish` installs the output.
    pub fn cancel_before_install(&self) -> bool {
        !matches!(self.kind, Kind::Finalize(_))
    }
    /// Installs the completed output and returns the final progress event.
    pub fn finish(mut self) -> Result<Progress> {
        let path = self.path.clone();
        match self.kind {
            Kind::Gain(_) => {
                install_replace(&mut self.temporary, &path, "apply_gain_to_file")?;
                Ok(event(1.0, 0, 0, "gain applied"))
            }
            Kind::Trim(_) => {
                install_replace(&mut self.temporary, &path, "trim_file_frames")?;
                Ok(event(1.0, 0, 0, "trimmed output"))
            }
            Kind::Bitdepth(Convert { total, .. }) => {
                install_with_backup(&path, &mut self.temporary)?;
                Ok(event(1.0, total, total, "bit depth converted"))
            }
            Kind::Finalize(Finalize { total, .. }) => {
                install_with_backup(&path, &mut self.temporary)?;
                Ok(event(1.0, total, total, "WAV channel layout finalized"))
            }
        }
    }
}

/// `replace_wav_axml`: `target` becomes a copy of `source` with the axml payload replaced.
/// Source and target may be the same file, a symlink or a hard link: a separate completed file
/// is installed.
pub fn replace_axml(source: &str, target: &str, axml: &[u8]) -> Result<()> {
    let Some(rewritten) = indexed_sidecar(Path::new(target), "axml_tmp") else {
        return Err(fail(IO, "无法分配 WAVE AXML 临时路径", at(target)));
    };
    // Exclusive create; the guard is armed only once creating and writing succeeded, so a name
    // collision never deletes another file.
    let mut rewritten = Temporary {
        path: rewritten,
        armed: false,
    };
    let context = format!("{source} -> {target}");
    (|| -> mradm_wav::Result<()> {
        let input = BufReader::new(File::open(source)?);
        let output = OpenOptions::new()
            .write(true)
            .create_new(true)
            .open(&rewritten.path)?;
        let written = (|| {
            let mut sink = BufWriter::new(output);
            mradm_wav::replace_chunk(input, &mut sink, *b"axml", axml)?;
            sink.into_inner()
                .map_err(|error| mradm_wav::Error::io(error.error().to_string()))?;
            Ok(())
        })();
        if written.is_err() {
            let _ = std::fs::remove_file(&rewritten.path);
        }
        written
    })()
    .map_err(|e| wav(e, &context))?;
    rewritten.armed = true;
    if let Err(error) = std::fs::rename(&rewritten.path, target) {
        // Windows cannot rename onto an existing file: use the recoverable replacement, but
        // never move an existing directory out of the way.
        if std::fs::metadata(target).is_ok_and(|m| m.is_file()) {
            return install_with_backup(target, &mut rewritten);
        }
        return Err(fail(
            IO,
            format!("无法安装 AXML 重写后的 WAVE：{}", os(&error)),
            at(target),
        ));
    }
    rewritten.armed = false;
    Ok(())
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn sidecar_names_follow_std_filesystem_stem_and_extension() {
        let name = |p: &str| sidecar(Path::new(p), "x", "0001");
        assert_eq!(name("a.wav"), Path::new("a.x.0001.wav"));
        assert_eq!(name("dir/a.b.caf"), Path::new("dir/a.b.x.0001.caf"));
        assert_eq!(name("dir/.hidden"), Path::new("dir/.hidden.x.0001"));
        assert_eq!(name("a."), Path::new("a.x.0001."));
        assert_eq!(name("noext"), Path::new("noext.x.0001"));
        assert_eq!(name("..a"), Path::new("..x.0001.a"));
    }

    fn scratch(name: &str) -> PathBuf {
        let directory =
            std::env::temp_dir().join(format!("mradm-post-{name}-{}", std::process::id()));
        let _ = std::fs::remove_dir_all(&directory);
        std::fs::create_dir_all(&directory).unwrap();
        directory
    }
    fn wav(path: &Path, channels: u32, frames: usize) {
        let mut writer = output::Writer::create(path, channels, 48000, None).unwrap();
        let samples: Vec<f32> = (0..frames * channels as usize)
            .map(|i| i as f32 * 1e-4)
            .collect();
        writer.write(&samples).unwrap();
        writer.finish().unwrap();
    }
    fn names(directory: &Path) -> Vec<String> {
        let mut names: Vec<_> = std::fs::read_dir(directory)
            .unwrap()
            .map(|e| e.unwrap().file_name().into_string().unwrap())
            .collect();
        names.sort();
        names
    }
    fn run(begin: Result<Begin>) -> Result<Progress> {
        match begin? {
            Begin::Skipped(progress) => Ok(progress),
            Begin::Started(mut pass, _) => {
                while !pass.step()?.complete {}
                pass.finish()
            }
        }
    }

    #[test]
    fn abandoned_passes_leave_only_the_original() {
        let _turn = output::TEST_WRITER_LOCK
            .lock()
            .unwrap_or_else(|e| e.into_inner());
        let directory = scratch("abandon");
        let path = directory.join("a.wav");
        wav(&path, 2, 9000);
        let before = std::fs::read(&path).unwrap();
        let path = path.to_str().unwrap();
        let begins: [fn(&str) -> Result<Begin>; 4] = [
            |p| gain(p, 0.5, b"", false),
            |p| trim(p, 10, 8000, b""),
            |p| bitdepth(p, 24),
            |p| finalize_rendered(p, "0+2+0", Profile::AdmSemantics, false),
        ];
        for (index, begin) in begins.iter().enumerate() {
            for steps in 0..3 {
                let Begin::Started(mut pass, _) = begin(path).unwrap() else {
                    panic!("pass {index} skipped");
                };
                for _ in 0..steps {
                    pass.step().unwrap();
                }
                drop(pass);
                assert_eq!(
                    names(&directory),
                    ["a.wav"],
                    "pass {index} after {steps} steps"
                );
                assert_eq!(std::fs::read(path).unwrap(), before);
            }
        }
        std::fs::remove_dir_all(&directory).unwrap();
    }

    #[test]
    fn failed_installs_restore_the_original() {
        let directory = scratch("install");
        let original = directory.join("a.wav");
        std::fs::write(&original, b"original").unwrap();
        let original = original.to_str().unwrap();
        let mut missing = Temporary {
            path: directory.join("gone.wav"),
            armed: true,
        };
        let error = install_with_backup(original, &mut missing).unwrap_err();
        assert!(
            error
                .message
                .starts_with("cannot install layout-finalized WAV: "),
            "{}",
            error.message
        );
        assert!(!error.message.contains("os error"));
        assert_eq!(error.context, format!("path={original}"));
        assert_eq!(std::fs::read(original).unwrap(), b"original");
        assert_eq!(names(&directory), ["a.wav"]);
        // As in C++, the fallback removes the original before its second rename.
        let error = install_replace(&mut missing, original, "trim_file_frames").unwrap_err();
        assert!(
            error
                .message
                .starts_with("failed to replace output after trim_file_frames: ")
        );
        assert!(!Path::new(original).exists());
        std::fs::write(original, b"original").unwrap();
        // Every backup name taken: nothing is moved.
        for index in 0..1024 {
            std::fs::write(
                directory.join(format!("a.layout_backup.{index:04}.wav")),
                b"",
            )
            .unwrap();
        }
        std::fs::write(directory.join("new.wav"), b"new").unwrap();
        let mut rewritten = Temporary {
            path: directory.join("new.wav"),
            armed: true,
        };
        let error = install_with_backup(original, &mut rewritten).unwrap_err();
        assert_eq!(error.message, "cannot allocate WAV layout backup path");
        assert_eq!(std::fs::read(original).unwrap(), b"original");
        drop(rewritten);
        assert!(!directory.join("new.wav").exists());
        std::fs::remove_dir_all(&directory).unwrap();
    }

    #[test]
    fn layout_table_masks_order_and_profiles() {
        let mask = |raw| layout_for(raw, Profile::AdmSemantics).unwrap().channel_mask;
        assert_eq!(
            [
                mask("0+2+0"),
                mask("5.1"),
                mask("5.1.2"),
                mask("7.1"),
                mask("5.1.4"),
                mask("7.1.4")
            ],
            [0x3, 0x3F, 0x503F, 0x63F, 0x2D03F, 0x2D63F]
        );
        let atmos = layout_for("4+7+0", Profile::AdmSemantics).unwrap();
        assert_eq!(atmos.permutation, [0, 1, 2, 3, 6, 7, 4, 5, 8, 9, 10, 11]);
        assert!(!atmos.force_extensible && atmos.axml.is_empty());
        let rerender = layout_for("7.1.4", Profile::SpeakerRerender).unwrap();
        assert!(rerender.force_extensible && rerender.prefer_riff && rerender.include_pcm_fact);
        assert_eq!(rerender.channel_mask, 0x2D63F);
        let compat = layout_for("9.1.6", Profile::SpeakerRerender).unwrap();
        assert!(compat.axml.is_empty() && compat.channel_mask == 0 && compat.force_extensible);
        assert_eq!(
            layout_for("5.1", Profile::SpeakerRerender).unwrap_err(),
            "speaker re-render WAV profile requires 7.1.4 or 9.1.6"
        );
        assert_eq!(
            layout_for("9+10+3", Profile::AdmSemantics)
                .unwrap()
                .chna
                .len(),
            24
        );
        assert_eq!(
            layout_for("hoa3", Profile::AdmSemantics)
                .unwrap()
                .chna
                .len(),
            16
        );
        assert_eq!(
            layout_for("nope", Profile::AdmSemantics).unwrap_err(),
            "WAV output layout 'nope' has no serialization definition"
        );
    }

    #[test]
    fn rewrites_keep_the_source_caf_tag_and_unity_gain_is_skipped() {
        let _turn = output::TEST_WRITER_LOCK
            .lock()
            .unwrap_or_else(|e| e.into_inner());
        let directory = scratch("caf");
        let path = directory.join("a.caf");
        let mut writer = output::Writer::create(&path, 2, 48000, Some(b"binaural")).unwrap();
        writer.write(&[0.5; 2 * 300]).unwrap();
        writer.finish().unwrap();
        let path = path.to_str().unwrap();
        let tag = || caf::Reader::open(Path::new(path)).unwrap().layout_tag();
        let binaural = tag();
        run(gain(path, 0.5, b"0+2+0", false)).unwrap();
        run(trim(path, 10, 100, b"0+2+0")).unwrap();
        assert_eq!(tag(), binaural);
        assert_eq!(
            caf::Reader::open(Path::new(path)).unwrap().frame_count(),
            100
        );
        let skipped = run(gain(path, 1.0 + 9e-7, b"", true)).unwrap();
        assert_eq!(skipped.message, "gain skipped");
        assert_eq!(run(gain(path, 1.0 + 2e-6, b"", true)).unwrap_err().code, 5);
        std::fs::remove_dir_all(&directory).unwrap();
    }

    #[test]
    fn trim_clamps_and_rejects_empty_ranges() {
        let _turn = output::TEST_WRITER_LOCK
            .lock()
            .unwrap_or_else(|e| e.into_inner());
        let directory = scratch("trim");
        let path = directory.join("a.wav");
        wav(&path, 1, 100);
        let path = path.to_str().unwrap();
        assert_eq!(
            run(trim(path, 0, 1000, b"")).unwrap().message,
            "trim skipped"
        );
        let error = run(trim(path, 100, 5, b"")).unwrap_err();
        assert_eq!(error.message, "trim range selects no audio frames");
        assert_eq!(
            error.context,
            format!("path={path} start_frame=100 frame_count=5 total_frames=100")
        );
        run(trim(path, 90, 1000, b"")).unwrap();
        assert_eq!(wave(path).unwrap().info().frames, 10);
        std::fs::remove_dir_all(&directory).unwrap();
    }

    #[test]
    fn os_messages_drop_the_rust_suffix() {
        let error = std::io::Error::from_raw_os_error(2);
        assert!(!os(&error).contains("os error"));
        assert_eq!(os(&std::io::Error::other("plain")), "plain");
    }
}
