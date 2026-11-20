//! Opus in Matroska (PCM consistency 7.2 B): the final float32 WAVE encoded by libopus's surround
//! multistream encoder into a hand-written EBML / Matroska file, and its Tags inserted before the
//! first Cluster, with the elements, byte layout, order and errors of the C++ island they replace
//! (`opus_mka_io.cpp`). libopus is built by the islands' CMake project (`mradm-islands-sys`).
use crate::metadata_temp::MetadataTemp;
use mradm_engine::fmt::fixed;
use mradm_engine::render::MetadataFields;
use mradm_engine::request::Error;
use mradm_wav::Reader;
use std::collections::hash_map::RandomState;
use std::ffi::{CStr, c_char, c_int};
use std::fs::File;
use std::hash::{BuildHasher, Hasher};
use std::io::{self, BufReader, BufWriter, Read, Seek, SeekFrom, Write};
use std::path::Path;

const INVALID_ARGUMENT: i32 = 1;
const UNSUPPORTED: i32 = 2;
const IO_ERROR: i32 = 3;
const CANCELLED: i32 = 5;

const RATE: u32 = 48000;
/// 20 ms at 48 kHz.
const FRAME: usize = 960;
const BLOCK: u64 = 4096;
/// A new cluster when a frame starts more than this many milliseconds after the cluster.
const CLUSTER_MS: u64 = 5000;
const APP: &str = "MacinRender ADM Core";

// EBML header and Matroska element IDs.
const EBML: u32 = 0x1A45_DFA3;
const EBML_VERSION: u32 = 0x4286;
const EBML_READ_VERSION: u32 = 0x42F7;
const EBML_MAX_ID_LEN: u32 = 0x42F2;
const EBML_MAX_SIZE_LEN: u32 = 0x42F3;
const EBML_DOC_TYPE: u32 = 0x4282;
const EBML_DOC_TYPE_VERSION: u32 = 0x4287;
const EBML_DOC_TYPE_READ_VERSION: u32 = 0x4285;
const SEGMENT: u32 = 0x1853_8067;
const INFO: u32 = 0x1549_A966;
const TRACKS: u32 = 0x1654_AE6B;
const CLUSTER: u32 = 0x1F43_B675;
const TAGS: u32 = 0x1254_C367;
const TIMESTAMP_SCALE: u32 = 0x2A_D7B1;
const DURATION: u32 = 0x4489;
const MUXING_APP: u32 = 0x4D80;
const WRITING_APP: u32 = 0x5741;
const TRACK_ENTRY: u32 = 0xAE;
const TRACK_NUMBER: u32 = 0xD7;
const TRACK_UID: u32 = 0x73C5;
const TRACK_TYPE: u32 = 0x83;
const FLAG_ENABLED: u32 = 0xB9;
const FLAG_DEFAULT: u32 = 0x88;
const FLAG_FORCED: u32 = 0x55AA;
const CODEC_ID: u32 = 0x86;
const CODEC_PRIVATE: u32 = 0x63A2;
const CODEC_DELAY: u32 = 0x56AA;
const SEEK_PRE_ROLL: u32 = 0x56BB;
const DEFAULT_DURATION: u32 = 0x23_E383;
const AUDIO: u32 = 0xE1;
const SAMPLING_FREQUENCY: u32 = 0xB5;
const CHANNELS: u32 = 0x9F;
const TIMESTAMP: u32 = 0xE7;
const SIMPLE_BLOCK: u32 = 0xA3;
const TAG: u32 = 0x7373;
const TARGETS: u32 = 0x63C0;
const SIMPLE_TAG: u32 = 0x67C8;
const TAG_NAME: u32 = 0x45A3;
const TAG_STRING: u32 = 0x4487;

// libopus (opus_multistream.h, opus_defines.h).
const OPUS_OK: c_int = 0;
const OPUS_APPLICATION_AUDIO: c_int = 2049;
const OPUS_SET_BITRATE: c_int = 4002;
const OPUS_SET_VBR: c_int = 4006;
const OPUS_SET_VBR_CONSTRAINT: c_int = 4020;
const OPUS_GET_LOOKAHEAD: c_int = 4027;

type MsEncoder = std::ffi::c_void;
unsafe extern "C" {
    fn opus_multistream_surround_encoder_create(
        rate: i32,
        channels: c_int,
        family: c_int,
        streams: *mut c_int,
        coupled: *mut c_int,
        mapping: *mut u8,
        application: c_int,
        error: *mut c_int,
    ) -> *mut MsEncoder;
    fn opus_multistream_encoder_destroy(encoder: *mut MsEncoder);
    fn opus_multistream_encoder_ctl(encoder: *mut MsEncoder, request: c_int, ...) -> c_int;
    fn opus_multistream_encode_float(
        encoder: *mut MsEncoder,
        pcm: *const f32,
        frame_size: c_int,
        data: *mut u8,
        max_data_bytes: i32,
    ) -> i32;
    fn opus_strerror(error: c_int) -> *const c_char;
}

