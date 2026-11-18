//! WAVE container passes over generated files (R3 F1, from the C++ `wav_container_test`,
//! `wav_profile_test`, `trim_file_smoke_test` and the post-processing parts of `core_smoke_test`,
//! `wav_io_test` and `flac_io_smoke_test`, which drove these passes through C++ wrappers that had
//! no production caller).
//!
//! The golden digests were recorded on 2026-11-14 while the frozen C++ implementation still gave
//! identical results for every case; each case runs the same Rust functions the wrappers called
//! (`mradm-wav` and the `postprocess` passes) and folds the error code and the resulting bytes
//! into one digest per operation. The C++ test also compared AXML reads with the SDK's
//! `io::get_axml`; that view is covered by the import tests and the C ABI query tests.
use mradm_render::{
    output,
    postprocess::{self, Begin, ChnaEntry, Layout, Profile, Progress},
};
use mradm_wav::{AMBI_HOA3, BextFields, Chna, ChunkReader, append_bext, bext_payload, has_chunk};
use std::{
    collections::BTreeMap,
    fs::{self, File, OpenOptions},
    io::BufReader,
    path::{Path, PathBuf},
};

/// FNV-1a over a framed record stream; a regression digest, not a security property.
struct Digest(u64);
impl Digest {
    const fn new() -> Self {
        Self(0xcbf2_9ce4_8422_2325)
    }
    fn bytes(&mut self, data: &[u8]) {
        self.number(data.len() as u64);
        for &byte in data {
            self.0 = (self.0 ^ u64::from(byte)).wrapping_mul(0x0000_0100_0000_01b3);
        }
    }
    fn text(&mut self, text: &str) {
        self.bytes(text.as_bytes());
    }
    fn number(&mut self, value: u64) {
        for shift in (0..64).step_by(8) {
            self.0 = (self.0 ^ ((value >> shift) & 0xFF)).wrapping_mul(0x0000_0100_0000_01b3);
        }
    }
}

#[derive(Default)]
struct Goldens {
    finalize: Option<Digest>,
    metadata: Option<Digest>,
    read: Option<Digest>,
    replace: Option<Digest>,
    scan: Option<Digest>,
    cases: usize,
}
fn digest(slot: &mut Option<Digest>) -> &mut Digest {
    slot.get_or_insert_with(Digest::new)
}

const FINALIZE_DIGEST: u64 = 0x229d_928e_3e6c_85e8;
const METADATA_DIGEST: u64 = 0xf3b1_b4ac_3939_711a;
const READ_DIGEST: u64 = 0x2937_fb9e_fa3b_be6d;
const REPLACE_DIGEST: u64 = 0x06e8_1fb9_b482_8b35;
const SCAN_DIGEST: u64 = 0x947c_5d73_ba37_d933;
const CASE_COUNT: usize = 320;

fn scratch(name: &str) -> PathBuf {
    let directory =
        std::env::temp_dir().join(format!("mradm-wav-container-{name}-{}", std::process::id()));
    let _ = fs::remove_dir_all(&directory);
    fs::create_dir_all(&directory).unwrap();
    directory
}
fn text(path: &Path) -> &str {
    path.to_str().unwrap()
}

fn put16(out: &mut Vec<u8>, value: u32) {
    out.extend_from_slice(&(value as u16).to_le_bytes());
}
fn put32(out: &mut Vec<u8>, value: u32) {
    out.extend_from_slice(&value.to_le_bytes());
}
fn put64(out: &mut Vec<u8>, value: u64) {
    out.extend_from_slice(&value.to_le_bytes());
}

fn fmt_payload(tag: u32, channels: u32, bits: u32, mask: Option<u32>) -> Vec<u8> {
    let align = channels * bits / 8;
    let mut fmt = Vec::new();
    put16(&mut fmt, if mask.is_some() { 0xFFFE } else { tag });
    put16(&mut fmt, channels);
    put32(&mut fmt, 48000);
    put32(&mut fmt, 48000 * align);
    put16(&mut fmt, align);
    put16(&mut fmt, bits);
    if let Some(mask) = mask {
        put16(&mut fmt, 22);
        put16(&mut fmt, bits);
        put32(&mut fmt, mask);
        put16(&mut fmt, tag);
        fmt.extend_from_slice(&[0, 0, 0, 0, 16, 0, 128, 0, 0, 170, 0, 56, 155, 113]);
    }
    fmt
}

fn samples(size: usize, mut seed: u32) -> Vec<u8> {
    (0..size)
        .map(|_| {
            seed = seed.wrapping_mul(1_664_525).wrapping_add(1_013_904_223);
            (seed >> 24) as u8
        })
        .collect()
}

/// RF64/BW64 get a ds64 first and a sentinel data size; `table` adds ds64 size-table entries.
fn wave(container: &str, chunks: &[(&[u8; 4], Vec<u8>)], table: u32) -> Vec<u8> {
    let wide = container != "RIFF";
    let mut body = Vec::new();
    let mut data_size = 0u64;
    for (id, payload) in chunks {
        body.extend_from_slice(*id);
        let sentinel = wide && *id == b"data";
        put32(
            &mut body,
            if sentinel {
                0xFFFF_FFFF
            } else {
                payload.len() as u32
            },
        );
        body.extend_from_slice(payload);
        if payload.len() & 1 != 0 {
            body.push(0);
        }
        if *id == b"data" {
            data_size = payload.len() as u64;
        }
    }
    let ds64_disk = if wide {
        8 + 28 + 12 * u64::from(table)
    } else {
        0
    };
    let riff_size = 4 + ds64_disk + body.len() as u64;
    let mut out = container.as_bytes().to_vec();
    put32(&mut out, if wide { 0xFFFF_FFFF } else { riff_size as u32 });
    out.extend_from_slice(b"WAVE");
    if wide {
        out.extend_from_slice(b"ds64");
        put32(&mut out, 28 + 12 * table);
        put64(&mut out, riff_size);
        put64(&mut out, data_size);
        put64(&mut out, 0);
        put32(&mut out, table);
        for _ in 0..table {
            out.extend_from_slice(b"JUNK");
            put64(&mut out, 2);
        }
    }
    out.extend_from_slice(&body);
    out
}

