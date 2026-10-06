use mradm_wav::{Chna, ChnaEntry, Container, Format, Reader, SampleFormat, Writer, WriterOptions};
use std::io::{self, Cursor, Read, Seek, SeekFrom, Write};
use std::sync::{
    Arc,
    atomic::{AtomicU8, Ordering},
};

fn format(sample_format: SampleFormat, channels: u16) -> Format {
    Format {
        sample_format,
        channels,
        sample_rate: 48000,
        channel_mask: None,
    }
}
fn chunk(id: &[u8; 4], data: &[u8]) -> Vec<u8> {
    let mut out = id.to_vec();
    out.extend_from_slice(&(data.len() as u32).to_le_bytes());
    out.extend_from_slice(data);
    if data.len() & 1 != 0 {
        out.push(0);
    }
    out
}
// Independent fixture assembly: no production writer or CHNA encoder.
fn fixture(container: Container, bits: u16, channels: u16, samples: &[i32]) -> Vec<u8> {
    let align = channels * (bits / 8);
    let mut fmt = Vec::new();
    for value in [1u16, channels] {
        fmt.extend_from_slice(&value.to_le_bytes());
    }
    fmt.extend_from_slice(&48000u32.to_le_bytes());
    fmt.extend_from_slice(&(48000u32 * u32::from(align)).to_le_bytes());
    fmt.extend_from_slice(&align.to_le_bytes());
    fmt.extend_from_slice(&bits.to_le_bytes());
    let mut pcm = Vec::new();
    for value in samples {
        pcm.extend_from_slice(&value.to_le_bytes()[..usize::from(bits / 8)]);
    }
    let mut body = chunk(b"fmt ", &fmt);
    body.extend(chunk(b"odd!", b"abc"));
    let mut data = chunk(b"data", &pcm);
    if container != Container::Riff {
        data[4..8].copy_from_slice(&u32::MAX.to_le_bytes());
    }
    body.extend(data);
    body.extend(chunk(b"axml", b"<xml/ >"));
    let mut out = match container {
        Container::Riff => b"RIFF".to_vec(),
        Container::Rf64 => b"RF64".to_vec(),
        Container::Bw64 => b"BW64".to_vec(),
    };
    let size = (body.len() + 4 + if container == Container::Riff { 0 } else { 36 }) as u64;
    out.extend_from_slice(
        &if container == Container::Riff {
            size as u32
        } else {
            u32::MAX
        }
        .to_le_bytes(),
    );
    out.extend_from_slice(b"WAVE");
    if container != Container::Riff {
        let mut ds = Vec::new();
        for n in [
            size,
            pcm.len() as u64,
            (samples.len() / usize::from(channels)) as u64,
        ] {
            ds.extend_from_slice(&n.to_le_bytes());
        }
        ds.extend_from_slice(&0u32.to_le_bytes());
        out.extend(chunk(b"ds64", &ds));
    }
    out.extend(body);
    out
}
fn sample_values(bits: u16, count: usize) -> Vec<i32> {
    let max = (1i64 << (bits - 1)) - 1;
    let min = -max - 1;
    let edges = [min, min + 1, -2, -1, 0, 1, 2, max - 1, max];
    let mut state = 0x12345678u32;
    (0..count)
        .map(|i| {
            state ^= state << 13;
            state ^= state >> 17;
            state ^= state << 5;
            if i < edges.len() {
                edges[i] as i32
            } else {
                (state as i32) >> (32 - bits)
            }
        })
        .collect()
}

#[test]
fn independent_pcm_matrix_and_tail_bounds() {
    for container in [Container::Riff, Container::Rf64, Container::Bw64] {
        for bits in [16, 24, 32] {
            for channels in [1, 2, 11, 128] {
                let values = sample_values(bits, 513 * usize::from(channels));
                let mut reader =
                    Reader::new(Cursor::new(fixture(container, bits, channels, &values))).unwrap();
                assert_eq!(reader.info().frames, 513);
                assert_eq!(reader.info().container, container);
                let mut output = vec![0.0; values.len()];
                assert_eq!(reader.read_frames(&mut output).unwrap(), 513);
                for (actual, value) in output.iter().zip(&values) {
                    let expected = (f64::from(*value) / (1u64 << (bits - 1)) as f64) as f32;
                    assert_eq!(actual.to_bits(), expected.to_bits());
                }
                assert_eq!(reader.seek_frame(u64::MAX).unwrap(), 513);
                assert_eq!(reader.read_frames(&mut output).unwrap(), 0);
                assert_eq!(reader.metadata(*b"axml", 100).unwrap().unwrap(), b"<xml/ >");
                assert!(reader.metadata(*b"axml", 1).is_err());
                reader.seek_frame(123).unwrap();
                assert_eq!(
                    reader
                        .read_frames(&mut output[..usize::from(channels)])
                        .unwrap(),
                    1
                );
                assert_eq!(
                    output[0],
                    (f64::from(values[123 * usize::from(channels)]) / (1u64 << (bits - 1)) as f64)
                        as f32
                );
            }
        }
    }
}

