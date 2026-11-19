//! `mr_adm_triple_balance_monitor_benchmark [layout=9.1.6]`: Triple Balance file streaming and
//! monitoring of a 70-second bed-and-objects input (formerly
//! `tests/manual/triple_balance_monitor_benchmark.cpp`, which drove the C++ SDK's renderer shell
//! and `MonitorEngine`). Release builds only.
//!
//! It writes the input (an RC 7.1.2 bed and three moving Cartesian objects, two with size 0.3),
//! imports it, streams the whole file through the Triple Balance backend for the real-time factor,
//! times a cold and a cached seek to 40 s, then monitors a 4-8 s loop for 60 s on a null output
//! clock. Exit code 0 when RTF < 1 and the monitor neither failed nor underran.
use mradm_engine::monitor::{Monitor, MonitorHost, Native, Source};
use mradm_engine::request::Selection;
use mradm_engine::stream::{self, Monitor as Opened};
use mradm_ffi::api::render::FfiHost;
use mradm_scene::import::{ImportOptions, import};
use mradm_wav::{Chna, ChnaEntry, Format, SampleFormat, Writer, WriterOptions};
use std::fmt::Write as _;
use std::io::Write as _;
use std::path::{Path, PathBuf};
use std::process::ExitCode;
use std::sync::Arc;
use std::time::{Duration, Instant};

#[path = "consistency/request.rs"]
#[allow(dead_code)]
mod consistency_request;
mod probe;
use probe::NullClock;

const RATE: u32 = 48000;
const INPUTS: usize = 13;
const FRAMES: u64 = RATE as u64 * 70;
const LABELS: [&str; 10] = [
    "RC_L", "RC_R", "RC_C", "RC_LFE", "RC_Lss", "RC_Rss", "RC_Lrs", "RC_Rrs", "RC_Lts", "RC_Rts",
];

/// The input is removed when the benchmark ends.
struct InputFile(PathBuf);
impl Drop for InputFile {
    fn drop(&mut self) {
        let _ = std::fs::remove_file(&self.0);
    }
}

fn fixed<const N: usize>(text: &str) -> [u8; N] {
    let mut out = [0u8; N];
    out[..text.len()].copy_from_slice(text.as_bytes());
    out
}