fn chna_payload(tracks: u32, uids: &[(u32, &str)], reserved: usize) -> Vec<u8> {
    let mut out = Vec::new();
    put16(&mut out, tracks);
    put16(&mut out, uids.len() as u32);
    for (track, uid) in uids {
        put16(&mut out, *track);
        let mut padded = uid.as_bytes().to_vec();
        padded.resize(12, b' ');
        out.extend_from_slice(&padded);
        out.extend_from_slice(b"AT_00010001_01");
        out.extend_from_slice(b"AP_00010002");
        out.push(0);
    }
    out.resize(out.len() + 40 * reserved, 0);
    out
}

fn axml_text(size: usize) -> String {
    let mut xml = String::from("<?xml version=\"1.0\"?><ebuCoreMain>");
    while xml.len() + 14 < size {
        xml += "<a/>";
    }
    xml + "</ebuCoreMain>"
}

struct Fixture {
    name: String,
    bytes: Vec<u8>,
    channels: u16,
    audio_supported: bool,
}
fn fixture(name: &str, bytes: Vec<u8>, channels: u16) -> Fixture {
    Fixture {
        name: name.into(),
        bytes,
        channels,
        audio_supported: true,
    }
}

fn fixtures() -> Vec<Fixture> {
    let data24 = samples(2 * 3 * 517, 1);
    let data16 = samples(2 * 333, 2);
    let data_f32 = samples(2 * 4 * 211, 3);
    let data24_mono_odd = samples(3 * 335, 4);
    let axml = axml_text(301).into_bytes();
    let chna = chna_payload(2, &[(1, "ATU_00000001"), (2, "atu_00000002")], 0);
    let mut out = vec![
        fixture(
            "riff-empty",
            wave(
                "RIFF",
                &[(b"fmt ", fmt_payload(1, 2, 24, None)), (b"data", vec![])],
                0,
            ),
            2,
        ),
        fixture(
            "riff-pcm24",
            wave(
                "RIFF",
                &[
                    (b"fmt ", fmt_payload(1, 2, 24, None)),
                    (b"data", data24.clone()),
                ],
                0,
            ),
            2,
        ),
        fixture(
            "riff-pcm16-junk-list",
            wave(
                "RIFF",
                &[
                    (b"JUNK", vec![0; 28]),
                    (b"fmt ", fmt_payload(1, 1, 16, None)),
                    (b"LIST", b"odd".to_vec()),
                    (b"data", data16),
                ],
                0,
            ),
            1,
        ),
        fixture(
            "riff-pcm24-odd-data",
            wave(
                "RIFF",
                &[
                    (b"fmt ", fmt_payload(1, 1, 24, None)),
                    (b"data", data24_mono_odd),
                ],
                0,
            ),
            1,
        ),
        fixture(
            "rf64-float-fact",
            wave(
                "RF64",
                &[
                    (b"fmt ", fmt_payload(3, 2, 32, None)),
                    (b"fact", vec![0; 4]),
                    (b"data", data_f32),
                ],
                0,
            ),
            2,
        ),
        fixture(
            "bw64-pcm24-adm",
            wave(
                "BW64",
                &[
                    (b"fmt ", fmt_payload(1, 2, 24, Some(0))),
                    (b"chna", chna),
                    (b"data", data24),
                    (b"axml", axml.clone()),
                ],
                0,
            ),
            2,
        ),
        fixture(
            "riff-pcm32-adm-ext",
            wave(
                "RIFF",
                &[
                    (b"fmt ", fmt_payload(1, 2, 32, Some(0x3))),
                    (
                        b"chna",
                        chna_payload(2, &[(1, "ATU_00000001"), (2, "ATU_00000002")], 2),
                    ),
                    (b"axml", axml_text(77).into_bytes()),
                    (b"data", samples(2 * 4 * 64, 5)),
                ],
                0,
            ),
            2,
        ),
        fixture(
            "riff-hoa3-ambi",
            wave(
                "RIFF",
                &[
                    (b"fmt ", fmt_payload(1, 16, 24, None)),
                    (b"ambi", vec![7; 16]),
                    (b"data", samples(16 * 3 * 40, 6)),
                ],
                0,
            ),
            16,
        ),
        fixture(
            "riff-hoa3",
            wave(
                "RIFF",
                &[
                    (b"fmt ", fmt_payload(3, 16, 32, None)),
                    (b"data", samples(16 * 4 * 40, 7)),
                ],
                0,
            ),
            16,
        ),
        fixture(
            "rf64-pcm16-table",
            wave(
                "RF64",
                &[
                    (b"fmt ", fmt_payload(1, 2, 16, None)),
                    (b"axml", axml.clone()),
                    (b"data", samples(2 * 2 * 50, 8)),
                ],
                1,
            ),
            2,
        ),
    ];
    let mono_chna = chna_payload(1, &[(1, "ATU_00000001")], 0);
    for container in ["RIFF", "RF64"] {
        out.push(Fixture {
            audio_supported: false,
            ..fixture(
                &format!("{container}-pcm8-metadata"),
                wave(
                    container,
                    &[
                        (b"fmt ", fmt_payload(1, 1, 8, None)),
                        (b"chna", mono_chna.clone()),
                        (b"data", samples(3, 9)),
                        (b"axml", axml.clone()),
                    ],
                    0,
                ),
                1,
            )
        });
        out.push(Fixture {
            audio_supported: false,
            ..fixture(
                &format!("{container}-float64-metadata"),
                wave(
                    container,
                    &[
                        (b"fmt ", fmt_payload(3, 1, 64, Some(0))),
                        (b"chna", mono_chna.clone()),
                        (b"data", samples(24, 10)),
                        (b"axml", axml.clone()),
                    ],
                    0,
                ),
                1,
            )
        });
    }
    out
}