fn strerror(code: c_int) -> String {
    let text = unsafe { opus_strerror(code) };
    if text.is_null() {
        return String::new();
    }
    unsafe { CStr::from_ptr(text) }
        .to_string_lossy()
        .into_owned()
}

fn failure(code: i32, message: impl Into<String>, context: String) -> Error {
    Error::new(code, message, context)
}

fn at(path: &str) -> String {
    format!("path={path}")
}

// ── EBML ────────────────────────────────────────────────────────────────────────

fn id(b: &mut Vec<u8>, id: u32) {
    let bytes = id.to_be_bytes();
    let skip = match id {
        0..=0xFF => 3,
        0x100..=0xFFFF => 2,
        0x1_0000..=0xFF_FFFF => 1,
        _ => 0,
    };
    b.extend_from_slice(&bytes[skip..]);
}

/// The size vint, with the C++ muxer's thresholds (a value equal to a width's all-ones marker
/// takes the next width).
fn vint(b: &mut Vec<u8>, v: u64) {
    let (width, marker): (usize, u64) = if v < 0x7F {
        (1, 0x80)
    } else if v < 0x3FFF {
        (2, 0x40 << 8)
    } else if v < 0x1F_FFFF {
        (3, 0x20 << 16)
    } else if v < 0x0FFF_FFFF {
        (4, 0x10 << 24)
    } else if v < 0x07_FFFF_FFFF {
        (5, 0x08 << 32)
    } else {
        b.push(0x01);
        b.extend_from_slice(&v.to_be_bytes());
        return;
    };
    b.extend_from_slice(&(v | marker).to_be_bytes()[8 - width..]);
}

fn unknown_size(b: &mut Vec<u8>) {
    b.extend_from_slice(&[0x01, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF]);
}

fn uint_width(v: u64) -> usize {
    if v == 0 {
        1
    } else {
        8 - (v.leading_zeros() as usize / 8)
    }
}

fn elem(b: &mut Vec<u8>, element: u32, content: &[u8]) {
    id(b, element);
    vint(b, content.len() as u64);
    b.extend_from_slice(content);
}

fn elem_uint(b: &mut Vec<u8>, element: u32, value: u64) {
    elem(b, element, &value.to_be_bytes()[8 - uint_width(value)..]);
}

fn elem_f64(b: &mut Vec<u8>, element: u32, value: f64) {
    elem(b, element, &value.to_bits().to_be_bytes());
}

fn elem_str(b: &mut Vec<u8>, element: u32, s: &str) {
    elem(b, element, s.as_bytes());
}

// ── Mapping ─────────────────────────────────────────────────────────────────────

fn is_square(channels: u32) -> bool {
    let root = f64::from(channels).sqrt() as u32;
    root * root == channels
}

/// The Opus channel mapping family (`opus_mapping_family`).
fn mapping_family(channels: u32, layout: &str) -> c_int {
    if layout.starts_with("hoa") && is_square(channels) {
        return 2;
    }
    if channels <= 2 {
        return 0;
    }
    if standard_surround(layout, channels) || (layout.is_empty() && channels <= 8) {
        return 1;
    }
    255
}

fn standard_surround(layout: &str, channels: u32) -> bool {
    matches!(
        (layout, channels),
        ("0+5+0" | "5.1", 6) | ("wav71" | "7.1", 8)
    )
}

/// The WAVE source channel of each Opus family 1 (Vorbis order) channel, or `None` to keep the
/// order (`opus_vorbis_source_order`).
fn vorbis_order(layout: &str, channels: u32) -> Option<&'static [usize]> {
    match (layout, channels) {
        // L R C LFE Ls Rs → L C R Ls Rs LFE.
        ("0+5+0" | "5.1", 6) => Some(&[0, 2, 1, 4, 5, 3]),
        // L R C LFE Rls Rrs Ls Rs → L C R Ls Rs Rls Rrs LFE.
        ("wav71" | "7.1", 8) => Some(&[0, 2, 1, 6, 7, 4, 5, 3]),
        _ => None,
    }
}

fn opus_head(
    channels: u32,
    streams: c_int,
    coupled: c_int,
    mapping: &[u8],
    preskip: u16,
    family: u8,
) -> Vec<u8> {
    let mut h = b"OpusHead".to_vec();
    h.push(1);
    h.push(channels as u8);
    h.extend_from_slice(&preskip.to_le_bytes());
    h.extend_from_slice(&RATE.to_le_bytes());
    h.extend_from_slice(&[0, 0]);
    h.push(family);
    if family != 0 {
        h.push(streams as u8);
        h.push(coupled as u8);
        h.extend_from_slice(&mapping[..channels as usize]);
    }
    h
}