fn data_payload(data: &[u8]) -> &[u8] {
    let mut pos = 12;
    while pos + 8 <= data.len() {
        let length = u32::from_le_bytes(data[pos + 4..pos + 8].try_into().unwrap()) as usize;
        if &data[pos..pos + 4] == b"data" {
            return &data[pos + 8..pos + 8 + length];
        }
        pos += 8 + length + (length & 1);
    }
    panic!("missing data");
}

#[test]
fn integer_writer_matches_legacy_quantization_and_rejects_nan() {
    for (sample_format, golden) in [
        (
            SampleFormat::Pcm16,
            vec![-32767, -16383, 0, 0, 0, 16383, 32767, 32767],
        ),
        (
            SampleFormat::Pcm24,
            vec![-8388607, -4194303, 0, 0, 0, 4194303, 8388607, 8388607],
        ),
        (
            SampleFormat::Pcm32,
            vec![
                -2147483647,
                -1073741823,
                -255,
                0,
                255,
                1073741823,
                2147483647,
                2147483647,
            ],
        ),
    ] {
        let mut writer = Writer::new(
            Cursor::new(Vec::new()),
            format(sample_format, 2),
            WriterOptions::default(),
        )
        .unwrap();
        let mut samples = vec![
            -1.0,
            -0.5,
            -1.0 / 8388608.0,
            0.0,
            1.0 / 8388608.0,
            0.5,
            1.0,
            f32::INFINITY,
        ];
        let random = sample_values(32, 65536 - samples.len());
        samples.extend(random.iter().map(|v| (*v as f32) / 1073741824.0));
        for block in samples.chunks(1024) {
            writer.write_frames(block).unwrap();
        }
        assert!(writer.write_frames(&[f32::NAN, 0.0]).is_err());
        let data = writer.finish().unwrap().into_inner();
        let width = usize::from(sample_format.bits() / 8);
        let payload = data_payload(&data);
        let maximum = ((1u64 << (sample_format.bits() - 1)) - 1) as f64;
        for (i, (raw, input)) in payload.chunks_exact(width).zip(&samples).enumerate() {
            let mut value = [if raw[width - 1] & 128 != 0 { 255 } else { 0 }; 4];
            value[..width].copy_from_slice(raw);
            let expected = if i < golden.len() {
                golden[i]
            } else {
                (f64::from(input.clamp(-1.0, 1.0)) * maximum) as i32
            };
            assert_eq!(i32::from_le_bytes(value), expected);
        }
        assert_eq!(payload.len() / width, 65536);
    }
}

#[test]
fn high_rates_extensible_zero_mask_float_and_metadata() {
    for rate in [96000, 192000] {
        for sample_format in [SampleFormat::Pcm24, SampleFormat::Float32] {
            for container in [None, Some(Container::Rf64), Some(Container::Bw64)] {
                let fmt = Format {
                    sample_rate: rate,
                    channel_mask: Some(0),
                    ..format(sample_format, 1)
                };
                let chna = Chna {
                    tracks: 1,
                    entries: vec![ChnaEntry {
                        track_index: 1,
                        uid: *b"ATU_00000001",
                        track_format: *b"AT_00031001_01",
                        pack_format: *b"AP_00031001",
                        pad: 0,
                    }],
                };
                let options = WriterOptions {
                    container,
                    chna: Some(chna.clone()),
                    axml: Some(b"<axml/>".to_vec()),
                };
                let result = Writer::new(Cursor::new(Vec::new()), fmt, options);
                if sample_format == SampleFormat::Float32 && container == Some(Container::Bw64) {
                    assert!(result.is_err());
                    continue;
                }
                let mut writer = result.unwrap();
                writer.write_frames(&[-2.0, 0.0, 2.0]).unwrap();
                let mut reader = Reader::new(writer.finish().unwrap()).unwrap();
                assert_eq!(reader.info().format, fmt);
                assert_eq!(reader.chna().unwrap().unwrap(), chna);
                assert_eq!(reader.metadata(*b"axml", 100).unwrap().unwrap(), b"<axml/>");
                let mut output = [0.0; 3];
                reader.read_frames(&mut output).unwrap();
                if sample_format == SampleFormat::Float32 {
                    assert_eq!(output, [-2.0, 0.0, 2.0]);
                }
            }
        }
    }
    let writer = Writer::new(
        Cursor::new(Vec::new()),
        format(SampleFormat::Pcm16, 1),
        WriterOptions::default(),
    )
    .unwrap();
    assert_eq!(
        Reader::new(writer.finish().unwrap()).unwrap().info().frames,
        0
    );
}