/// Drives a started pass to completion and installs it, as the C++ `run_output_pass` did
/// without cancellation or a progress sink.
fn run(begin: postprocess::Result<Begin>) -> postprocess::Result<Progress> {
    match begin? {
        Begin::Skipped(progress) => Ok(progress),
        Begin::Started(mut pass, _) => {
            while !pass.step()?.complete {}
            pass.finish()
        }
    }
}
/// The C++ `ErrorCode` of a Rust code: 1–6 as they are, anything else `internal_error`.
fn code(code: i32) -> i32 {
    if (1..=6).contains(&code) { code } else { 6 }
}
fn finalize_code(path: &Path, layout: &Layout) -> i32 {
    run(postprocess::finalize(text(path), layout)).map_or_else(|f| code(f.code), |_| 0)
}

/// `MetadataFields`: encoder, UTC date, renderer, output layout, loudness, true peak.
#[derive(Default)]
struct Metadata {
    encoder: String,
    date_utc: String,
    renderer: String,
    output_layout: String,
    lufs: Option<f64>,
    peak_dbtp: Option<f64>,
}
fn metadata(
    encoder: &str,
    date_utc: &str,
    renderer: &str,
    output_layout: &str,
    lufs: Option<f64>,
    peak_dbtp: Option<f64>,
) -> Metadata {
    Metadata {
        encoder: encoder.into(),
        date_utc: date_utc.into(),
        renderer: renderer.into(),
        output_layout: output_layout.into(),
        lufs,
        peak_dbtp,
    }
}
/// `write_wav_metadata`: the island's bext (and HOA3 ambi) append.
fn write_metadata(path: &Path, meta: &Metadata) -> i32 {
    let description = if meta.renderer.is_empty() {
        String::new()
    } else {
        format!("renderer={} layout={}", meta.renderer, meta.output_layout)
    };
    let payload = bext_payload(&BextFields {
        description: description.as_bytes(),
        originator: meta.encoder.as_bytes(),
        originator_reference: meta.output_layout.as_bytes(),
        date_utc: meta.date_utc.as_bytes(),
        loudness: meta.lufs,
        true_peak: meta.peak_dbtp,
    });
    let result = (|| {
        let mut file = OpenOptions::new().read(true).write(true).open(path)?;
        append_bext(
            &mut file,
            &payload,
            (meta.output_layout == "hoa3").then_some(&AMBI_HOA3),
        )
    })();
    result.map_or_else(|e| code(e.code), |()| 0)
}

/// The AXML payload (empty when absent) and the import-side CHNA records.
type AdmChunks = (Vec<u8>, Vec<(u16, [u8; 12])>);
fn read_chunks(path: &Path) -> Result<AdmChunks, i32> {
    (|| {
        let mut reader = ChunkReader::new(BufReader::new(File::open(path)?))?;
        let mut axml = Vec::new();
        if let Some(chunk) = reader.chunks().iter().find(|c| c.id == *b"axml").copied() {
            axml.resize(chunk.size as usize, 0);
            if chunk.size != 0 {
                reader.read_chunk(*b"axml", &mut axml)?;
            }
        }
        let chna = match reader.metadata(*b"chna", u64::MAX)? {
            Some(data) => Chna::decode_import(&data)?
                .entries
                .iter()
                .map(|e| (e.track_index, e.uid))
                .collect(),
            None => Vec::new(),
        };
        Ok((axml, chna))
    })()
    .map_err(|e: mradm_wav::Error| code(e.code))
}

fn replace_code(source: &Path, target: &Path, axml: &[u8]) -> i32 {
    postprocess::replace_axml(text(source), text(target), axml)
        .map_or_else(|f| code(f.code), |()| 0)
}

