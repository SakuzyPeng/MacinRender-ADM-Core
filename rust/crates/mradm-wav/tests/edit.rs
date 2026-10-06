// Independent regressions for container edits; fixtures are assembled by hand, not by the writer.
use mradm_wav::{
    AMBI_HOA3, BEXT_SIZE, BextFields, Chna, ChnaTrack, Container, LayoutOptions, LayoutRewriter,
    Reader, SampleFormat, append_bext, bext_payload, has_chunk, replace_chunk,
};
use std::io::Cursor;

fn chunk(id: &[u8; 4], data: &[u8]) -> Vec<u8> {
    let mut out = id.to_vec();
    out.extend_from_slice(&(data.len() as u32).to_le_bytes());
    out.extend_from_slice(data);
    if data.len() & 1 != 0 {
        out.push(0);
    }
    out
}
fn fmt(tag: u16, channels: u16, bits: u16) -> Vec<u8> {
    let align = channels * bits / 8;
    let mut out = Vec::new();
    out.extend_from_slice(&tag.to_le_bytes());
    out.extend_from_slice(&channels.to_le_bytes());
    out.extend_from_slice(&48000u32.to_le_bytes());
    out.extend_from_slice(&(48000 * u32::from(align)).to_le_bytes());
    out.extend_from_slice(&align.to_le_bytes());
    out.extend_from_slice(&bits.to_le_bytes());
    out
}
fn riff(chunks: &[Vec<u8>]) -> Vec<u8> {
    let body: Vec<u8> = chunks.concat();
    let mut out = b"RIFF".to_vec();
    out.extend_from_slice(&(4 + body.len() as u32).to_le_bytes());
    out.extend_from_slice(b"WAVE");
    out.extend_from_slice(&body);
    out
}
fn rf64(id: &[u8; 4], chunks: &[Vec<u8>], data_bytes: u64) -> Vec<u8> {
    let body: Vec<u8> = chunks.concat();
    let mut out = id.to_vec();
    out.extend_from_slice(&u32::MAX.to_le_bytes());
    out.extend_from_slice(b"WAVE");
    let mut ds64 = Vec::new();
    ds64.extend_from_slice(&(4 + 36 + body.len() as u64).to_le_bytes());
    ds64.extend_from_slice(&data_bytes.to_le_bytes());
    ds64.extend_from_slice(&0u64.to_le_bytes());
    ds64.extend_from_slice(&0u32.to_le_bytes());
    out.extend_from_slice(&chunk(b"ds64", &ds64));
    out.extend_from_slice(&body);
    out
}
fn u32_at(data: &[u8], at: usize) -> u32 {
    u32::from_le_bytes(data[at..at + 4].try_into().unwrap())
}
fn u64_at(data: &[u8], at: usize) -> u64 {
    u64::from_le_bytes(data[at..at + 8].try_into().unwrap())
}
fn find(data: &[u8], id: &[u8; 4]) -> usize {
    let mut at = 12;
    while at + 8 <= data.len() {
        if &data[at..at + 4] == id {
            return at;
        }
        let size = u64::from(u32_at(data, at + 4));
        at += 8 + (size + (size & 1)) as usize;
    }
    panic!("chunk {id:?} missing");
}