/// A random 64-bit track UID (the C++ muxer drew it from `std::random_device`).
fn track_uid() -> u64 {
    RandomState::new().build_hasher().finish()
}

// ── Writer ──────────────────────────────────────────────────────────────────────

/// An open Opus MKA file (`FloatOpusMkaWriter`). Dropping it closes it as the C++ destructor did:
/// the last partial frame padded with silence and encoded, the Duration patched.
struct Writer {
    file: Option<BufWriter<File>>,
    encoder: *mut MsEncoder,
    channels: usize,
    duration_at: u64,
    cluster_ms: u64,
    cluster_open: bool,
    frames: u64,
    pcm: Vec<f32>,
    packet: Vec<u8>,
    failure: Option<String>,
    path: String,
}

impl Writer {
    fn open(
        path: &str,
        channels: u32,
        rate: u32,
        bitrate_per_ch: u32,
        layout: &str,
    ) -> Result<Self, Error> {
        if rate != RATE {
            return Err(failure(
                UNSUPPORTED,
                format!("Opus MKA requires 48000 Hz input, got {rate} Hz"),
                at(path),
            ));
        }
        if !(1..=255).contains(&channels) {
            return Err(failure(
                UNSUPPORTED,
                format!("Opus supports 1-255 channels, got {channels}"),
                at(path),
            ));
        }
        let family = mapping_family(channels, layout);
        let (mut streams, mut coupled, mut error) = (0, 0, 0);
        let mut mapping = [0u8; 255];
        let encoder = unsafe {
            opus_multistream_surround_encoder_create(
                RATE as i32,
                channels as c_int,
                family,
                &mut streams,
                &mut coupled,
                mapping.as_mut_ptr(),
                OPUS_APPLICATION_AUDIO,
                &mut error,
            )
        };
        let mut w = Self {
            file: None,
            encoder,
            channels: channels as usize,
            duration_at: 0,
            cluster_ms: 0,
            cluster_open: false,
            frames: 0,
            pcm: Vec::with_capacity(FRAME * channels as usize),
            packet: Vec::new(),
            failure: None,
            path: path.into(),
        };
        if encoder.is_null() || error != OPUS_OK {
            return Err(failure(
                IO_ERROR,
                format!(
                    "opus_multistream_surround_encoder_create failed: {}",
                    strerror(error)
                ),
                at(path),
            ));
        }
        if bitrate_per_ch > 0 && !(6..=320).contains(&bitrate_per_ch) {
            return Err(failure(
                INVALID_ARGUMENT,
                format!("opus_bitrate_per_ch_kbps must be 0 (auto) or 6-320, got {bitrate_per_ch}"),
                at(path),
            ));
        }
        let total = if bitrate_per_ch > 0 {
            u64::from(bitrate_per_ch) * u64::from(channels) * 1000
        } else if channels <= 2 {
            128_000
        } else {
            u64::from(channels) * 64_000
        };
        let ctl = |request: c_int, value: i32, name: &str| -> Result<(), Error> {
            let r = unsafe { opus_multistream_encoder_ctl(encoder, request, value) };
            if r == OPUS_OK {
                Ok(())
            } else {
                Err(failure(
                    IO_ERROR,
                    format!("{name} failed: {}", strerror(r)),
                    at(path),
                ))
            }
        };
        ctl(OPUS_SET_VBR, 1, "OPUS_SET_VBR")?;
        ctl(OPUS_SET_VBR_CONSTRAINT, 0, "OPUS_SET_VBR_CONSTRAINT")?;
        ctl(OPUS_SET_BITRATE, total as i32, "OPUS_SET_BITRATE")?;
        let mut preskip = 0i32;
        let r = unsafe {
            opus_multistream_encoder_ctl(encoder, OPUS_GET_LOOKAHEAD, &mut preskip as *mut i32)
        };
        if r != OPUS_OK {
            return Err(failure(
                IO_ERROR,
                format!("OPUS_GET_LOOKAHEAD failed: {}", strerror(r)),
                at(path),
            ));
        }
        w.packet = vec![0; streams as usize * 1275];
        let head = opus_head(
            channels,
            streams,
            coupled,
            &mapping,
            preskip as u16,
            family as u8,
        );

        let mut out = Vec::new();
        let mut hdr = Vec::new();
        elem_uint(&mut hdr, EBML_VERSION, 1);
        elem_uint(&mut hdr, EBML_READ_VERSION, 1);
        elem_uint(&mut hdr, EBML_MAX_ID_LEN, 4);
        elem_uint(&mut hdr, EBML_MAX_SIZE_LEN, 8);
        elem_str(&mut hdr, EBML_DOC_TYPE, "matroska");
        elem_uint(&mut hdr, EBML_DOC_TYPE_VERSION, 4);
        elem_uint(&mut hdr, EBML_DOC_TYPE_READ_VERSION, 2);
        elem(&mut out, EBML, &hdr);
        id(&mut out, SEGMENT);
        unknown_size(&mut out);

        let mut info = Vec::new();
        elem_uint(&mut info, TIMESTAMP_SCALE, 1_000_000);
        // Duration at a fixed layout (2-byte ID, 1-byte size, 8-byte value), patched on close.
        id(&mut info, DURATION);
        vint(&mut info, 8);
        info.extend_from_slice(&0f64.to_bits().to_be_bytes());
        elem_str(&mut info, MUXING_APP, APP);
        elem_str(&mut info, WRITING_APP, APP);
        let size_bytes = match info.len() as u64 {
            n if n < 0x7F => 1,
            n if n < 0x3FFF => 2,
            n if n < 0x1F_FFFF => 3,
            _ => 4,
        };
        // The Info ID (4 bytes), its size, then TimestampScale (7 bytes) and the Duration header (3).
        w.duration_at = out.len() as u64 + 4 + size_bytes + 10;
        elem(&mut out, INFO, &info);

        let mut entry = Vec::new();
        elem_uint(&mut entry, TRACK_NUMBER, 1);
        elem_uint(&mut entry, TRACK_UID, track_uid());
        elem_uint(&mut entry, TRACK_TYPE, 2);
        elem_uint(&mut entry, FLAG_ENABLED, 1);
        elem_uint(&mut entry, FLAG_DEFAULT, 1);
        elem_uint(&mut entry, FLAG_FORCED, 0);
        elem_str(&mut entry, CODEC_ID, "A_OPUS");
        elem(&mut entry, CODEC_PRIVATE, &head);
        elem_uint(
            &mut entry,
            CODEC_DELAY,
            preskip as u64 * 1_000_000_000 / u64::from(RATE),
        );
        elem_uint(&mut entry, SEEK_PRE_ROLL, 80_000_000);
        elem_uint(&mut entry, DEFAULT_DURATION, 20_000_000);
        let mut audio = Vec::new();
        elem_f64(&mut audio, SAMPLING_FREQUENCY, 48000.);
        elem_uint(&mut audio, CHANNELS, u64::from(channels));
        elem(&mut entry, AUDIO, &audio);
        let mut tracks = Vec::new();
        elem(&mut tracks, TRACK_ENTRY, &entry);
        elem(&mut out, TRACKS, &tracks);

        let Ok(file) = File::create(path) else {
            return Err(failure(
                IO_ERROR,
                "failed to open MKA file for writing",
                at(path),
            ));
        };
        let mut file = BufWriter::new(file);
        if file.write_all(&out).is_err() {
            return Err(failure(IO_ERROR, "failed to write MKA header", at(path)));
        }
        w.file = Some(file);
        if !w.open_cluster(0) {
            return Err(match w.close() {
                Err(e) => e,
                Ok(()) => failure(IO_ERROR, "failed to write initial MKA cluster", at(path)),
            });
        }
        Ok(w)
    }

