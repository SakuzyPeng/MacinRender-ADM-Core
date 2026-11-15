//! `mr_adm_pcm_bits`: PCM bit-pattern extraction and comparison for the consistency baselines
//! (formerly `tests/tools/pcm_bits.cpp`).
//!
//! The determinism contract compares final float32 PCM by bit pattern, not by container hash
//! (every output carries the current UTC time) and not by float tolerance (a zero difference does
//! not separate +0.0 from -0.0, and NaN would slip through).
//!
//! Modes:
//! - `extract <audio-in> <bits-out>`: decode WAV or CAF, reject non-finite samples, write the
//!   canonical little-endian bit image and print one metadata line.
//! - `validate <image>`: check a canonical image.
//! - `compare <a> <b>`: bit-compare two images or audio files; on a mismatch, report the first
//!   differing sample with both bit patterns, the maximum absolute error and the ULP distance.
//!
//! The image is `MRPB`, version 1, channels, sample rate (u32) and frame count (u64), then the
//! interleaved sample bits, all little-endian; a byte comparison of two images is the contract's
//! bit comparison. FLAC input is not read here: the C++ FLAC reader island has its own probe.
use mradm_testkit::adm::g;
use std::fs::File;
use std::io::{BufReader, Read};
use std::path::Path;
use std::process::ExitCode;

const MAGIC: &[u8; 4] = b"MRPB";
const VERSION: u32 = 1;
const HEADER: usize = 4 + 4 + 4 + 4 + 8;
const CHUNK_FRAMES: u64 = 8192;

struct Image {
    channels: u32,
    sample_rate: u32,
    frames: u64,
    bits: Vec<u32>,
}

/// An error already reported on stderr.
struct Reported;

fn fail(message: String) -> Reported {
    eprintln!("error: {message}");
    Reported
}

fn valid_shape(path: &str, img: &Image) -> Result<(), Reported> {
    if img.channels == 0
        || img.sample_rate == 0
        || img.frames > usize::MAX as u64 / u64::from(img.channels)
    {
        return Err(fail(format!("invalid or overflowing PCM shape in {path}")));
    }
    Ok(())
}

enum Audio {
    Wav(mradm_wav::Reader<BufReader<File>>),
    Caf(mradm_wav::caf::Reader<BufReader<File>>),
}

impl Audio {
    fn open(path: &str) -> Result<(Self, u32, u32, u64), String> {
        let ext = Path::new(path)
            .extension()
            .map(|e| e.to_string_lossy().to_ascii_lowercase())
            .unwrap_or_default();
        match ext.as_str() {
            "caf" => {
                let r = mradm_wav::caf::Reader::open(Path::new(path)).map_err(|e| e.to_string())?;
                let shape = (r.channels(), r.sample_rate(), r.frame_count());
                Ok((Audio::Caf(r), shape.0, shape.1, shape.2))
            }
            "flac" => Err("FLAC input is not supported (decode it to WAV first)".to_owned()),
            _ => {
                let file = File::open(path).map_err(|e| e.to_string())?;
                let r = mradm_wav::Reader::new(BufReader::new(file)).map_err(|e| e.to_string())?;
                let info = r.info();
                let shape = (
                    u32::from(info.format.channels),
                    info.format.sample_rate,
                    info.frames,
                );
                Ok((Audio::Wav(r), shape.0, shape.1, shape.2))
            }
        }
    }

    fn read(&mut self, out: &mut [f32]) -> Result<u64, String> {
        match self {
            Audio::Wav(r) => r.read_frames(out).map_err(|e| e.to_string()),
            Audio::Caf(r) => r.read(out).map_err(|e| e.to_string()),
        }
    }
}