fn layouts(channels: u16) -> Vec<(&'static str, Layout)> {
    let mut out = vec![
        ("identity", Layout::default()),
        (
            "prefer-riff",
            Layout {
                prefer_riff: true,
                ..Layout::default()
            },
        ),
        (
            "force-extensible-fact",
            Layout {
                force_extensible: true,
                include_pcm_fact: true,
                ..Layout::default()
            },
        ),
    ];
    if channels == 2 {
        let entry = |track_index, uid: &str, track_format: &str, pack_format: &str| ChnaEntry {
            track_index,
            uid: uid.into(),
            track_format: track_format.into(),
            pack_format: pack_format.into(),
        };
        let adm = Layout {
            axml: axml_text(1001).into_bytes(),
            chna: vec![
                entry(1, "ATU_00000001", "AT_00010001_01", "AP_00010002"),
                entry(2, "ATU_2", "AT_00010002_01", "AP_1"),
            ],
            ..Layout::default()
        };
        let mut adm_riff = adm.clone();
        adm_riff.prefer_riff = true;
        adm_riff.axml.push(b' '); // odd AXML length
        let mut bad_track = adm.clone();
        bad_track.chna[1].track_index = 3;
        let mut duplicate = adm.clone();
        duplicate.chna[1].track_index = 1;
        let mut wide_uid = adm.clone();
        wide_uid.chna[0].uid = b"ATU_000000001".to_vec();
        out.extend([
            (
                "mask-swap",
                Layout {
                    channel_mask: 0x3,
                    permutation: vec![1, 0],
                    ..Layout::default()
                },
            ),
            ("adm", adm),
            ("adm-odd-riff", adm_riff),
            // Error cases: the digest records their error codes.
            (
                "bad-mask",
                Layout {
                    channel_mask: 0x7,
                    ..Layout::default()
                },
            ),
            (
                "bad-permutation",
                Layout {
                    permutation: vec![0, 0],
                    ..Layout::default()
                },
            ),
            (
                "short-permutation",
                Layout {
                    permutation: vec![0],
                    ..Layout::default()
                },
            ),
            (
                "axml-without-chna",
                Layout {
                    axml: b"<x/>".to_vec(),
                    ..Layout::default()
                },
            ),
            ("chna-track-range", bad_track),
            ("chna-duplicate", duplicate),
            ("chna-width", wide_uid),
        ]);
    }
    if channels == 16 {
        out.push((
            "reverse-16",
            Layout {
                permutation: (0..16).rev().collect(),
                force_extensible: true,
                ..Layout::default()
            },
        ));
    }
    out
}

fn metadata_cases() -> Vec<(&'static str, Metadata)> {
    vec![
        (
            "full",
            metadata(
                "MacinRender ADM Core",
                "2026-05-22T14:30:07Z",
                "ear",
                "0+2+0",
                Some(-23.456),
                Some(-1.004),
            ),
        ),
        ("empty", Metadata::default()),
        (
            "date-only",
            metadata("enc", "2026-05-22", "", "5.1", None, Some(0.005)),
        ),
        (
            "negative-half",
            metadata(
                "e",
                "2026-05-22T00:00:00",
                "vbap",
                "x",
                Some(-0.005),
                Some(-24.125),
            ),
        ),
        (
            "long-fields",
            metadata(
                &"o".repeat(40),
                "2026-05-22T14:30:07Z",
                &"r".repeat(300),
                &"l".repeat(40),
                Some(12.0),
                None,
            ),
        ),
        (
            "hoa3",
            metadata(
                "enc",
                "2026-05-22T14:30:07Z",
                "hoa-encode",
                "hoa3",
                Some(-18.0),
                Some(-3.0),
            ),
        ),
    ]
}

fn record(
    goldens: &mut Goldens,
    which: fn(&mut Goldens) -> &mut Option<Digest>,
    label: &str,
    outcome: (i32, Vec<u8>),
) {
    let d = digest(which(goldens));
    d.text(label);
    d.number(i64::from(outcome.0) as u64);
    if outcome.0 == 0 {
        d.bytes(&outcome.1);
    }
    goldens.cases += 1;
}

fn normalize_uid(raw: &[u8; 12]) -> Vec<u8> {
    let mut uid = raw.to_vec();
    while matches!(uid.last(), Some(b' ' | 0)) {
        uid.pop();
    }
    uid.make_ascii_uppercase();
    uid
}

fn scan(goldens: &mut Goldens, path: &Path, label: &str) {
    let state = File::open(path)
        .map_err(mradm_wav::Error::from)
        .and_then(|file| has_chunk(BufReader::new(file), *b"axml"))
        .map_or(0, |present| if present { 2 } else { 1 });
    let d = digest(&mut goldens.scan);
    d.text(&format!("scan {label}"));
    d.number(state);
    goldens.cases += 1;
}