    fn fail(&mut self, message: &str) -> bool {
        self.failure = Some(message.into());
        false
    }

    fn put(&mut self, bytes: &[u8], message: &str) -> bool {
        let ok = self
            .file
            .as_mut()
            .is_some_and(|f| f.write_all(bytes).is_ok());
        ok || self.fail(message)
    }

    fn open_cluster(&mut self, ms: u64) -> bool {
        self.cluster_ms = ms;
        self.cluster_open = true;
        let mut hdr = Vec::new();
        id(&mut hdr, CLUSTER);
        unknown_size(&mut hdr);
        if !self.put(&hdr, "failed to write MKA cluster header") {
            return false;
        }
        let mut t = Vec::new();
        elem_uint(&mut t, TIMESTAMP, ms);
        self.put(&t, "failed to write MKA cluster timestamp")
    }

    /// Encodes the full frame in `pcm` starting at `ms` into a SimpleBlock.
    fn encode(&mut self, ms: u64) -> bool {
        if self.failure.is_some() {
            return false;
        }
        if (!self.cluster_open || ms.wrapping_sub(self.cluster_ms) > CLUSTER_MS)
            && !self.open_cluster(ms)
        {
            return false;
        }
        let ret = unsafe {
            opus_multistream_encode_float(
                self.encoder,
                self.pcm.as_ptr(),
                FRAME as c_int,
                self.packet.as_mut_ptr(),
                self.packet.len() as i32,
            )
        };
        if ret <= 0 {
            let message = format!("opus_multistream_encode_float failed: {}", strerror(ret));
            return self.fail(&message);
        }
        let rel = (ms as i64 - self.cluster_ms as i64) as i16 as u16;
        let mut block = vec![0x81];
        block.extend_from_slice(&rel.to_be_bytes());
        block.push(0x80);
        block.extend_from_slice(&self.packet[..ret as usize]);
        let mut e = Vec::new();
        elem(&mut e, SIMPLE_BLOCK, &block);
        if !self.put(&e, "failed to write MKA SimpleBlock") {
            return false;
        }
        self.pcm.clear();
        true
    }

