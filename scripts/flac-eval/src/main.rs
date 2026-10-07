//! Selection probe only: reports upstream behaviour without repairing errors or gaps.
use std::error::Error;
use std::fs::File;
use std::io::{BufWriter, Write};
use symphonia::core::codecs::audio::AudioDecoderOptions;
use symphonia::core::formats::{SeekMode, SeekTo, TrackType, probe::Hint};
use symphonia::core::io::MediaSourceStream;
use symphonia::core::units::Timestamp;

type Result<T> = std::result::Result<T, Box<dyn Error>>;

// Each sample is a left-aligned signed i32 followed by its f32 bits, both LE.
fn sample(out: &mut impl Write, integer: i32, float: f32) -> Result<()> {
    out.write_all(&integer.to_le_bytes())?;
    out.write_all(&float.to_le_bytes())?;
    Ok(())
}

fn symphonia(input: &str, out: &mut impl Write, seek: Option<u64>) -> Result<()> {
    let mss = MediaSourceStream::new(Box::new(File::open(input)?), Default::default());
    let mut format = symphonia::default::get_probe().probe(
        &Hint::new(),
        mss,
        Default::default(),
        Default::default(),
    )?;
    let track = format
        .default_track(TrackType::Audio)
        .ok_or("no audio track")?;
    let params = track
        .codec_params
        .as_ref()
        .and_then(|p| p.audio())
        .ok_or("no audio params")?;
    let channels = params.channels.as_ref().ok_or("no channels")?.count();
    println!(
        "channels={channels}\nrate={}\nbits={}\nframes_hint={}",
        params.sample_rate.unwrap_or(0),
        params.bits_per_sample.unwrap_or(0),
        track
            .num_frames
            .map_or_else(|| "unknown".into(), |n| n.to_string())
    );
    let mut options = AudioDecoderOptions::default();
    options.verify = seek.is_none();
    let mut decoder = symphonia::default::get_codecs().make_audio_decoder(params, &options)?;
    let id = track.id;
    let mut expected_ts = 0_i64;
    if let Some(target) = seek {
        let result = format.seek(
            SeekMode::Accurate,
            SeekTo::Timestamp {
                ts: Timestamp::new(i64::try_from(target)?),
                track_id: id,
            },
        )?;
        println!("seek_actual={}", result.actual_ts.get());
        expected_ts = result.actual_ts.get();
        decoder.reset();
    }
    let mut integers = Vec::<i32>::new();
    let mut floats = Vec::<f32>::new();
    let mut gaps = 0;
    let mut frames = 0;
    let mut status = "eof".to_string();
    loop {
        let packet = match format.next_packet() {
            Ok(Some(packet)) => packet,
            Ok(None) => break,
            Err(e) => {
                status = format!("demux_error: {e}");
                break;
            }
        };
        if packet.track_id != id {
            continue;
        }
        let ts = packet.pts.get();
        if ts != expected_ts {
            gaps += 1;
        }
        let audio = match decoder.decode(&packet) {
            Ok(audio) => audio,
            Err(e) => {
                status = format!("decode_error: {e}");
                break;
            }
        };
        integers.resize(audio.samples_interleaved(), 0);
        floats.resize(audio.samples_interleaved(), 0.0);
        audio.copy_to_slice_interleaved(&mut integers);
        audio.copy_to_slice_interleaved(&mut floats);
        let decoded_frames = integers.len() / channels;
        expected_ts = ts + decoded_frames as i64;
        // Accurate seek returns the containing packet. Discard the preceding samples explicitly.
        let skip = seek
            .map_or(0, |target| target.saturating_sub(ts.max(0) as u64))
            .min(decoded_frames as u64) as usize;
        for (&i, &f) in integers[skip * channels..]
            .iter()
            .zip(&floats[skip * channels..])
        {
            sample(out, i, f)?;
        }
        frames += decoded_frames - skip;
    }
    println!("frames_read={frames}\ngaps={gaps}\nstatus={status}");
    println!("md5={:?}", decoder.finalize().verify_ok);
    Ok(())
}

fn claxon(input: &str, out: &mut impl Write, seek: Option<u64>) -> Result<()> {
    if seek.is_some() {
        return Err("Claxon has no native seek API".into());
    }
    let mut reader = claxon::FlacReader::open(input)?;
    let info = reader.streaminfo();
    println!(
        "channels={}\nrate={}\nbits={}\nframes_hint={}",
        info.channels,
        info.sample_rate,
        info.bits_per_sample,
        info.samples
            .map_or_else(|| "unknown".into(), |n| n.to_string())
    );
    let mut count = 0_u64;
    let mut status = "eof".to_string();
    for value in reader.samples() {
        match value {
            Ok(value) => {
                let left_aligned = ((value as u32) << (32 - info.bits_per_sample)) as i32;
                sample(
                    out,
                    left_aligned,
                    left_aligned as f32 * (1.0 / 2147483648.0),
                )?;
                count += 1;
            }
            Err(e) => {
                status = format!("decode_error: {e}");
                break;
            }
        }
    }
    println!(
        "frames_read={}\nstatus={status}",
        count / u64::from(info.channels)
    );
    Ok(())
}

fn run() -> Result<()> {
    let args: Vec<String> = std::env::args().collect();
    if !(4..=5).contains(&args.len()) {
        return Err("decoder input output [seek_frame]".into());
    }
    let mut out = BufWriter::new(File::create(&args[3])?);
    let seek = args.get(4).map(|s| s.parse::<u64>()).transpose()?;
    let result = match args[1].as_str() {
        "symphonia" => symphonia(&args[2], &mut out, seek),
        "claxon" => claxon(&args[2], &mut out, seek),
        _ => Err("unknown decoder".into()),
    };
    out.flush()?;
    result
}

fn main() {
    if let Err(error) = run() {
        println!("status=error: {error}");
    }
}