#[test]
fn container_passes_match_the_golden_digests() {
    let root = scratch("golden");
    let all = fixtures();
    let mut goldens = Goldens::default();
    let case = root.join("case.wav");
    for fixture in &all {
        if fixture.audio_supported {
            for (name, layout) in layouts(fixture.channels) {
                fs::write(&case, &fixture.bytes).unwrap();
                let result = finalize_code(&case, &layout);
                let label = format!("finalize {} {name}", fixture.name);
                record(
                    &mut goldens,
                    |g| &mut g.finalize,
                    &label,
                    (result, fs::read(&case).unwrap()),
                );
            }
        } else {
            // Metadata-only encodings are rejected for audio decoding.
            fs::write(&case, &fixture.bytes).unwrap();
            let opened = mradm_wav::Reader::new(BufReader::new(File::open(&case).unwrap()));
            assert_eq!(opened.err().map(|e| e.code), Some(2), "{}", fixture.name);
        }
        for (name, meta) in metadata_cases() {
            // Applied twice so a second bext and an existing ambi are covered too.
            fs::write(&case, &fixture.bytes).unwrap();
            let first = write_metadata(&case, &meta);
            let result = if first != 0 {
                first
            } else {
                write_metadata(&case, &meta)
            };
            let label = format!("metadata {} {name}", fixture.name);
            record(
                &mut goldens,
                |g| &mut g.metadata,
                &label,
                (result, fs::read(&case).unwrap()),
            );
        }

        let read = root.join("read.wav");
        fs::write(&read, &fixture.bytes).unwrap();
        let actual = read_chunks(&read);
        let d = digest(&mut goldens.read);
        d.text(&format!("read {}", fixture.name));
        d.number(i64::from(actual.as_ref().err().copied().unwrap_or(0)) as u64);
        if let Ok((axml, chna)) = &actual {
            d.bytes(axml);
            let mut uid_to_channel = BTreeMap::new();
            for (track, uid) in chna {
                assert_ne!(*track, 0, "read {}: unexpected track index 0", fixture.name);
                let uid = normalize_uid(uid);
                if !uid.is_empty() {
                    uid_to_channel.insert(uid, track - 1);
                }
            }
            d.number(uid_to_channel.len() as u64);
            for (uid, channel) in &uid_to_channel {
                d.bytes(uid);
                d.number(u64::from(*channel));
            }
        }
        goldens.cases += 1;

        for size in [0, 1, 2, 4097] {
            let source = root.join("source.wav");
            let output = root.join("output.wav");
            fs::write(&source, &fixture.bytes).unwrap();
            let result = replace_code(&source, &output, axml_text(size).as_bytes());
            let bytes = if result == 0 {
                fs::read(&output).unwrap()
            } else {
                Vec::new()
            };
            let label = format!("replace {} {size}", fixture.name);
            record(&mut goldens, |g| &mut g.replace, &label, (result, bytes));
            let _ = fs::remove_file(&output);
        }
    }

    // The routing state the channel-bed probe derives: 0 invalid WAVE, 1 no AXML, 2 AXML present.
    // Truncated tails and trailing garbage end the tolerant scan instead of failing it.
    let path = root.join("scan.wav");
    for fixture in &all {
        fs::write(&path, &fixture.bytes).unwrap();
        scan(&mut goldens, &path, &fixture.name);
        for cut in [1, 5, 9] {
            fs::write(&path, &fixture.bytes[..fixture.bytes.len() - cut]).unwrap();
            scan(&mut goldens, &path, &format!("{} cut {cut}", fixture.name));
        }
        let mut trailing = fixture.bytes.clone();
        trailing.extend_from_slice(b"axm");
        fs::write(&path, trailing).unwrap();
        scan(&mut goldens, &path, &format!("{} trailing", fixture.name));
    }
    let eight_bit = wave(
        "RIFF",
        &[
            (b"fmt ", fmt_payload(1, 1, 8, None)),
            (b"data", samples(9, 9)),
            (b"axml", b"x".to_vec()),
        ],
        0,
    );
    fs::write(&path, eight_bit).unwrap();
    scan(&mut goldens, &path, "8-bit with axml");
    fs::write(&path, b"RIFF\x04\0\0\0AIFF").unwrap();
    scan(&mut goldens, &path, "not wave");
    fs::write(&path, b"RIF").unwrap();
    scan(&mut goldens, &path, "short");
    scan(&mut goldens, &root.join("missing.wav"), "missing");

    let value = |slot: &Option<Digest>| slot.as_ref().map_or(0, |d| d.0);
    let actual = [
        ("finalize", value(&goldens.finalize), FINALIZE_DIGEST),
        ("metadata", value(&goldens.metadata), METADATA_DIGEST),
        ("read", value(&goldens.read), READ_DIGEST),
        ("replace", value(&goldens.replace), REPLACE_DIGEST),
        ("scan", value(&goldens.scan), SCAN_DIGEST),
    ];
    let report: Vec<_> = actual
        .iter()
        .map(|(name, got, want)| format!("{name} {got:#018x} (expected {want:#018x})"))
        .collect();
    assert!(
        goldens.cases == CASE_COUNT && actual.iter().all(|(_, got, want)| got == want),
        "{} cases (expected {CASE_COUNT}): {report:?}",
        goldens.cases
    );
    fs::remove_dir_all(&root).unwrap();
}