    fn frame_ms(&self) -> u64 {
        self.frames * FRAME as u64 * 1000 / u64::from(RATE)
    }

    /// Consumes `frames` interleaved frames; the number consumed (short after an encoder failure).
    fn write(&mut self, mut samples: &[f32], frames: u64) -> u64 {
        if self.failure.is_some() {
            return 0;
        }
        let mut consumed = 0;
        while consumed < frames {
            let needed = FRAME * self.channels - self.pcm.len();
            let available = (frames - consumed) as usize * self.channels;
            let take = needed.min(available);
            self.pcm.extend_from_slice(&samples[..take]);
            samples = &samples[take..];
            consumed += (take / self.channels) as u64;
            if self.pcm.len() == FRAME * self.channels {
                let ms = self.frame_ms();
                if !self.encode(ms) {
                    return consumed;
                }
                self.frames += 1;
            }
        }
        frames
    }

    fn close(&mut self) -> Result<(), Error> {
        if self.encoder.is_null() && self.file.is_none() {
            return match &self.failure {
                Some(m) => Err(failure(IO_ERROR, m.clone(), at(&self.path))),
                None => Ok(()),
            };
        }
        if !self.pcm.is_empty() && self.file.is_some() {
            self.pcm.resize(FRAME * self.channels, 0.);
            let ms = self.frame_ms();
            if self.encode(ms) {
                self.frames += 1;
            }
        }
        if let Some(mut file) = self.file.take() {
            let duration = self.frames as f64 * (FRAME as f64 / 48000.) * 1000.;
            let patched = file.seek(SeekFrom::Start(self.duration_at)).is_ok()
                && file.write_all(&duration.to_bits().to_be_bytes()).is_ok();
            if !patched {
                self.failure = Some("failed to patch MKA duration".into());
            }
            // Flushing the buffer is what `fclose` reported; the file closes when dropped.
            let closed = file.into_inner().is_ok();
            if !closed && self.failure.is_none() {
                self.failure = Some("failed to close MKA file".into());
            }
        }
        if !self.encoder.is_null() {
            unsafe { opus_multistream_encoder_destroy(self.encoder) };
            self.encoder = std::ptr::null_mut();
        }
        match &self.failure {
            Some(m) => Err(failure(IO_ERROR, m.clone(), at(&self.path))),
            None => Ok(()),
        }
    }
}

impl Drop for Writer {
    fn drop(&mut self) {
        let _ = self.close();
    }
}

/// Encodes the fully post-processed 48 kHz float32 WAVE `src` to Opus MKA `dst`
/// (`convert_to_opus_mka`); `layout` selects the mapping family and the Vorbis channel order.
/// `cancelled` is polled before each 4096-frame block.
pub(crate) fn encode(
    src: &str,
    dst: &str,
    layout: &str,
    bitrate_per_ch: u32,
    cancelled: &dyn Fn() -> bool,
) -> Result<(), Error> {
    if cancelled() {
        return Err(failure(CANCELLED, "render cancelled", at(dst)));
    }
    let mut reader = File::open(src)
        .map_err(mradm_wav::Error::from)
        .and_then(|f| Reader::new(BufReader::new(f)))
        .map_err(|e| failure(wav_code(e.code), e.message, at(src)))?;
    let info = reader.info();
    let (channels, rate) = (u32::from(info.format.channels), info.format.sample_rate);
    if rate != RATE {
        return Err(failure(
            UNSUPPORTED,
            format!("Opus MKA requires 48000 Hz source, got {rate} Hz"),
            format!("src={src}"),
        ));
    }
    let mut writer = Writer::open(dst, channels, rate, bitrate_per_ch, layout)?;
    let n = channels as usize;
    let mut pcm = vec![0f32; n * BLOCK as usize];
    let order = vorbis_order(layout, channels);
    let mut reordered = vec![0f32; if order.is_some() { pcm.len() } else { 0 }];
    let mut left = info.frames;
    while left > 0 {
        if cancelled() {
            return Err(failure(CANCELLED, "render cancelled", at(dst)));
        }
        let want = BLOCK.min(left);
        // A failed read ends the loop, as the C++ reader's 0 did.
        let got = reader
            .read_frames(&mut pcm[..want as usize * n])
            .unwrap_or(0);
        if got == 0 {
            break;
        }
        let block = match order {
            Some(order) => {
                for frame in 0..got as usize {
                    let base = frame * n;
                    for (to, &from) in order.iter().enumerate() {
                        reordered[base + to] = pcm[base + from];
                    }
                }
                &reordered[..]
            }
            None => &pcm[..],
        };
        if writer.write(block, got) != got {
            return Err(failure(
                IO_ERROR,
                "short write in convert_to_opus_mka",
                at(dst),
            ));
        }
        left -= got;
    }
    if left != 0 {
        return Err(failure(
            IO_ERROR,
            "short read in convert_to_opus_mka",
            format!("src={src}"),
        ));
    }
    writer.close()
}