#[test]
fn malformed_files_return_errors_without_panicking() {
    let original = fixture(Container::Riff, 24, 2, &[0; 12]);
    for length in 0..original.len() {
        assert!(Reader::new(Cursor::new(original[..length].to_vec())).is_err());
    }
    for (offset, value) in [(22, 0u16), (32, 1), (34, 20), (20, 6)] {
        let mut input = original.clone();
        input[offset..offset + 2].copy_from_slice(&value.to_le_bytes());
        assert!(Reader::new(Cursor::new(input)).is_err());
    }
    let mut input = original;
    input[16..20].copy_from_slice(&u32::MAX.to_le_bytes());
    assert!(Reader::new(Cursor::new(input)).is_err());
    let mut rf64 = fixture(Container::Rf64, 24, 2, &[0; 12]);
    rf64[44..48].copy_from_slice(&u32::MAX.to_le_bytes());
    assert!(Reader::new(Cursor::new(rf64)).is_err());
    // ds64 sampleCount is informational: a wrong value must not reject the file.
    let mut bw64 = fixture(Container::Bw64, 24, 2, &[0; 12]);
    bw64[36..44].copy_from_slice(&999u64.to_le_bytes());
    assert_eq!(Reader::new(Cursor::new(bw64)).unwrap().info().frames, 6);
    assert!(Chna::decode(&[0; 3]).is_err());
    assert!(Chna::decode(&[1, 0, 2, 0]).is_err());
}

struct FaultIo {
    data: Cursor<Vec<u8>>,
    fault: Arc<AtomicU8>,
}
impl Read for FaultIo {
    fn read(&mut self, data: &mut [u8]) -> io::Result<usize> {
        if self.fault.load(Ordering::Relaxed) == 1 {
            return Err(io::Error::other("injected read failure"));
        }
        self.data.read(data)
    }
}
impl Write for FaultIo {
    fn write(&mut self, data: &[u8]) -> io::Result<usize> {
        if self.fault.load(Ordering::Relaxed) == 2 {
            return Err(io::Error::other("injected write failure"));
        }
        self.data.write(data)
    }
    fn flush(&mut self) -> io::Result<()> {
        if self.fault.load(Ordering::Relaxed) == 3 {
            return Err(io::Error::other("injected flush failure"));
        }
        Ok(())
    }
}
impl Seek for FaultIo {
    fn seek(&mut self, to: SeekFrom) -> io::Result<u64> {
        if self.fault.load(Ordering::Relaxed) == 4 {
            return Err(io::Error::other("injected seek failure"));
        }
        self.data.seek(to)
    }
}
#[test]
fn io_failures_propagate_and_poison_audio_handles() {
    for fault in [1, 4] {
        let flag = Arc::new(AtomicU8::new(0));
        let io = FaultIo {
            data: Cursor::new(fixture(Container::Riff, 24, 1, &[1, 2, 3])),
            fault: flag.clone(),
        };
        let mut reader = Reader::new(io).unwrap();
        flag.store(fault, Ordering::Relaxed);
        assert!(reader.read_frames(&mut [0.0; 3]).is_err());
        flag.store(0, Ordering::Relaxed);
        assert!(reader.read_frames(&mut [0.0; 3]).is_err());
    }
    for fault in [2, 3, 4] {
        let flag = Arc::new(AtomicU8::new(0));
        let io = FaultIo {
            data: Cursor::new(Vec::new()),
            fault: flag.clone(),
        };
        let mut writer =
            Writer::new(io, format(SampleFormat::Pcm24, 1), WriterOptions::default()).unwrap();
        writer.write_frames(&[0.5]).unwrap();
        flag.store(fault, Ordering::Relaxed);
        assert!(writer.finish().is_err());
    }
    let flag = Arc::new(AtomicU8::new(0));
    let mut writer = Writer::new(
        FaultIo {
            data: Cursor::new(Vec::new()),
            fault: flag.clone(),
        },
        format(SampleFormat::Pcm16, 1),
        WriterOptions::default(),
    )
    .unwrap();
    flag.store(2, Ordering::Relaxed);
    assert!(writer.write_frames(&[0.5]).is_err());
    flag.store(0, Ordering::Relaxed);
    assert!(writer.finish().is_err());
}