/// Decodes `path` to bit patterns; non-finite samples are rejected rather than compared.
fn load_audio(path: &str) -> Result<Image, Reported> {
    let (mut reader, channels, sample_rate, frames) =
        Audio::open(path).map_err(|e| fail(format!("cannot open audio {path}: {e}")))?;
    let mut img = Image {
        channels,
        sample_rate,
        frames,
        bits: Vec::new(),
    };
    valid_shape(path, &img)?;
    let width = channels as usize;
    let mut chunk = vec![0f32; CHUNK_FRAMES as usize * width];
    img.bits.reserve(frames as usize * width);
    let mut done = 0u64;
    while done < frames {
        let want = CHUNK_FRAMES.min(frames - done) as usize;
        let got = reader
            .read(&mut chunk[..want * width])
            .map_err(|e| fail(format!("cannot decode {path}: {e}")))?;
        if got == 0 {
            break;
        }
        for (i, &s) in chunk[..got as usize * width].iter().enumerate() {
            if !s.is_finite() {
                return Err(fail(format!(
                    "non-finite sample in {path} at frame {} channel {} (bits=0x{:x})",
                    done + (i / width) as u64,
                    i % width,
                    s.to_bits()
                )));
            }
            img.bits.push(s.to_bits());
        }
        done += got;
    }
    if done != frames {
        return Err(fail(format!(
            "{path} declared {frames} frames but decoded {done}"
        )));
    }
    Ok(img)
}

fn u32_at(raw: &[u8], at: usize) -> u32 {
    u32::from_le_bytes(raw[at..at + 4].try_into().expect("four bytes"))
}

fn load_image(path: &str) -> Result<Image, Reported> {
    let raw = std::fs::read(path).map_err(|_| fail(format!("cannot open {path}")))?;
    if raw.len() < HEADER || &raw[..4] != MAGIC {
        return Err(fail(format!("{path} is not a .pcmbits image")));
    }
    let version = u32_at(&raw, 4);
    if version != VERSION {
        return Err(fail(format!(
            "{path} has image version {version}, expected {VERSION}"
        )));
    }
    let mut img = Image {
        channels: u32_at(&raw, 8),
        sample_rate: u32_at(&raw, 12),
        frames: u64::from(u32_at(&raw, 16)) | (u64::from(u32_at(&raw, 20)) << 32),
        bits: Vec::new(),
    };
    valid_shape(path, &img)?;
    let payload = raw.len() - HEADER;
    if !payload.is_multiple_of(4) {
        return Err(fail(format!(
            "{path} payload is not a whole number of samples"
        )));
    }
    let samples = payload / 4;
    let expect = img.frames as usize * img.channels as usize;
    if samples != expect {
        return Err(fail(format!(
            "{path} header declares {expect} samples but payload holds {samples}"
        )));
    }
    img.bits.reserve(samples);
    for i in 0..samples {
        let bits = u32_at(&raw, HEADER + i * 4);
        if !f32::from_bits(bits).is_finite() {
            return Err(fail(format!(
                "non-finite sample in {path} at frame {} channel {}",
                i / img.channels as usize,
                i % img.channels as usize
            )));
        }
        img.bits.push(bits);
    }
    Ok(img)
}

/// Either form, so the same tool works locally on renders and in CI on images.
fn load_any(path: &str) -> Result<Image, Reported> {
    let mut head = [0u8; 4];
    let mut file = File::open(path).map_err(|_| fail(format!("cannot open {path}")))?;
    let n = file.read(&mut head).unwrap_or(0);
    if n == 4 && &head == MAGIC {
        load_image(path)
    } else {
        load_audio(path)
    }
}

fn write_image(path: &str, img: &Image) -> Result<(), Reported> {
    let mut out = Vec::with_capacity(HEADER + img.bits.len() * 4);
    out.extend_from_slice(MAGIC);
    out.extend_from_slice(&VERSION.to_le_bytes());
    out.extend_from_slice(&img.channels.to_le_bytes());
    out.extend_from_slice(&img.sample_rate.to_le_bytes());
    out.extend_from_slice(&img.frames.to_le_bytes());
    for b in &img.bits {
        out.extend_from_slice(&b.to_le_bytes());
    }
    std::fs::write(path, out).map_err(|_| fail(format!("short write for {path}")))
}