#[test]
fn bext_fields_follow_tech_3285_v2_layout() {
    let payload = bext_payload(&BextFields {
        description: b"renderer=ear layout=0+2+0",
        originator: &[b'o'; 40],
        originator_reference: b"0+2+0",
        date_utc: b"2026-05-22T14:30:07Z",
        loudness: Some(-23.456),
        true_peak: Some(-0.005),
    });
    assert_eq!(payload.len(), BEXT_SIZE);
    assert_eq!(&payload[..25], b"renderer=ear layout=0+2+0");
    assert!(payload[25..256].iter().all(|&b| b == 0));
    // Fixed-width text keeps a terminating NUL.
    assert_eq!(&payload[256..287], &[b'o'; 31]);
    assert_eq!(payload[287], 0);
    assert_eq!(&payload[320..338], b"2026-05-2214-30-07");
    assert_eq!(u16::from_le_bytes([payload[346], payload[347]]), 2);
    let level = |at: usize| i16::from_le_bytes([payload[at], payload[at + 1]]);
    // Rounded half away from zero like lround.
    assert_eq!(
        [level(412), level(414), level(416), level(418), level(420)],
        [-2346, 0x7fff, -1, 0x7fff, 0x7fff]
    );
    assert!(payload[422..].iter().all(|&b| b == 0));

    let short = bext_payload(&BextFields {
        description: b"",
        originator: b"",
        originator_reference: b"",
        date_utc: b"2026",
        loudness: Some(f64::NAN),
        true_peak: Some(400.0),
    });
    assert!(short[320..338].iter().all(|&b| b == 0));
    assert_eq!(i16::from_le_bytes([short[412], short[413]]), 0x7fff);
    // 40000 hundredths wraps like the former int16 cast.
    assert_eq!(
        i16::from_le_bytes([short[416], short[417]]),
        40000u16 as i16
    );
}

#[test]
fn append_bext_updates_riff_and_ds64_sizes_and_ambi() {
    let payload = [7u8; BEXT_SIZE];
    let original = riff(&[chunk(b"fmt ", &fmt(1, 16, 16)), chunk(b"data", &[1; 64])]);
    let mut file = Cursor::new(original.clone());
    append_bext(&mut file, &payload, Some(&AMBI_HOA3)).unwrap();
    let out = file.into_inner();
    assert_eq!(&out[..original.len()], {
        let mut head = original.clone();
        head[4..8].copy_from_slice(&((out.len() - 8) as u32).to_le_bytes());
        head
    });
    let bext = find(&out, b"bext");
    assert_eq!(u32_at(&out, bext + 4), 602);
    assert_eq!(&out[bext + 8..bext + 610], &payload);
    let ambi = find(&out, b"ambi");
    assert_eq!(ambi, bext + 610);
    assert_eq!(&out[ambi + 8..ambi + 24], &AMBI_HOA3);

    // A second write appends another bext but rewrites the existing ambi in place.
    let mut file = Cursor::new(out.clone());
    append_bext(&mut file, &payload, Some(&AMBI_HOA3)).unwrap();
    let twice = file.into_inner();
    assert_eq!(twice.len(), out.len() + 610);
    assert_eq!(u32_at(&twice, 4) as usize, twice.len() - 8);
    assert!(Reader::new(Cursor::new(twice)).is_ok());

    let wide = rf64(
        b"RF64",
        &[chunk(b"fmt ", &fmt(3, 1, 32)), chunk(b"data", &[0; 16])],
        16,
    );
    let mut file = Cursor::new(wide);
    append_bext(&mut file, &payload, None).unwrap();
    let out = file.into_inner();
    assert_eq!(u32_at(&out, 4), u32::MAX);
    assert_eq!(u64_at(&out, 20) as usize, out.len() - 8);
    assert!(Reader::new(Cursor::new(out)).is_ok());

    // Invalid existing ambi and bytes beyond the container are rejected before any write.
    let bad = riff(&[
        chunk(b"fmt ", &fmt(1, 1, 16)),
        chunk(b"ambi", &[0; 12]),
        chunk(b"data", &[0; 4]),
    ]);
    let mut file = Cursor::new(bad.clone());
    assert!(append_bext(&mut file, &payload, Some(&AMBI_HOA3)).is_err());
    assert!(append_bext(&mut file, &payload, None).is_ok());
    let mut trailing = bad;
    trailing.push(0);
    let mut file = Cursor::new(trailing.clone());
    assert!(append_bext(&mut file, &payload, None).is_err());
    assert_eq!(file.into_inner(), trailing);
}

fn rewrite(source: Vec<u8>, options: &LayoutOptions, block: u64) -> mradm_wav::Result<Vec<u8>> {
    let mut rewriter = LayoutRewriter::new(Cursor::new(source), Vec::new(), options)?;
    let mut copied = 0;
    loop {
        let count = rewriter.step(block)?;
        if count == 0 {
            break;
        }
        assert!(count <= block);
        copied += count;
    }
    assert_eq!(copied, rewriter.frames());
    rewriter.finish()
}