// A seekable sparse source models >4 GiB offsets without reserving disk or RAM.
struct Sparse {
    position: u64,
    length: u64,
    islands: Vec<(u64, Vec<u8>)>,
}
impl Seek for Sparse {
    fn seek(&mut self, to: SeekFrom) -> io::Result<u64> {
        let target = match to {
            SeekFrom::Start(n) => i128::from(n),
            SeekFrom::Current(n) => i128::from(self.position) + i128::from(n),
            SeekFrom::End(n) => i128::from(self.length) + i128::from(n),
        };
        self.position = u64::try_from(target).map_err(|_| io::Error::other("bad sparse seek"))?;
        Ok(self.position)
    }
}
impl Read for Sparse {
    fn read(&mut self, data: &mut [u8]) -> io::Result<usize> {
        let length = (data.len() as u64).min(self.length.saturating_sub(self.position)) as usize;
        data[..length].fill(0);
        for (start, bytes) in &self.islands {
            let lo = self.position.max(*start);
            let hi = (self.position + length as u64).min(*start + bytes.len() as u64);
            if lo < hi {
                data[(lo - self.position) as usize..(hi - self.position) as usize]
                    .copy_from_slice(&bytes[(lo - start) as usize..(hi - start) as usize]);
            }
        }
        self.position += length as u64;
        Ok(length)
    }
}
#[test]
fn sparse_long_frame_and_extended_ds64_table() {
    for container in [Container::Rf64, Container::Bw64] {
        let frames = (1u64 << 31) + 123;
        let mut header = fixture(container, 16, 1, &[1, 2]);
        let start = header.windows(4).position(|v| v == b"data").unwrap() + 8;
        header.truncate(start);
        let tail = chunk(b"axml", b"<xml/>");
        let length = start as u64 + frames * 2 + tail.len() as u64;
        header[20..28].copy_from_slice(&(length - 8).to_le_bytes());
        header[28..36].copy_from_slice(&(frames * 2).to_le_bytes());
        header[36..44].copy_from_slice(&frames.to_le_bytes());
        let source = Sparse {
            position: 0,
            length,
            islands: vec![
                (0, header),
                (
                    start as u64 + (frames - 1) * 2,
                    1234i16.to_le_bytes().to_vec(),
                ),
                (start as u64 + frames * 2, tail),
            ],
        };
        let mut reader = Reader::new(source).unwrap();
        assert_eq!(reader.info().frames, frames);
        reader.seek_frame(frames - 1).unwrap();
        let mut output = [0.0; 1];
        assert_eq!(reader.read_frames(&mut output).unwrap(), 1);
        assert_eq!(output[0], 1234.0 / 32768.0);
        assert_eq!(reader.read_frames(&mut output).unwrap(), 0);
        assert_eq!(reader.metadata(*b"axml", 20).unwrap().unwrap(), b"<xml/>");
    }
    // Two identically named long chunks must consume ds64 entries in occurrence order.
    let a = (1u64 << 32) + 2;
    let b = (1u64 << 32) + 4;
    let pcm = fixture(Container::Riff, 16, 1, &[1234]);
    let suffix = pcm[12..].to_vec();
    let length = 12 + 8 + 52 + 8 + a + 8 + b + suffix.len() as u64;
    let mut header = b"BW64".to_vec();
    header.extend_from_slice(&u32::MAX.to_le_bytes());
    header.extend_from_slice(b"WAVE");
    let mut ds = Vec::new();
    for n in [length - 8, 2, 1] {
        ds.extend_from_slice(&n.to_le_bytes());
    }
    ds.extend_from_slice(&2u32.to_le_bytes());
    for n in [a, b] {
        ds.extend_from_slice(b"JUNK");
        ds.extend_from_slice(&n.to_le_bytes());
    }
    header.extend(chunk(b"ds64", &ds));
    header.extend_from_slice(b"JUNK");
    header.extend_from_slice(&u32::MAX.to_le_bytes());
    let first = header.len() as u64;
    let mut second = b"JUNK".to_vec();
    second.extend_from_slice(&u32::MAX.to_le_bytes());
    let source = Sparse {
        position: 0,
        length,
        islands: vec![
            (0, header),
            (first + a, second),
            (first + a + 8 + b, suffix),
        ],
    };
    let mut reader = Reader::new(source).unwrap();
    assert_eq!(reader.read_frames(&mut [0.0]).unwrap(), 1);
}