fn wav_code(code: i32) -> i32 {
    if (1..=6).contains(&code) { code } else { 6 }
}

// ── Tags ────────────────────────────────────────────────────────────────────────

/// The Tags element of `m` (`write_mka_metadata`).
fn tags(m: &MetadataFields) -> Vec<u8> {
    let mut content = Vec::new();
    elem(&mut content, TARGETS, &[]);
    let mut simple = |name: &str, value: &str| {
        let mut st = Vec::new();
        elem_str(&mut st, TAG_NAME, name);
        elem_str(&mut st, TAG_STRING, value);
        elem(&mut content, SIMPLE_TAG, &st);
    };
    if !m.encoder.is_empty() {
        simple("ENCODER", &m.encoder);
    }
    if !m.date_utc.is_empty() {
        simple("DATE_RECORDED", &m.date_utc);
    }
    if !m.renderer.is_empty() {
        simple("RENDERER", &m.renderer);
    }
    if !m.output_layout.is_empty() {
        simple("OUTPUT_LAYOUT", &m.output_layout);
    }
    if let Some(l) = m.lufs {
        simple("INTEGRATED_LOUDNESS", &fixed(l, 1, false));
    }
    if let Some(p) = m.peak_dbtp {
        simple("TRUE_PEAK", &fixed(p, 2, false));
    }
    let mut tag = Vec::new();
    elem(&mut tag, TAG, &content);
    let mut out = Vec::new();
    elem(&mut out, TAGS, &tag);
    out
}

/// Inserts the Tags before the first (unknown-size) Cluster of the MKA file at `path`: tags after
/// an unknown-size Cluster are commonly ignored by Matroska parsers.
pub(crate) fn write_metadata(path: &str, m: &MetadataFields) -> Result<(), Error> {
    let mut source = File::open(path)
        .map_err(|_| failure(IO_ERROR, "cannot open MKA for metadata read", at(path)))?;
    let read_error = |_| failure(IO_ERROR, "failed to read MKA for metadata update", at(path));
    let cluster = first_cluster(&mut source)
        .map_err(read_error)?
        .ok_or_else(|| {
            failure(
                IO_ERROR,
                "cannot locate MKA Cluster for metadata insertion",
                at(path),
            )
        })?;
    source.rewind().map_err(read_error)?;

    let output = Path::new(path);
    let (temporary, mut dest) = MetadataTemp::create(output)
        .map_err(|_| failure(IO_ERROR, "cannot open MKA for metadata rewrite", at(path)))?;
    let rewrite = (|| -> io::Result<()> {
        if io::copy(&mut (&mut source).take(cluster), &mut dest)? != cluster {
            return Err(io::ErrorKind::UnexpectedEof.into());
        }
        dest.write_all(&tags(m))?;
        io::copy(&mut source, &mut dest)?;
        Ok(())
    })();
    drop(dest);
    drop(source);
    let write_error = |_| failure(IO_ERROR, "failed to rewrite MKA metadata", at(path));
    rewrite.map_err(write_error)?;
    temporary.install(output).map_err(write_error)
}

fn first_cluster(source: &mut impl Read) -> io::Result<Option<u64>> {
    let mut needle = Vec::new();
    id(&mut needle, CLUSTER);
    unknown_size(&mut needle);
    let mut buffer = [0; 8192];
    let mut offset = 0;
    let mut keep = 0;
    loop {
        let count = match source.read(&mut buffer[keep..]) {
            Ok(0) => return Ok(None),
            Ok(count) => count,
            Err(error) if error.kind() == io::ErrorKind::Interrupted => continue,
            Err(error) => return Err(error),
        };
        let end = keep + count;
        if let Some(index) = buffer[..end]
            .windows(needle.len())
            .position(|w| w == needle)
        {
            return Ok(Some(offset + index as u64));
        }
        // Retain enough of this block to match a Cluster marker split across two reads.
        keep = end.min(needle.len() - 1);
        buffer.copy_within(end - keep..end, 0);
        offset += (end - keep) as u64;
    }
}