/// The AXML and CHNA of the bed (channels 1-10, one DirectSpeakers object) and the three objects
/// (channels 11-13), each object with one block per second along a circle at z 0.5.
fn metadata() -> (String, Chna) {
    let (mut formats, mut uids, mut objects, mut top) =
        (String::new(), String::new(), String::new(), String::new());
    let mut entries = Vec::new();
    let mut bed_refs = String::new();
    let mut bed_uids = String::new();
    for channel in 0..INPUTS {
        let n = channel + 1;
        let ty = if channel < 10 { "0001" } else { "0003" };
        let code = format!("{ty}{:04X}", 0x1000 + n);
        let (cf, tf, sf, uid) = (
            format!("AC_{code}"),
            format!("AT_{code}_01"),
            format!("AS_{code}"),
            format!("ATU_{n:08X}"),
        );
        let pack = if channel < 10 {
            "AP_00011001".to_owned()
        } else {
            format!("AP_0003{:04X}", 0x1000 + n)
        };
        let mut blocks = String::new();
        if channel < 10 {
            let _ = write!(
                blocks,
                r#"<audioBlockFormat audioBlockFormatID="AB_{code}_00000001"><position coordinate="X">0</position><position coordinate="Y">0</position><position coordinate="Z">0</position><cartesian>1</cartesian><speakerLabel>{}</speakerLabel></audioBlockFormat>"#,
                LABELS.get(channel).copied().unwrap_or_default()
            );
        } else {
            let size = if channel == 12 { 0. } else { 0.3 };
            for (index, frame) in (0..FRAMES).step_by(RATE as usize).enumerate() {
                let t = frame as f32 / RATE as f32;
                let _ = write!(
                    blocks,
                    r#"<audioBlockFormat audioBlockFormatID="AB_{code}_{:08X}" rtime="00:00:{:02}.000000000"><position coordinate="X">{}</position><position coordinate="Y">{}</position><position coordinate="Z">0.5</position><cartesian>1</cartesian><width>{size}</width><height>{size}</height><depth>{size}</depth></audioBlockFormat>"#,
                    index + 1,
                    frame / u64::from(RATE),
                    0.6 * t.sin(),
                    0.6 * t.cos(),
                );
            }
        }
        let _ = write!(
            formats,
            r#"<audioChannelFormat audioChannelFormatID="{cf}" audioChannelFormatName="track-{channel}" typeLabel="{ty}">{blocks}</audioChannelFormat><audioStreamFormat audioStreamFormatID="{sf}" audioStreamFormatName="track-{channel}" formatDefinition="PCM" formatLabel="0001"><audioChannelFormatIDRef>{cf}</audioChannelFormatIDRef><audioTrackFormatIDRef>{tf}</audioTrackFormatIDRef></audioStreamFormat><audioTrackFormat audioTrackFormatID="{tf}" audioTrackFormatName="track-{channel}" formatDefinition="PCM" formatLabel="0001"><audioStreamFormatIDRef>{sf}</audioStreamFormatIDRef></audioTrackFormat>"#
        );
        let _ = write!(
            uids,
            r#"<audioTrackUID UID="{uid}"><audioTrackFormatIDRef>{tf}</audioTrackFormatIDRef><audioPackFormatIDRef>{pack}</audioPackFormatIDRef></audioTrackUID>"#
        );
        if channel < 10 {
            let _ = write!(
                bed_refs,
                "<audioChannelFormatIDRef>{cf}</audioChannelFormatIDRef>"
            );
            let _ = write!(bed_uids, "<audioTrackUIDRef>{uid}</audioTrackUIDRef>");
        } else {
            let id = format!("AO_{:04X}", 0x1000 + n);
            let _ = write!(
                formats,
                r#"<audioPackFormat audioPackFormatID="{pack}" audioPackFormatName="track-{channel}" typeLabel="0003"><audioChannelFormatIDRef>{cf}</audioChannelFormatIDRef></audioPackFormat>"#
            );
            let _ = write!(
                objects,
                r#"<audioObject audioObjectID="{id}" audioObjectName="track-{channel}"><audioPackFormatIDRef>{pack}</audioPackFormatIDRef><audioTrackUIDRef>{uid}</audioTrackUIDRef></audioObject>"#
            );
            let _ = write!(top, "<audioObjectIDRef>{id}</audioObjectIDRef>");
        }
        entries.push(ChnaEntry {
            track_index: n as u16,
            uid: fixed(&uid),
            track_format: fixed(&tf),
            pack_format: fixed(&pack),
            pad: 0,
        });
    }
    let _ = write!(
        formats,
        r#"<audioPackFormat audioPackFormatID="AP_00011001" audioPackFormatName="bed" typeLabel="0001">{bed_refs}</audioPackFormat>"#
    );
    let _ = write!(
        objects,
        r#"<audioObject audioObjectID="AO_1001" audioObjectName="bed"><audioPackFormatIDRef>AP_00011001</audioPackFormatIDRef>{bed_uids}</audioObject>"#
    );
    let xml = format!(
        r#"<?xml version="1.0" encoding="UTF-8"?><ebuCoreMain><coreMetadata><format><audioFormatExtended><audioProgramme audioProgrammeID="APR_1001" audioProgrammeName="benchmark"><audioContentIDRef>ACO_1001</audioContentIDRef></audioProgramme><audioContent audioContentID="ACO_1001" audioContentName="benchmark">{top}<audioObjectIDRef>AO_1001</audioObjectIDRef></audioContent>{objects}{formats}{uids}</audioFormatExtended></format></coreMetadata></ebuCoreMain>"#
    );
    let chna = Chna {
        tracks: INPUTS as u16,
        entries,
    };
    (xml, chna)
}

/// A float32 WAVE of the metadata and a quiet sine per channel.
fn write_input(path: &Path) -> Result<(), String> {
    let (axml, chna) = metadata();
    let format = Format {
        channels: INPUTS as u16,
        sample_rate: RATE,
        sample_format: SampleFormat::Float32,
        channel_mask: None,
    };
    let options = WriterOptions {
        container: None,
        axml: Some(axml.into_bytes()),
        chna: Some(chna),
    };
    let file = std::fs::File::create(path).map_err(|e| e.to_string())?;
    let mut writer =
        Writer::new(std::io::BufWriter::new(file), format, options).map_err(|e| e.to_string())?;
    let mut pcm = vec![0f32; 1024 * INPUTS];
    for start in (0..FRAMES).step_by(1024) {
        let frames = (FRAMES - start).min(1024) as usize;
        for f in 0..frames {
            for ch in 0..INPUTS {
                pcm[f * INPUTS + ch] =
                    0.025 * ((start + f as u64) as f32 * (0.017 + 0.002 * ch as f32)).sin();
            }
        }
        writer
            .write_frames(&pcm[..frames * INPUTS])
            .map_err(|e| e.to_string())?;
    }
    writer.finish().map_err(|e| e.to_string())?;
    Ok(())
}

/// The null output's controls, as the monitor drives its device.
struct Host(Arc<NullClock>);
impl MonitorHost for Host {
    fn log(&self, _: u32, _: &str, _: &[u8]) {}
    fn device_flush(&self) {}
    fn device_pause(&self) {
        self.0.pause();
    }
    fn device_resume(&self) {
        self.0.resume();
    }
}

