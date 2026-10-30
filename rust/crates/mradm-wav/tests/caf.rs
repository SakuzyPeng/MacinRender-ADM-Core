//! caf-v1: the CAF writer, reader and `info` append replayed against records of the C++
//! `caf_io.cpp` (main `4207627`, before PCM consistency 4.8 B moved CAF to Rust). The generator
//! was a scratchpad program, not committed: `cases.jsonl` holds the inputs, `expected.jsonl`
//! the C++ results (nlohmann `dump()`), `samples/` complete files for some cases. Error
//! contexts are `path=` plus the path, added by the C++ wrapper.
use mradm_json::{Map, Value};
use mradm_wav::caf::{Reader, Writer, append_info};
use std::path::{Path, PathBuf};

const SPECIALS: [u32; 10] = [
    0x7fc00000, 0xffc00001, 0x7f800000, 0xff800000, 0x80000000, 0x00000000, 0x00000001, 0x7f7fffff,
    0x3f800000, 0x7fa00005,
];

fn splitmix(mut z: u64) -> u64 {
    z = z.wrapping_add(0x9e3779b97f4a7c15);
    z = (z ^ (z >> 30)).wrapping_mul(0xbf58476d1ce4e5b9);
    z = (z ^ (z >> 27)).wrapping_mul(0x94d049bb133111eb);
    z ^ (z >> 31)
}
fn get<'a>(value: &'a Value, key: &str) -> &'a Value {
    &value.as_object().unwrap()[key]
}
fn u(value: &Value, key: &str) -> u64 {
    get(value, key).as_f64().unwrap() as u64
}
fn s<'a>(value: &'a Value, key: &str) -> &'a str {
    get(value, key).as_str().unwrap()
}
fn signal(case: &Value) -> Vec<f32> {
    let seed = u(case, "seed");
    (0..u(case, "frames") * u(case, "channels"))
        .map(|i| {
            f32::from_bits(if s(case, "signal") == "bits" {
                splitmix((seed << 32).wrapping_add(i)) as u32
            } else {
                SPECIALS[((i + seed) % 10) as usize]
            })
        })
        .collect()
}
fn fnv(bytes: &[u8], mut hash: u64) -> u64 {
    for byte in bytes {
        hash = (hash ^ u64::from(*byte)).wrapping_mul(0x100000001b3);
    }
    hash
}
const FNV: u64 = 0xcbf29ce484222325;
fn object(entries: Vec<(&str, Value)>) -> Value {
    Value::Object(
        entries
            .into_iter()
            .map(|(k, v)| (k.to_owned(), v))
            .collect::<Map>(),
    )
}
fn error(error: &mradm_wav::Error, path: &Path, directory: &Path) -> Value {
    let name = path.strip_prefix(directory).unwrap().to_str().unwrap();
    object(vec![
        ("code", Value::Unsigned(error.code as u64)),
        ("message", error.message.as_str().into()),
        ("context", format!("path=<dir>/{name}").into()),
    ])
}
fn file(path: &Path) -> Value {
    let bytes = std::fs::read(path).unwrap();
    object(vec![
        ("len", Value::Unsigned(bytes.len() as u64)),
        ("fnv", format!("{:016x}", fnv(&bytes, FNV)).into()),
    ])
}
fn read_back(path: &Path, directory: &Path) -> Value {
    let mut record = Map::new();
    let mut chunks = Map::new();
    for size in [1u64, 3, 4096] {
        let mut reader = match Reader::open(path) {
            Ok(reader) => reader,
            Err(e) => return object(vec![("error", error(&e, path, directory))]),
        };
        record.insert("channels".into(), Value::Unsigned(reader.channels().into()));
        record.insert("rate".into(), Value::Unsigned(reader.sample_rate().into()));
        record.insert("frames".into(), Value::Unsigned(reader.frame_count()));
        if reader.channels() > 64 {
            record.insert("chunks".into(), Value::Null);
            return Value::Object(record);
        }
        let channels = reader.channels() as usize;
        let mut buffer = vec![0f32; size as usize * channels];
        let (mut total, mut samples, mut counts) = (0u64, FNV, FNV);
        for _ in 0..100_000 {
            let got = reader.read(&mut buffer).unwrap();
            counts = fnv(&got.to_le_bytes(), counts);
            if got == 0 {
                break;
            }
            for sample in &buffer[..got as usize * channels] {
                samples = fnv(&sample.to_bits().to_le_bytes(), samples);
            }
            total += got;
        }
        chunks.insert(
            size.to_string(),
            object(vec![
                ("total", Value::Unsigned(total)),
                ("counts", format!("{counts:016x}").into()),
                ("fnv", format!("{samples:016x}").into()),
            ]),
        );
    }
    record.insert("chunks".into(), Value::Object(chunks));
    Value::Object(record)
}
/// Null on success, otherwise the error (or the first short write) as in the C++ record.
fn write_case(case: &Value, path: &Path, directory: &Path) -> Value {
    let channels = u(case, "channels");
    let mut writer = match Writer::create(
        path,
        channels as u32,
        u(case, "rate") as u32,
        s(case, "layout").as_bytes(),
    ) {
        Ok(writer) => writer,
        Err(e) => return error(&e, path, directory),
    };
    let samples = signal(case);
    let frames = u(case, "frames");
    let chunk = match u(case, "chunk") {
        0 => frames.max(1),
        chunk => chunk,
    };
    let mut at = 0;
    while at < frames {
        let n = chunk.min(frames - at);
        let block = &samples[(at * channels) as usize..((at + n) * channels) as usize];
        if writer.write_frames(block).unwrap() != n {
            return object(vec![("short", Value::Unsigned(at))]);
        }
        at += n;
    }
    writer.finish().unwrap();
    Value::Null
}
fn write_hex(path: &Path, hex: &str) {
    let bytes: Vec<u8> = (0..hex.len())
        .step_by(2)
        .map(|i| u8::from_str_radix(&hex[i..i + 2], 16).unwrap())
        .collect();
    std::fs::write(path, bytes).unwrap();
}
fn root() -> PathBuf {
    Path::new(env!("CARGO_MANIFEST_DIR")).join("tests/caf-v1")
}