/// Behaviour that intentionally differed from the retired frozen implementation.
#[test]
fn rejected_edits_leave_the_file_unchanged() {
    let root = scratch("rejected");
    let path = root.join("rust-only.wav");
    // An invalid existing ambi is rejected before anything is written (the old code had already
    // appended bext).
    let bad_ambi = wave(
        "RIFF",
        &[
            (b"fmt ", fmt_payload(1, 16, 24, None)),
            (b"ambi", vec![0; 12]),
            (b"data", vec![0; 48]),
        ],
        0,
    );
    fs::write(&path, &bad_ambi).unwrap();
    let hoa = Metadata {
        output_layout: "hoa3".into(),
        ..Metadata::default()
    };
    assert_ne!(write_metadata(&path, &hoa), 0, "invalid ambi accepted");
    assert_eq!(
        fs::read(&path).unwrap(),
        bad_ambi,
        "invalid ambi left a partial bext"
    );
    // Bytes beyond the declared container end are no longer absorbed into it.
    let mut trailing = fixtures().remove(0).bytes;
    trailing.push(0);
    fs::write(&path, &trailing).unwrap();
    assert_ne!(
        write_metadata(&path, &Metadata::default()),
        0,
        "trailing garbage absorbed"
    );
    assert_eq!(fs::read(&path).unwrap(), trailing);
    // CHNA records may reserve unused space after numUIDs; track index 0 is reported for the
    // caller to reject.
    let reserved = wave(
        "RIFF",
        &[
            (b"fmt ", fmt_payload(1, 2, 16, None)),
            (b"chna", chna_payload(2, &[(0, "ATU_00000009")], 3)),
            (b"data", vec![0; 8]),
        ],
        0,
    );
    fs::write(&path, &reserved).unwrap();
    let (axml, chna) = read_chunks(&path).unwrap();
    assert!(axml.is_empty());
    assert_eq!(chna, [(0, *b"ATU_00000009")]);
    // A failed layout rewrite, and a pass abandoned after its start (the C++ progress sink that
    // threw), leave neither the original modified nor a temporary file behind.
    let source = fixtures().remove(0).bytes;
    fs::write(&path, &source).unwrap();
    let bad = Layout {
        channel_mask: 0x1,
        ..Layout::default()
    };
    assert_ne!(finalize_code(&path, &bad), 0, "bad mask accepted");
    let progress = root.join("progress.wav");
    fs::write(
        &progress,
        wave(
            "RIFF",
            &[
                (b"fmt ", fmt_payload(1, 2, 16, None)),
                (b"data", vec![0; 20000]),
            ],
            0,
        ),
    )
    .unwrap();
    let before = fs::read(&progress).unwrap();
    for _ in 0..3 {
        let Begin::Started(pass, first) =
            postprocess::finalize(text(&progress), &Layout::default()).unwrap()
        else {
            panic!("layout pass skipped");
        };
        assert!(first.emit, "the pass reports its start");
        drop(pass);
        assert_eq!(fs::read(&progress).unwrap(), before);
    }
    assert_eq!(
        fs::read(&path).unwrap(),
        source,
        "failed finalize modified the source"
    );
    let mut names: Vec<_> = fs::read_dir(&root)
        .unwrap()
        .map(|e| e.unwrap().file_name().into_string().unwrap())
        .collect();
    names.sort();
    assert_eq!(names, ["progress.wav", "rust-only.wav"]);
    fs::remove_dir_all(&root).unwrap();
}

#[test]
fn axml_replacement_handles_aliases_and_failures() {
    let root = scratch("alias");
    let source = root.join("alias-source.wav");
    let destination = root.join("alias-destination.wav");
    let format = fmt_payload(1, 1, 16, None);
    let audio = samples(20, 11);
    let original = wave(
        "RIFF",
        &[
            (b"fmt ", format.clone()),
            (b"data", audio.clone()),
            (b"axml", b"<old/>".to_vec()),
        ],
        0,
    );
    let expected = wave(
        "RIFF",
        &[
            (b"fmt ", format.clone()),
            (b"data", audio.clone()),
            (b"axml", b"<new/>!".to_vec()),
        ],
        0,
    );
    let new = b"<new/>!";
    fs::write(&source, &original).unwrap();
    assert_eq!(replace_code(&source, &source, new), 0);
    assert_eq!(
        fs::read(&source).unwrap(),
        expected,
        "in-place replacement lost data"
    );

    fs::write(&source, &original).unwrap();
    fs::write(&destination, b"existing output").unwrap();
    assert_eq!(replace_code(&source, &destination, new), 0);
    assert_eq!(fs::read(&destination).unwrap(), expected);
    assert_eq!(
        fs::read(&source).unwrap(),
        original,
        "replacing an output changed the source"
    );
    fs::remove_file(&destination).unwrap();
    fs::hard_link(&source, &destination).unwrap();
    assert_eq!(replace_code(&source, &destination, new), 0);
    assert_eq!(fs::read(&destination).unwrap(), expected);
    assert_eq!(
        fs::read(&source).unwrap(),
        original,
        "hard-link replacement lost source data"
    );
    fs::remove_file(&destination).unwrap();
    // Windows symlink creation requires privileges that the test runner may not have.
    #[cfg(unix)]
    {
        std::os::unix::fs::symlink(&source, &destination).unwrap();
        assert_eq!(replace_code(&source, &destination, new), 0);
        assert_eq!(fs::read(&destination).unwrap(), expected);
        assert_eq!(
            fs::read(&source).unwrap(),
            original,
            "symlink replacement lost source data"
        );
        fs::remove_file(&destination).unwrap();
    }

    let missing_axml = wave("RIFF", &[(b"fmt ", format), (b"data", audio)], 0);
    fs::write(&source, &missing_axml).unwrap();
    fs::write(&destination, &expected).unwrap();
    assert_ne!(
        replace_code(&source, &destination, new),
        0,
        "missing AXML accepted"
    );
    assert_eq!(fs::read(&source).unwrap(), missing_axml);
    assert_eq!(
        fs::read(&destination).unwrap(),
        expected,
        "failed rewrite changed the output"
    );
    assert_ne!(
        replace_code(&source, &source, new),
        0,
        "missing AXML accepted in place"
    );
    assert_eq!(fs::read(&source).unwrap(), missing_axml);

    let directory = root.join("directory.wav");
    fs::create_dir(&directory).unwrap();
    fs::write(directory.join("keep"), b"keep").unwrap();
    fs::write(&source, &original).unwrap();
    assert_ne!(
        replace_code(&source, &directory, new),
        0,
        "directory destination accepted"
    );
    assert_eq!(fs::read(directory.join("keep")).unwrap(), b"keep");
    assert_eq!(fs::read(&source).unwrap(), original);
    let leftovers = fs::read_dir(&root).unwrap().any(|e| {
        e.unwrap()
            .file_name()
            .to_string_lossy()
            .contains(".axml_tmp.")
    });
    assert!(!leftovers, "AXML rewrite left a temporary file behind");
    fs::remove_dir_all(&root).unwrap();
}