#[test]
fn layout_rewrite_permutes_frames_and_selects_container() {
    // 3 channels of 24-bit PCM, 5 frames; byte (frame, channel) = 10 * frame + channel.
    let data: Vec<u8> = (0..5u8)
        .flat_map(|frame| (0..3u8).flat_map(move |channel| [10 * frame + channel; 3]))
        .collect();
    let source = riff(&[chunk(b"fmt ", &fmt(1, 3, 24)), chunk(b"data", &data)]);
    let options = LayoutOptions {
        channel_mask: 0x7,
        force_extensible: false,
        include_pcm_fact: false,
        prefer_riff: false,
        permutation: &[2, 0, 1],
        axml: &[],
        chna: &[],
    };
    let out = rewrite(source.clone(), &options, 2).unwrap();
    let mut reader = Reader::new(Cursor::new(out.clone())).unwrap();
    let info = reader.info();
    assert_eq!(info.container, Container::Riff);
    assert_eq!(info.format.channel_mask, Some(0x7));
    assert_eq!(info.frames, 5);
    let at = find(&out, b"data") + 8;
    for frame in 0..5u8 {
        for (slot, source) in [2u8, 0, 1].into_iter().enumerate() {
            let offset = at + usize::from(frame) * 9 + slot * 3;
            assert_eq!(&out[offset..offset + 3], &[10 * frame + source; 3]);
        }
    }
    let mut samples = [0.0f32; 15];
    assert_eq!(reader.read_frames(&mut samples).unwrap(), 5);

    // ADM chunks turn integer PCM into BW64 with chna before data and axml last.
    let tracks = [
        ChnaTrack {
            track_index: 1,
            uid: b"ATU_00000001",
            track_format: b"AT_00010001_01",
            pack_format: b"AP_00010002",
        },
        ChnaTrack {
            track_index: 3,
            uid: b"ATU_3",
            track_format: b"AT_1",
            pack_format: b"AP_1",
        },
    ];
    let adm = LayoutOptions {
        channel_mask: 0,
        force_extensible: false,
        include_pcm_fact: true,
        prefer_riff: true,
        permutation: &[],
        axml: b"<xml/>!",
        chna: &tracks,
    };
    let out = rewrite(source.clone(), &adm, 64).unwrap();
    assert_eq!(&out[..4], b"BW64");
    let mut reader = Reader::new(Cursor::new(out.clone())).unwrap();
    assert_eq!(reader.info().container, Container::Bw64);
    let chna = reader.chna().unwrap().unwrap();
    assert_eq!(chna.tracks, 3);
    assert_eq!(chna.entries[1].uid, *b"ATU_3       ");
    assert_eq!(chna.entries[1].pad, b' ');
    assert_eq!(reader.metadata(*b"axml", 64).unwrap().unwrap(), b"<xml/>!");
    let ids: Vec<_> = reader.chunks().iter().map(|c| c.id).collect();
    assert_eq!(
        ids,
        [*b"ds64", *b"fmt ", *b"chna", *b"fact", *b"data", *b"axml"]
    );
    assert_eq!(&out[find(&out, b"data") + 8..][..data.len()], &data[..]);

    // Float input keeps its 64-bit container unless RIFF is preferred.
    let float = rf64(
        b"RF64",
        &[chunk(b"fmt ", &fmt(3, 1, 32)), chunk(b"data", &[0; 12])],
        12,
    );
    let plain = LayoutOptions {
        channel_mask: 0,
        force_extensible: true,
        include_pcm_fact: false,
        prefer_riff: false,
        permutation: &[],
        axml: &[],
        chna: &[],
    };
    let out = rewrite(float.clone(), &plain, 1).unwrap();
    let info = Reader::new(Cursor::new(out)).unwrap().info();
    assert_eq!(info.container, Container::Rf64);
    assert_eq!(info.format.sample_format, SampleFormat::Float32);
    assert_eq!(info.format.channel_mask, Some(0));
    let riff_options = LayoutOptions {
        prefer_riff: true,
        ..plain
    };
    let out = rewrite(float, &riff_options, 1).unwrap();
    assert_eq!(&out[..4], b"RIFF");
    assert!(find(&out, b"fact") > 0);

    // Invalid layouts fail before writing; finishing early fails.
    let bad = [
        LayoutOptions {
            channel_mask: 0x3,
            ..plain
        },
        LayoutOptions {
            permutation: &[0, 0, 1],
            ..plain
        },
        LayoutOptions {
            axml: b"x",
            ..plain
        },
    ];
    for options in &bad {
        let mut sink = Vec::new();
        assert!(LayoutRewriter::new(Cursor::new(source.clone()), &mut sink, options).is_err());
        assert!(sink.is_empty());
    }
    let mut early = LayoutRewriter::new(Cursor::new(source), Vec::new(), &plain).unwrap();
    early.step(1).unwrap();
    assert!(early.finish().is_err());
}