/// Whether `path` names a Matroska audio file (the extension, ASCII case-insensitive).
pub(crate) fn is_mka(path: &str) -> bool {
    Path::new(path)
        .extension()
        .is_some_and(|e| e.eq_ignore_ascii_case("mka"))
}

#[cfg(test)]
mod tests {
    use super::*;
    use std::cell::Cell;
    use std::path::PathBuf;

    struct Dir(PathBuf);
    impl Dir {
        fn new(name: &str) -> Self {
            let path = std::env::temp_dir()
                .join(format!("mradm-mka-\u{a9}-{name}-{}", std::process::id()));
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

    fn source(path: &str, channels: u32, rate: u32, frames: usize) {
        let pcm: Vec<f32> = (0..frames * channels as usize)
            .map(|i| ((i * 37 % 200) as f32 - 100.) / 256.)
            .collect();
        let mut w = mradm_wav::create_file_writer(Path::new(path), channels, rate, 32, true, false)
            .unwrap();
        w.write_frames(&pcm).unwrap();
        w.finish().unwrap();
    }

    fn find(data: &[u8], needle: &[u8]) -> Option<usize> {
        data.windows(needle.len()).position(|w| w == needle)
    }

    /// Encodes `channels` × `frames` with `layout` and returns the file.
    fn encoded(dir: &Dir, name: &str, channels: u32, frames: usize, layout: &str) -> Vec<u8> {
        let (src, dst) = (
            dir.file(&format!("{name}.wav")),
            dir.file(&format!("{name}.mka")),
        );
        source(&src, channels, 48000, frames);
        encode(&src, &dst, layout, 0, &|| false).unwrap();
        std::fs::read(&dst).unwrap()
    }

    /// The OpusHead's family, stream and coupled counts and mapping.
    fn head(data: &[u8]) -> (u8, u8, u8, Vec<u8>) {
        let at = find(data, b"OpusHead").unwrap();
        let channels = data[at + 9] as usize;
        let family = data[at + 18];
        if family == 0 {
            return (0, 0, 0, Vec::new());
        }
        (
            family,
            data[at + 19],
            data[at + 20],
            data[at + 21..at + 21 + channels].to_vec(),
        )
    }

    fn duration_ms(data: &[u8]) -> f64 {
        let at = find(data, &[0x44, 0x89, 0x88]).unwrap() + 3;
        f64::from_bits(u64::from_be_bytes(data[at..at + 8].try_into().unwrap()))
    }

    fn clusters(data: &[u8]) -> usize {
        let mut needle = Vec::new();
        id(&mut needle, CLUSTER);
        unknown_size(&mut needle);
        data.windows(needle.len()).filter(|w| *w == needle).count()
    }

    #[test]
    fn the_header_carries_the_mapping_family_and_the_patched_duration() {
        let dir = Dir::new("heads");
        let stereo = encoded(&dir, "stereo", 2, 4800, "0+2+0");
        assert_eq!(&stereo[..4], &[0x1A, 0x45, 0xDF, 0xA3]);
        assert_eq!(head(&stereo).0, 0);
        assert_eq!(duration_ms(&stereo), 100.);
        // A partial last frame is padded with silence and counted.
        assert_eq!(duration_ms(&encoded(&dir, "partial", 2, 1000, "")), 40.);
        assert_eq!(
            head(&encoded(&dir, "wav71", 8, 4800, "wav71")),
            (1, 5, 3, vec![0, 6, 1, 2, 3, 4, 5, 7])
        );
        assert_eq!(
            head(&encoded(&dir, "hoa3", 16, 4800, "hoa3")),
            (2, 16, 0, (0..16).collect::<Vec<u8>>())
        );
        let wide = encoded(&dir, "wide", 24, 4800, "3+7+0");
        assert_eq!(head(&wide).0, 255);
        assert!(wide.len() > 1000);
    }

    #[test]
    fn a_new_cluster_starts_after_five_seconds() {
        let dir = Dir::new("clusters");
        // Frames at 0, 5020 and 10040 ms open the clusters of a twelve-second file.
        let long = encoded(&dir, "long", 1, 48000 * 12, "");
        assert_eq!(clusters(&long), 3);
        assert_eq!(duration_ms(&long), 12000.);
    }

    #[test]
    fn the_tags_go_before_the_first_cluster() {
        let dir = Dir::new("tags");
        let (src, dst) = (dir.file("in.wav"), dir.file("out.mka"));
        source(&src, 2, 48000, 4800);
        encode(&src, &dst, "0+2+0", 0, &|| false).unwrap();
        let m = MetadataFields {
            encoder: "TestEncoder".into(),
            date_utc: "2026-11-19T00:00:00Z".into(),
            renderer: "vbap-saf".into(),
            output_layout: "0+2+0".into(),
            lufs: Some(-23.04),
            peak_dbtp: Some(-0.004),
        };
        let original = std::fs::read(&dst).unwrap();
        write_metadata(&dst, &m).unwrap();
        let data = std::fs::read(&dst).unwrap();
        let tags_at = find(&data, &[0x12, 0x54, 0xC3, 0x67]).unwrap();
        let mut needle = Vec::new();
        id(&mut needle, CLUSTER);
        unknown_size(&mut needle);
        let cluster_at = find(&data, &needle).unwrap();
        let original_cluster = find(&original, &needle).unwrap();
        assert_eq!(tags_at, original_cluster);
        assert!(tags_at < cluster_at);
        assert_eq!(data[..tags_at], original[..original_cluster]);
        assert_eq!(data[cluster_at..], original[original_cluster..]);
        for text in [
            "ENCODER",
            "TestEncoder",
            "DATE_RECORDED",
            "RENDERER",
            "OUTPUT_LAYOUT",
            "0+2+0",
        ] {
            assert!(find(&data, text.as_bytes()).is_some(), "{text}");
        }
        assert!(find(&data, b"INTEGRATED_LOUDNESS").is_some() && find(&data, b"-23.0").is_some());
        assert!(
            find(&data, b"-0.00").is_some(),
            "a negative zero keeps its sign"
        );
        std::fs::write(&dst, b"no clusters").unwrap();
        let e = write_metadata(&dst, &m).unwrap_err();
        assert_eq!(
            e.message,
            "cannot locate MKA Cluster for metadata insertion"
        );
        assert_eq!(std::fs::read(&dst).unwrap(), b"no clusters");
        assert_eq!(std::fs::read_dir(&dir.0).unwrap().count(), 2);
    }

    #[test]
    fn finds_a_cluster_marker_split_across_reads() {
        let mut bytes = vec![0; 8192 - 5];
        let offset = bytes.len() as u64;
        id(&mut bytes, CLUSTER);
        unknown_size(&mut bytes);
        assert_eq!(first_cluster(&mut bytes.as_slice()).unwrap(), Some(offset));
    }

    #[test]
    fn the_rate_the_bitrate_and_cancellation_are_checked() {
        let dir = Dir::new("checks");
        let src = dir.file("44k.wav");
        source(&src, 2, 44100, 100);
        let e = encode(&src, &dir.file("44k.mka"), "", 0, &|| false).unwrap_err();
        assert_eq!(
            (e.code, e.message.as_str(), e.context),
            (
                UNSUPPORTED,
                "Opus MKA requires 48000 Hz source, got 44100 Hz",
                format!("src={src}")
            )
        );
        let src = dir.file("in.wav");
        source(&src, 2, 48000, 48000 * 2);
        let e = encode(&src, &dir.file("rate.mka"), "", 5, &|| false).unwrap_err();
        assert_eq!(e.code, INVALID_ARGUMENT);

        let before = dir.file("before.mka");
        let e = encode(&src, &before, "", 0, &|| true).unwrap_err();
        assert_eq!((e.code, e.context), (CANCELLED, format!("path={before}")));
        assert!(
            !Path::new(&before).exists(),
            "a pre-cancelled encoder leaves no output"
        );
        assert!(Path::new(&src).exists());

        let polls = Cell::new(0);
        let during = dir.file("during.mka");
        let e = encode(&src, &during, "", 0, &|| {
            polls.set(polls.get() + 1);
            polls.get() > 3
        })
        .unwrap_err();
        assert_eq!(e.code, CANCELLED);
        assert_eq!(polls.get(), 4, "polled before each block");
    }

    #[test]
    fn element_sizes_follow_the_muxer_thresholds() {
        let v = |n: u64| {
            let mut b = Vec::new();
            vint(&mut b, n);
            b
        };
        assert_eq!(v(0x7E), [0xFE]);
        assert_eq!(v(0x7F), [0x40, 0x7F]);
        assert_eq!(v(0x3FFF), [0x20, 0x3F, 0xFF]);
        assert_eq!(
            v(0x07_FFFF_FFFF),
            [1, 0, 0, 0, 0x07, 0xFF, 0xFF, 0xFF, 0xFF]
        );
        assert_eq!(uint_width(0), 1);
        assert_eq!(uint_width(0x100), 2);
        let mut b = Vec::new();
        id(&mut b, TIMESTAMP_SCALE);
        assert_eq!(b, [0x2A, 0xD7, 0xB1]);
        assert!(is_mka("x/Y.MKA") && !is_mka("x/y.mkv"));
    }
}