#[test]
fn caf_v1_matches_the_cpp_writer_reader_and_info_records() {
    let directory = std::env::temp_dir().join(format!("mradm-caf-v1-{}", std::process::id()));
    std::fs::create_dir_all(&directory).unwrap();
    let cases = std::fs::read_to_string(root().join("cases.jsonl")).unwrap();
    let expected = std::fs::read_to_string(root().join("expected.jsonl")).unwrap();
    let mut compared = 0;
    for (line, want) in cases.lines().zip(expected.lines()) {
        let case = mradm_json::parse(line.as_bytes()).unwrap();
        let id = u(&case, "id");
        let path = directory.join(format!("case-{id}.caf"));
        let _ = std::fs::remove_file(&path);
        let mut record = Map::new();
        record.insert("id".into(), Value::Unsigned(id));
        match s(&case, "kind") {
            "write" => {
                let open = write_case(&case, &path, &directory);
                let ok = open == Value::Null;
                record.insert("open".into(), open);
                record.insert("exists".into(), Value::Bool(path.exists()));
                if ok {
                    record.insert("file".into(), file(&path));
                    record.insert("read".into(), read_back(&path, &directory));
                    let sample = root().join(format!("samples/write-{id}.caf"));
                    if sample.exists() {
                        assert_eq!(
                            std::fs::read(&path).unwrap(),
                            std::fs::read(sample).unwrap()
                        );
                    }
                }
            }
            kind @ ("info" | "info-raw" | "info-missing") => {
                let want = mradm_json::parse(want.as_bytes()).unwrap();
                let strings = get(&want, "strings");
                if kind == "info" {
                    let base = write_case(get(&case, "base"), &path, &directory);
                    if base != Value::Null {
                        record.insert("base".into(), base);
                    }
                } else if kind == "info-raw" {
                    write_hex(&path, s(&case, "hex"));
                }
                let rounds = match case.as_object().unwrap().get("twice") {
                    Some(Value::Bool(true)) => 2,
                    _ => 1,
                };
                let mut results = Vec::new();
                for _ in 0..rounds {
                    results.push(
                        match append_info(
                            &path,
                            s(strings, "encoder").as_bytes(),
                            s(strings, "date").as_bytes(),
                            s(strings, "comments").as_bytes(),
                        ) {
                            Ok(()) => Value::Null,
                            Err(e) => error(&e, &path, &directory),
                        },
                    );
                }
                record.insert("strings".into(), strings.clone());
                record.insert("result".into(), Value::Array(results));
                record.insert("exists".into(), Value::Bool(path.exists()));
                if path.exists() {
                    record.insert("file".into(), file(&path));
                    record.insert("read".into(), read_back(&path, &directory));
                }
                let sample = root().join(format!("samples/info-{id}.caf"));
                if sample.exists() {
                    assert_eq!(
                        std::fs::read(&path).unwrap(),
                        std::fs::read(sample).unwrap()
                    );
                }
            }
            _ => {
                write_hex(&path, s(&case, "hex"));
                record.insert("read".into(), read_back(&path, &directory));
            }
        }
        let _ = std::fs::remove_file(&path);
        assert_eq!(
            mradm_json::dump(&Value::Object(record), None),
            want,
            "case {id}: {line}"
        );
        compared += 1;
    }
    assert_eq!(compared, 376);
    std::fs::remove_dir_all(&directory).unwrap();
}

