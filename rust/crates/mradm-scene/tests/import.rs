use mradm_scene::{Context, ImportOptions, SourceKind, import, import_adm};
use mradm_wav::{Chna, ChnaEntry, Format, SampleFormat, Writer, WriterOptions};
use std::fs::File;
use std::path::PathBuf;
use std::sync::atomic::{AtomicUsize, Ordering};

const XML: &str = include_str!("../../../../tests/fixtures/adm/objects-stream-only.xml");

struct Temp(PathBuf);
impl Temp {
    fn new() -> Self {
        static NEXT: AtomicUsize = AtomicUsize::new(0);
        let n = NEXT.fetch_add(1, Ordering::Relaxed);
        Self(std::env::temp_dir().join(format!("mradm-scene-{}-{n}.wav", std::process::id())))
    }
    fn path(&self) -> &[u8] {
        self.0.to_str().unwrap().as_bytes()
    }
}
impl Drop for Temp {
    fn drop(&mut self) {
        let _ = std::fs::remove_file(&self.0);
    }
}

fn wav(channels: u16, mask: Option<u32>, axml: Option<&str>, chna: Option<Chna>) -> Temp {
    let temp = Temp::new();
    let format = Format {
        channels,
        sample_rate: 48000,
        sample_format: SampleFormat::Pcm24,
        channel_mask: mask,
    };
    let options = WriterOptions {
        container: None,
        axml: axml.map(|x| x.as_bytes().to_vec()),
        chna,
    };
    let mut writer = Writer::new(File::create(&temp.0).unwrap(), format, options).unwrap();
    writer
        .write_frames(&vec![0.0; usize::from(channels) * 480])
        .unwrap();
    writer.finish().unwrap();
    temp
}

fn entry(track_index: u16, uid: &[u8]) -> ChnaEntry {
    let mut fixed = [b' '; 12];
    fixed[..uid.len()].copy_from_slice(uid);
    ChnaEntry {
        track_index,
        uid: fixed,
        track_format: *b"AT_00031001_01",
        pack_format: *b"AP_00031001",
        pad: 0,
    }
}

#[test]
fn chna_matching_ignores_padding_and_case_and_keeps_the_last_track() {
    let chna = Chna {
        tracks: 3,
        entries: vec![
            entry(1, b"atu_00000002"),
            entry(2, b"ATU_00000001"),
            entry(3, b"ATU_00000001"),
        ],
    };
    let file = wav(3, None, Some(XML), Some(chna));
    let scene = import_adm(file.path()).unwrap();
    assert_eq!(scene.info.source_kind, SourceKind::Adm);
    assert_eq!(
        (
            scene.info.sample_rate,
            scene.info.channels,
            scene.info.frames
        ),
        (48000, 3, 480)
    );
    let s = &scene.snapshot;
    let channels: Vec<(&str, i32)> = s
        .tracks
        .iter()
        .map(|t| (s.strings[t.uid as usize].as_str(), t.channel))
        .collect();
    assert_eq!(channels, [("ATU_00000001", 2), ("ATU_00000002", 0)]);
}

#[test]
fn adm_import_errors_carry_the_importer_locator() {
    let zero = Chna {
        tracks: 1,
        entries: vec![entry(0, b"ATU_00000001")],
    };
    let error = import_adm(wav(1, None, Some(XML), Some(zero)).path()).unwrap_err();
    assert_eq!((error.code, error.context), (3, Context::Input));
    assert_eq!(error.message, "CHNA trackIndex 必须从 1 开始");

    let error = import_adm(wav(1, None, None, None).path()).unwrap_err();
    assert_eq!(
        (error.code, error.message.as_str(), error.context),
        (3, "axml chunk 缺失或为空", Context::Input)
    );

    let missing = Temp::new();
    let error = import_adm(missing.path()).unwrap_err();
    assert_eq!((error.code, error.context), (3, Context::Path));
}

#[test]
fn channel_beds_follow_the_wave_mask_or_an_explicit_layout() {
    let file = wav(6, Some(0x3F), None, None);
    let scene = import(file.path(), ImportOptions::default()).unwrap();
    assert_eq!(scene.info.source_kind, SourceKind::ChannelBed);
    assert_eq!(scene.info.input_layout, "5.1");
    assert_eq!(
        scene.info.input_channel_labels,
        ["M+030", "M-030", "M+000", "LFE1", "M+110", "M-110"]
    );
    let blocks = &scene.snapshot.direct_blocks;
    assert_eq!(blocks.len(), 6);
    assert_eq!(
        (
            blocks[3].has_position,
            blocks[3].low_pass_present,
            blocks[3].low_pass
        ),
        (0, 1, 120.0)
    );
    assert_eq!((blocks[0].has_position, blocks[0].low_pass_present), (1, 0));
    assert!(scene.snapshot.warnings.is_empty());

    // An explicit mapping wins over embedded AXML, with a warning.
    let adm = wav(6, None, Some(XML), None);
    let scene = import(
        adm.path(),
        ImportOptions {
            input_layout: Some("5.1"),
            input_channel_labels: &[],
        },
    )
    .unwrap();
    assert_eq!(scene.info.source_kind, SourceKind::ChannelBed);
    assert_eq!(scene.snapshot.warnings.len(), 1);
    // "auto" keeps the ADM route.
    let scene = import(
        adm.path(),
        ImportOptions {
            input_layout: Some("AUTO"),
            input_channel_labels: &[],
        },
    )
    .unwrap();
    assert_eq!(scene.info.source_kind, SourceKind::Adm);
}

#[test]
fn channel_bed_errors_keep_their_messages_and_locators() {
    let mismatch = wav(2, Some(0x3F), None, None);
    let error = import(mismatch.path(), ImportOptions::default()).unwrap_err();
    assert_eq!(error.context, Context::Input);
    assert_eq!(
        error.message,
        "WAVE channel mask 0x3F contains 6 positions but the file has 2 channels"
    );

    let error = import(
        mismatch.path(),
        ImportOptions {
            input_layout: Some("4.0"),
            input_channel_labels: &[],
        },
    )
    .unwrap_err();
    assert_eq!(error.context, Context::None);
    assert_eq!(
        error.message,
        "unknown input layout '4.0'; use mradm input-layouts to list presets"
    );

    let labels = ["L".to_owned(), "M+030".to_owned()];
    let error = import(
        mismatch.path(),
        ImportOptions {
            input_layout: None,
            input_channel_labels: &labels,
        },
    )
    .unwrap_err();
    assert_eq!((error.code, error.context), (1, Context::None));
    assert_eq!(error.message, "duplicate input speaker label 'M+030'");

    let not_wave = Temp::new();
    std::fs::write(&not_wave.0, b"not a wave file").unwrap();
    let error = import(not_wave.path(), ImportOptions::default()).unwrap_err();
    assert_eq!((error.code, error.context), (3, Context::Input));
    assert_eq!(error.message, "input is not a RIFF/RF64/BW64 WAVE file");
}

#[test]
fn ambiguous_catalog_labels_need_an_elevation_token() {
    let catalog = mradm_scene::input::channel_catalog();
    assert!(catalog.iter().any(|c| c.token == "U+110@30"));
    assert!(catalog.iter().any(|c| c.token == "U+110@45"));
    assert!(mradm_scene::input::resolve_input_channel_labels(&["U+110".to_owned()]).is_err());
}