/// A monotonic integer order of floats, so ULP distance is a subtraction across the sign
/// boundary (-0 and +0 are adjacent). Diagnostics only: the decision stays a bit comparison.
fn ordered_key(bits: u32) -> i64 {
    const SIGN: u32 = 0x8000_0000;
    i64::from(if bits & SIGN != 0 { !bits } else { bits | SIGN })
}

fn meta(label: &str, img: &Image) {
    println!(
        "{label} channels={} sample_rate={} frames={} samples={}",
        img.channels,
        img.sample_rate,
        img.frames,
        img.bits.len()
    );
}

fn extract(input: &str, output: &str) -> u8 {
    let Ok(img) = load_audio(input) else {
        return 2;
    };
    if write_image(output, &img).is_err() {
        return 2;
    }
    meta("extracted", &img);
    0
}

fn compare(a_path: &str, b_path: &str) -> u8 {
    let (Ok(a), Ok(b)) = (load_any(a_path), load_any(b_path)) else {
        return 2;
    };
    if a.channels != b.channels || a.sample_rate != b.sample_rate || a.frames != b.frames {
        println!("DIFFER: shape mismatch");
        meta("  a:", &a);
        meta("  b:", &b);
        return 1;
    }
    let (mut max_ulp, mut max_abs, mut diff_count, mut first) = (0i64, 0f64, 0usize, None);
    for (i, (&x, &y)) in a.bits.iter().zip(&b.bits).enumerate() {
        if x == y {
            continue;
        }
        diff_count += 1;
        first.get_or_insert(i);
        let (fx, fy) = (f64::from(f32::from_bits(x)), f64::from(f32::from_bits(y)));
        max_abs = max_abs.max((fx - fy).abs());
        max_ulp = max_ulp.max((ordered_key(x) - ordered_key(y)).abs());
    }
    let Some(first) = first else {
        println!(
            "IDENTICAL samples={} frames={} channels={}",
            a.bits.len(),
            a.frames,
            a.channels
        );
        return 0;
    };
    let channels = a.channels as usize;
    let (x, y) = (a.bits[first], b.bits[first]);
    println!("DIFFER: {diff_count} of {} samples", a.bits.len());
    println!(
        "  first: sample_index={first} frame={} channel={}",
        first / channels,
        first % channels
    );
    println!(
        "    a={} (bits=0x{x:x})",
        g(f64::from(f32::from_bits(x)), 6)
    );
    println!(
        "    b={} (bits=0x{y:x})",
        g(f64::from(f32::from_bits(y)), 6)
    );
    println!("  max_abs_error={} max_ulp={max_ulp}", g(max_abs, 6));
    println!(
        "  note: error metrics locate the failing stage; they do not relax the bit-equality gate."
    );
    1
}

fn usage() -> u8 {
    eprint!(
        "usage:\n  mr_adm_pcm_bits extract <audio-in> <bits-out>\n  mr_adm_pcm_bits validate <image>     \
         # validate a canonical .pcmbits image\n  mr_adm_pcm_bits compare <a> <b>        \
         # .pcmbits images or audio files\nexit: 0 identical / 1 differ / 2 usage or IO error\n"
    );
    2
}

fn main() -> ExitCode {
    let args: Vec<String> = std::env::args().collect();
    let code = match (args.get(1).map(String::as_str), args.len()) {
        (Some("validate"), 3) => {
            if load_image(&args[2]).is_ok() {
                0
            } else {
                2
            }
        }
        (Some("extract"), 4) => extract(&args[2], &args[3]),
        (Some("compare"), 4) => compare(&args[2], &args[3]),
        _ => usage(),
    };
    ExitCode::from(code)
}