#[test]
fn chunked_and_whole_writes_produce_the_same_file() {
    let samples: Vec<f32> = (0..6 * 1000).map(|i| (i as f32).sin()).collect();
    let mut whole = Writer::new(std::io::Cursor::new(Vec::new()), 6, 48000, b"0+5+0").unwrap();
    whole.write_frames(&samples).unwrap();
    let whole = whole.finish().unwrap().into_inner();
    let mut parts = Writer::new(std::io::Cursor::new(Vec::new()), 6, 48000, b"0+5+0").unwrap();
    for chunk in samples.chunks(6 * 7) {
        parts.write_frames(chunk).unwrap();
    }
    assert_eq!(parts.finish().unwrap().into_inner(), whole);
}

/// A sink that accepts `limit` bytes and then fails, to observe reported I/O errors.
struct Failing {
    inner: std::io::Cursor<Vec<u8>>,
    limit: usize,
    fail_seek: bool,
}
impl std::io::Write for Failing {
    fn write(&mut self, data: &[u8]) -> std::io::Result<usize> {
        if self.inner.get_ref().len() + data.len() > self.limit {
            return Err(std::io::Error::other("disk full"));
        }
        self.inner.write(data)
    }
    fn flush(&mut self) -> std::io::Result<()> {
        Ok(())
    }
}
impl std::io::Seek for Failing {
    fn seek(&mut self, to: std::io::SeekFrom) -> std::io::Result<u64> {
        if self.fail_seek {
            return Err(std::io::Error::other("seek failed"));
        }
        self.inner.seek(to)
    }
}

#[test]
fn write_and_finish_failures_are_reported() {
    let sink = |limit, fail_seek| Failing {
        inner: std::io::Cursor::new(Vec::new()),
        limit,
        fail_seek,
    };
    assert_eq!(
        Writer::new(sink(10, false), 2, 48000, b"0+2+0")
            .err()
            .unwrap()
            .code,
        3
    );
    let mut writer = Writer::new(sink(100, false), 2, 48000, b"0+2+0").unwrap();
    assert_eq!(writer.write_frames(&[0.0; 2]).unwrap(), 1);
    assert_eq!(
        writer.write_frames(&[0.0; 4]).err().unwrap().message,
        "disk full"
    );
    // A failed write leaves no usable file and refuses to finish.
    assert!(writer.write_frames(&[0.0; 2]).is_err());
    assert!(writer.finish().is_err());
    let mut writer = Writer::new(sink(1000, true), 2, 48000, b"0+2+0").unwrap();
    writer.write_frames(&[0.0; 2]).unwrap();
    assert_eq!(writer.finish().err().unwrap().message, "seek failed");
    let mut writer = Writer::new(sink(1000, false), 2, 48000, b"0+2+0").unwrap();
    assert_eq!(writer.write_frames(&[0.0; 3]).err().unwrap().code, 1);
}