fn open(input: &Path, layout: &str, scene: &mradm_scene::RenderScene) -> Result<Native, String> {
    let mut r = consistency_request::request(input, input);
    r.output = None;
    r.renderer = Selection::TripleBalance as u32;
    r.output_layout = layout.to_owned();
    match stream::open_monitor(&r, false, Some(scene), false, &FfiHost).result {
        Ok(Opened::Stream {
            validator,
            body,
            info,
            ..
        }) => Ok(Native::new(validator, body, info)),
        Ok(Opened::Apple { .. }) => Err("unexpected Apple stream".to_owned()),
        Err(e) => Err(e.message),
    }
}

fn run() -> Result<bool, String> {
    let layout = std::env::args()
        .nth(1)
        .unwrap_or_else(|| "9.1.6".to_owned());
    let nanos = std::time::SystemTime::now()
        .duration_since(std::time::UNIX_EPOCH)
        .map_or(0, |d| d.as_nanos());
    let path = std::env::temp_dir().join(format!("mr_triple_benchmark_{nanos}.wav"));
    let guard = InputFile(path.clone());
    write_input(&guard.0)?;
    let text = path.to_str().ok_or("temporary path is not UTF-8")?;
    let scene = import(
        text.as_bytes(),
        ImportOptions {
            input_layout: None,
            input_channel_labels: &[],
        },
    )
    .map_err(|e| e.message)?;
    let mut stream = open(&path, &layout, &scene)?;
    let mut scratch = vec![0f32; 1024 * stream.out_channels()];
    let start = Instant::now();
    while stream
        .process(&mut scratch, 1024)
        .map_err(|e| String::from_utf8_lossy(&e.message).into_owned())?
        != 0
    {}
    let rtf = start.elapsed().as_secs_f64() / 70.;
    drop(stream);
    // A sequential render populates checkpoints. Use a fresh stream for the cold seek.
    let mut stream = open(&path, &layout, &scene)?;
    let seek = |stream: &mut Native| -> Result<f64, String> {
        let start = Instant::now();
        stream
            .seek(40 * u64::from(RATE) + 7)
            .map_err(|e| String::from_utf8_lossy(&e.message).into_owned())?;
        Ok(start.elapsed().as_secs_f64() * 1000.)
    };
    let cold_seek = seek(&mut stream)?;
    let warm_seek = seek(&mut stream)?;
    drop(stream);
    let mut out = std::io::stdout().lock();
    let _ = writeln!(
        out,
        "layout={layout} inputs=13 size_objects=2 point_objects=1 bed=7.1.2 RTF={rtf} cold_seek_ms={cold_seek} cached_seek_ms={warm_seek}"
    );
    let _ = out.flush();
    let source = open(&path, &layout, &scene)?;
    let channels = source.out_channels();
    let clock = Arc::new(NullClock::default());
    let monitor = Monitor::create(Box::new(source), true, Arc::new(Host(clock.clone()))).map_err(
        |e| match e {
            mradm_engine::monitor::CreateError::Error(e) => {
                String::from_utf8_lossy(&e.message).into_owned()
            }
            mradm_engine::monitor::CreateError::Construction(e) => e,
        },
    )?;
    let (before, status, elapsed) = std::thread::scope(|scope| {
        scope.spawn(|| {
            clock.run(channels, RATE, |buffer, frames| {
                monitor.pull(buffer, frames);
            });
        });
        monitor.set_loop(4 * u64::from(RATE) + 7, 8 * u64::from(RATE) + 11);
        monitor.play();
        std::thread::sleep(Duration::from_secs(1));
        let before = monitor.status().underruns;
        let start = Instant::now();
        for seconds in 0..60 {
            std::thread::sleep(Duration::from_secs(1));
            if (seconds + 1) % 15 == 0 {
                let _ = writeln!(
                    out,
                    "monitor_seconds={} underruns={}",
                    seconds + 1,
                    monitor.status().underruns - before
                );
                let _ = out.flush();
            }
        }
        let status = monitor.status();
        let elapsed = start.elapsed().as_secs_f64();
        clock.stop();
        (before, status, elapsed)
    });
    drop(monitor);
    let _ = write!(
        out,
        "elapsed_s={elapsed} underruns={} failed={}",
        status.underruns - before,
        u8::from(status.failed)
    );
    if cfg!(target_os = "macos") {
        let _ = write!(
            out,
            " peak_rss_MiB={}",
            probe::peak_rss() as f64 / (1024. * 1024.)
        );
    }
    let _ = writeln!(out);
    let _ = out.flush();
    Ok(rtf < 1. && !status.failed && status.underruns == before)
}

fn main() -> ExitCode {
    if cfg!(debug_assertions) {
        eprintln!("Run this benchmark from the Release build.");
        return ExitCode::from(2);
    }
    match run() {
        Ok(true) => ExitCode::SUCCESS,
        Ok(false) => ExitCode::from(1),
        Err(e) => {
            eprintln!("{e}");
            ExitCode::from(1)
        }
    }
}