/// A float RF64 file of `channels`; `value(index)` gives each interleaved sample.
fn float_wav(path: &Path, channels: u32, frames: usize, value: impl Fn(usize) -> f32) {
    let mut writer = output::Writer::create(path, channels, 48000, None).unwrap();
    let samples: Vec<f32> = (0..frames * channels as usize).map(value).collect();
    writer.write(&samples).unwrap();
    writer.finish().unwrap();
}
fn read_all(path: &Path) -> (mradm_wav::Info, Vec<f32>) {
    let mut reader = mradm_wav::Reader::new(BufReader::new(File::open(path).unwrap())).unwrap();
    let info = reader.info();
    let mut samples = vec![0.0; info.frames as usize * usize::from(info.format.channels)];
    assert_eq!(reader.read_frames(&mut samples).unwrap(), info.frames);
    (info, samples)
}

/// One parsed WAVE file: container id, fmt payload, data payload, fact frames, ADM chunks.
#[derive(Default)]
struct Wave {
    container: Vec<u8>,
    format: Vec<u8>,
    pcm: Vec<u8>,
    fact_frames: u32,
    axml: bool,
    chna: bool,
}
fn le32(bytes: &[u8], offset: usize) -> u32 {
    u32::from_le_bytes(bytes[offset..offset + 4].try_into().unwrap())
}
fn parse(path: &Path) -> Wave {
    let bytes = fs::read(path).unwrap();
    let mut result = Wave {
        container: bytes[..4].to_vec(),
        ..Wave::default()
    };
    let mut offset = 12;
    let mut data_size = 0u64;
    while offset + 8 <= bytes.len() {
        let id = &bytes[offset..offset + 4];
        let mut size = u64::from(le32(&bytes, offset + 4));
        offset += 8;
        if id == b"ds64" {
            data_size =
                u64::from(le32(&bytes, offset + 8)) | (u64::from(le32(&bytes, offset + 12)) << 32);
        } else if id == b"data" && size == 0xFFFF_FFFF {
            size = data_size;
        }
        let payload = bytes[offset..offset + size as usize].to_vec();
        match id {
            b"fmt " => result.format = payload,
            b"data" => result.pcm = payload,
            b"fact" => result.fact_frames = le32(&payload, 0),
            _ => {}
        }
        result.axml |= id == b"axml";
        result.chna |= id == b"chna";
        offset += (size + (size & 1)) as usize;
    }
    result
}

/// The speaker re-render profile writes small RIFF WAVEFORMATEXTENSIBLE with a fact chunk and
/// no rendered ADM; 7.1.4 keeps the mask and the side/rear swap, 9.1.6 its native order without a
/// mask. The ADM semantics profile keeps the rendered ADM and the PCM.
#[test]
fn wav_output_profiles_keep_pcm_and_write_their_headers() {
    let root = scratch("profile");
    const FRAMES: usize = 32;
    for bits in [24u16, 32] {
        for (layout, profile) in [
            ("7.1.4", Profile::SpeakerRerender),
            ("9.1.6", Profile::SpeakerRerender),
            ("9.1.6", Profile::AdmSemantics),
        ] {
            let label = format!("{layout} {profile:?} {bits}-bit");
            let path = root.join("profile.wav");
            let channels: u32 = if layout == "7.1.4" { 12 } else { 16 };
            float_wav(&path, channels, FRAMES, |i| i as f32 / 2048.0);
            if bits == 24 {
                run(postprocess::bitdepth(text(&path), 24)).unwrap();
            }
            let before = parse(&path);
            run(postprocess::finalize_rendered(
                text(&path),
                layout,
                profile,
                false,
            ))
            .unwrap();
            let after = parse(&path);
            if profile == Profile::AdmSemantics {
                assert!(
                    after.axml && after.chna && after.pcm == before.pcm,
                    "{label}"
                );
                continue;
            }
            assert_eq!(after.container, b"RIFF", "{label}");
            assert!(!after.axml && !after.chna, "{label}: no rendered-ADM track");
            assert_eq!(after.format.len(), 40, "{label}: WAVEFORMATEXTENSIBLE");
            assert_eq!(le32(&after.format, 0), (channels << 16) | 0xFFFE, "{label}");
            assert_eq!(after.fact_frames, FRAMES as u32, "{label}");
            assert_eq!(
                le32(&after.format, 24),
                if bits == 32 { 3 } else { 1 },
                "{label}: subformat"
            );
            assert_eq!(
                le32(&after.format, 20),
                if channels == 12 { 0x2D63F } else { 0 },
                "{label}: channel mask"
            );
            if channels == 16 {
                assert_eq!(after.pcm, before.pcm, "{label}: native order");
                continue;
            }
            const SOURCES: [usize; 12] = [0, 1, 2, 3, 6, 7, 4, 5, 8, 9, 10, 11];
            let width = usize::from(bits / 8);
            for frame in 0..FRAMES {
                for (channel, source) in SOURCES.iter().enumerate() {
                    let at = |c: usize| ((frame * 12 + c) * width)..((frame * 12 + c + 1) * width);
                    assert_eq!(
                        after.pcm[at(channel)],
                        before.pcm[at(*source)],
                        "{label}: 7.1.4 order"
                    );
                }
            }
        }
    }
    fs::remove_dir_all(&root).unwrap();
}