#[test]
#[ignore = "writes >4 GiB; set MRADM_WAV_LARGE_TEST_DIR to an available scratch volume"]
fn real_large_writer_promotes_and_reads_back() {
    let directory =
        std::env::var_os("MRADM_WAV_LARGE_TEST_DIR").expect("explicit scratch directory required");
    let path = std::path::PathBuf::from(directory)
        .join(format!("mradm-large-wave-{}.wav", std::process::id()));
    struct Cleanup(std::path::PathBuf);
    impl Drop for Cleanup {
        fn drop(&mut self) {
            let _ = std::fs::remove_file(&self.0);
        }
    }
    let file = std::fs::OpenOptions::new()
        .write(true)
        .create_new(true)
        .open(&path)
        .unwrap();
    let _cleanup = Cleanup(path.clone());
    let mut writer = Writer::new(
        file,
        format(SampleFormat::Pcm32, 2),
        WriterOptions::default(),
    )
    .unwrap();
    let block = vec![0.25f32; 131072];
    let frames = (1u64 << 29) + 1024;
    let mut remaining = frames;
    while remaining > 0 {
        let n = remaining.min((block.len() / 2) as u64);
        writer.write_frames(&block[..n as usize * 2]).unwrap();
        remaining -= n;
    }
    drop(writer.finish().unwrap());
    let mut reader = Reader::new(std::fs::File::open(&path).unwrap()).unwrap();
    assert_eq!(reader.info().container, Container::Bw64);
    assert_eq!(reader.info().frames, frames);
    for position in [0, (1 << 29) - 2048, frames - 1] {
        reader.seek_frame(position).unwrap();
        let mut buffer = [0.0; 2];
        assert_eq!(reader.read_frames(&mut buffer).unwrap(), 1);
        assert_eq!(buffer, [0.25, 0.25]);
    }
}

#[test]
fn independent_extensible_pcm_and_float_bytes() {
    for valid in [0u16, 24, 32, 33] {
        let values = [-8388608i32, -1, 0, 1, 8388607].map(|v| v << 8);
        let original = fixture(Container::Riff, 32, 1, &values);
        let mut fmt = original[20..36].to_vec();
        fmt[0..2].copy_from_slice(&0xfffeu16.to_le_bytes());
        fmt.extend_from_slice(&22u16.to_le_bytes());
        fmt.extend_from_slice(&valid.to_le_bytes());
        fmt.extend_from_slice(&0u32.to_le_bytes());
        fmt.extend_from_slice(&[1, 0, 0, 0, 0, 0, 16, 0, 128, 0, 0, 170, 0, 56, 155, 113]);
        let mut body = b"WAVE".to_vec();
        body.extend(chunk(b"fmt ", &fmt));
        body.extend_from_slice(&original[36..]);
        let mut input = b"RIFF".to_vec();
        input.extend_from_slice(&(body.len() as u32).to_le_bytes());
        input.extend(body);
        let result = Reader::new(Cursor::new(input));
        if valid > 32 {
            assert!(result.is_err());
            continue;
        }
        let mut reader = result.unwrap();
        assert_eq!(
            reader.info().valid_bits,
            if valid == 0 { 32 } else { valid }
        );
        assert_eq!(reader.info().format.channel_mask, Some(0));
        let mut output = [0.0; 5];
        reader.read_frames(&mut output).unwrap();
        assert_eq!(
            output,
            [
                -1.0,
                -1.0 / 8388608.0,
                0.0,
                1.0 / 8388608.0,
                8388607.0 / 8388608.0
            ]
        );
    }
    let samples = [-2.0f32, -0.0, 0.0, 2.0, f32::INFINITY];
    let values = samples.map(|s| s.to_bits() as i32);
    let mut file = fixture(Container::Rf64, 32, 1, &values);
    file[56..58].copy_from_slice(&3u16.to_le_bytes());
    let mut reader = Reader::new(Cursor::new(file)).unwrap();
    let mut decoded = [0.0f32; 5];
    reader.read_frames(&mut decoded).unwrap();
    assert_eq!(samples.map(f32::to_bits), decoded.map(f32::to_bits));
}