#[test]
fn replace_chunk_copies_other_chunks_and_resizes_container() {
    let source = riff(&[
        chunk(b"fmt ", &fmt(1, 1, 16)),
        chunk(b"LIST", b"odd"),
        chunk(b"axml", b"<old/>"),
        chunk(b"data", &[9; 10]),
    ]);
    let mut out = Vec::new();
    replace_chunk(
        Cursor::new(source.clone()),
        &mut out,
        *b"axml",
        b"<new-longer/>",
    )
    .unwrap();
    assert_eq!(u32_at(&out, 4) as usize, out.len() - 8);
    let axml = find(&out, b"axml");
    assert_eq!(&out[axml + 8..axml + 21], b"<new-longer/>");
    assert_eq!(
        &out[..axml],
        &{
            let mut head = source[..find(&source, b"axml")].to_vec();
            head[4..8].copy_from_slice(&out[4..8]);
            head
        }[..]
    );
    let mut reader = Reader::new(Cursor::new(out)).unwrap();
    assert_eq!(reader.metadata(*b"LIST", 8).unwrap().unwrap(), b"odd");

    let wide = rf64(
        b"BW64",
        &[
            chunk(b"fmt ", &fmt(1, 1, 16)),
            chunk(b"axml", b"<a/>"),
            chunk(b"data", &[1; 6]),
        ],
        6,
    );
    let mut out = Vec::new();
    replace_chunk(Cursor::new(wide), &mut out, *b"axml", b"").unwrap();
    assert_eq!(&out[..4], b"BW64");
    assert_eq!(u64_at(&out, 20) as usize, out.len() - 8);
    assert!(Reader::new(Cursor::new(out)).is_ok());

    let mut out = Vec::new();
    assert!(replace_chunk(Cursor::new(source), &mut out, *b"chna", b"").is_err());
    assert!(out.is_empty());
}

#[test]
fn has_chunk_is_tolerant_but_requires_wave() {
    let file = riff(&[
        chunk(b"fmt ", &fmt(1, 1, 8)),
        chunk(b"data", &[0; 3]),
        chunk(b"axml", b"x"),
    ]);
    assert!(has_chunk(Cursor::new(file.clone()), *b"axml").unwrap());
    assert!(!has_chunk(Cursor::new(file.clone()), *b"chna").unwrap());
    let cut = &file[..file.len() - 9];
    assert!(!has_chunk(Cursor::new(cut), *b"axml").unwrap());
    assert!(has_chunk(Cursor::new(b"RIFF\0\0\0\0AIFF".as_slice()), *b"axml").is_err());
    assert!(has_chunk(Cursor::new(b"RIF".as_slice()), *b"axml").is_err());
}

#[test]
fn import_chna_accepts_reserved_records() {
    let mut data = vec![2, 0, 1, 0];
    data.extend_from_slice(&0u16.to_le_bytes());
    data.extend_from_slice(&[b'A'; 38]);
    data.resize(4 + 40 * 3, 0);
    let chna = Chna::decode_import(&data).unwrap();
    assert_eq!(chna.entries.len(), 1);
    assert_eq!(chna.entries[0].track_index, 0);
    assert!(Chna::decode(&data).is_err());
    assert!(Chna::decode_import(&data[..43]).is_err());
    assert!(Chna::decode_import(&data[..3]).is_err());
}