/// Channel 0 of frame `i` holds `i`, so a trimmed range is identified by its first and last
/// samples.
#[test]
fn trim_keeps_the_selected_frames() {
    let root = scratch("trim");
    let path = root.join("ramp.wav");
    float_wav(
        &path,
        4,
        100,
        |i| if i % 4 == 0 { (i / 4) as f32 } else { 0.0 },
    );
    run(postprocess::trim(text(&path), 25, 50, b"")).unwrap();
    let (info, samples) = read_all(&path);
    assert_eq!(info.frames, 50);
    assert_eq!((samples[0], samples[49 * 4]), (25.0, 74.0));

    float_wav(&path, 1, 50, |i| i as f32);
    let error = run(postprocess::trim(text(&path), 50, 10, b"")).unwrap_err();
    assert_eq!(error.code, 1, "a start past the end is invalid_argument");

    float_wav(&path, 2, 64, |i| i as f32);
    let before = fs::read(&path).unwrap();
    assert_eq!(
        run(postprocess::trim(text(&path), 0, 64, b""))
            .unwrap()
            .message,
        "trim skipped"
    );
    assert_eq!(
        fs::read(&path).unwrap(),
        before,
        "a whole-file trim leaves the file"
    );
    fs::remove_dir_all(&root).unwrap();
}

/// Gain rewrites keep CAF (with a stale fixed-name sidecar left alone) and refuse FLAC.
#[test]
fn gain_rewrites_caf_and_refuses_flac() {
    let root = scratch("gain");
    let path = root.join("gain.caf");
    let mut writer = output::Writer::create(&path, 2, 48000, Some(b"0+2+0")).unwrap();
    writer
        .write(&[0.0, 0.0, 0.25, -0.25, 0.5, -0.5, 0.75, -0.75])
        .unwrap();
    writer.finish().unwrap();
    run(postprocess::gain(text(&path), 2.0, b"0+2+0", false)).unwrap();
    let stale = root.join("gain.gain_tmp.caf");
    fs::write(&stale, b"stale").unwrap();
    run(postprocess::gain(text(&path), 0.5, b"0+2+0", false)).unwrap();
    assert_eq!(
        fs::read(&stale).unwrap(),
        b"stale",
        "the legacy fixed sidecar name is not reused"
    );
    assert_eq!(&fs::read(&path).unwrap()[..4], b"caff");
    let mut reader = mradm_wav::caf::Reader::open(&path).unwrap();
    let mut samples = [0.0; 8];
    assert_eq!(reader.read(&mut samples).unwrap(), 4);
    assert_eq!((samples[2], samples[7]), (0.25, -0.75));

    // A WAVE file named .flac: the extension alone refuses the rewrite.
    let flac = root.join("gain.flac");
    float_wav(&flac, 1, 16, |_| 0.5);
    let before = fs::read(&flac).unwrap();
    let error = run(postprocess::gain(text(&flac), 0.5, b"", false)).unwrap_err();
    assert_eq!(error.code, 2, "gain on FLAC is unsupported");
    assert_eq!(fs::read(&flac).unwrap(), before);
    fs::remove_dir_all(&root).unwrap();
}

/// Integer conversion at 48/96/192 kHz: full scale clips instead of wrapping, the reader stops at
/// the end after a 64-bit seek, and a NaN sample fails without replacing the original.
#[test]
fn integer_conversion_clips_and_rejects_nan() {
    let root = scratch("bitdepth");
    let input = [-2.0f32, -1.0, -0.5, 0.0, 0.5, 1.0, 2.0];
    for rate in [48000u32, 96000, 192000] {
        for bits in [16u16, 24, 32] {
            let path = root.join(format!("{rate}-{bits}.wav"));
            let mut writer = output::Writer::create(&path, 1, rate, None).unwrap();
            writer.write(&input).unwrap();
            writer.finish().unwrap();
            run(postprocess::bitdepth(text(&path), bits)).unwrap();
            let mut reader =
                mradm_wav::Reader::new(BufReader::new(File::open(&path).unwrap())).unwrap();
            let info = reader.info();
            assert_eq!(
                (info.format.sample_rate, info.format.sample_format.bits()),
                (rate, bits)
            );
            assert!(info.format.sample_format.is_integer());
            let mut output = [0.0; 7];
            assert_eq!(reader.read_frames(&mut output).unwrap(), 7);
            assert!(
                output[0] < -0.99 && output[6] > 0.99,
                "{rate}/{bits}: full scale wrapped"
            );
            reader.seek_frame(u64::MAX).unwrap();
            assert_eq!(reader.read_frames(&mut output).unwrap(), 0);
        }
    }
    let nan = root.join("nan.wav");
    float_wav(&nan, 1, 2, |i| if i == 0 { 0.5 } else { f32::NAN });
    let original = fs::read(&nan).unwrap();
    let error = run(postprocess::bitdepth(text(&nan), 24)).unwrap_err();
    assert_eq!(error.code, 1, "NaN is invalid_argument");
    assert_eq!(
        fs::read(&nan).unwrap(),
        original,
        "failed conversion replaced the original"
    );
    let leaked = fs::read_dir(&root).unwrap().any(|e| {
        e.unwrap()
            .file_name()
            .to_string_lossy()
            .contains("bitdepth_tmp")
    });
    assert!(!leaked, "temporary output leaked");
    fs::remove_dir_all(&root).unwrap();
}
