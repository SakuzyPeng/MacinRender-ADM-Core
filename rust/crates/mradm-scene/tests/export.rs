//! Export writeback goldens (PCM consistency 4.4). `export-v1/expected.jsonl` was produced once by
//! the C++ implementation this module replaced (`metadata::patch_axml` at `c0a3720`) for every
//! scene without a policy and every case of `policy-v1/policies.jsonl` and
//! `export-v1/policies.jsonl`: the patched AXML's length and FNV-1a 64, or the error code and
//! message. Policies the parser rejects never reach the export. The 11 records marked
//! `"regenerated":"extent-adm-range"` were recomputed by Rust when the extent default became the
//! ADM ranges instead of a maximum of 1.
use mradm_adm::{Document, Snapshot};
use mradm_json::Value;
use mradm_scene::{RenderScene, policy, writeback};
use mradm_wav::{Chna, ChnaEntry, Format, SampleFormat, Writer, WriterOptions};
use std::collections::HashMap;
use std::fs::File;
use std::path::{Path, PathBuf};

fn tests() -> PathBuf {
    Path::new(env!("CARGO_MANIFEST_DIR")).join("tests")
}

fn fnv(bytes: &[u8]) -> String {
    let mut h: u64 = 0xcbf2_9ce4_8422_2325;
    for &b in bytes {
        h ^= u64::from(b);
        h = h.wrapping_mul(0x100_0000_01b3);
    }
    format!("{h:016x}")
}

fn fixed<const N: usize>(text: &str) -> [u8; N] {
    let mut out = [b' '; N];
    out[..text.len()].copy_from_slice(text.as_bytes());
    out
}

fn field<'a>(value: &'a Value, key: &str) -> Option<&'a Value> {
    value.as_object().unwrap().get(key)
}

fn text<'a>(value: &'a Value, key: &str) -> &'a str {
    field(value, key).unwrap().as_str().unwrap()
}

fn number(value: &Value, key: &str) -> i64 {
    field(value, key).unwrap().as_f64().unwrap() as i64
}

/// A 480-frame PCM24 BW64 file with the scene's AXML and CHNA, and the AXML bytes.
fn scene_wav(scenes: &Path, name: &str, out: &Path) -> (PathBuf, Vec<u8>) {
    let meta =
        mradm_json::parse(&std::fs::read(scenes.join(format!("{name}.json"))).unwrap()).unwrap();
    let channels = number(&meta, "channels") as u16;
    let entries = field(&meta, "chna")
        .unwrap()
        .as_array()
        .unwrap()
        .iter()
        .map(|e| {
            let e = e.as_array().unwrap();
            ChnaEntry {
                track_index: e[0].as_f64().unwrap() as u16,
                uid: fixed::<12>(e[1].as_str().unwrap()),
                track_format: fixed::<14>(e[2].as_str().unwrap()),
                pack_format: fixed::<11>(e[3].as_str().unwrap()),
                pad: 0,
            }
        })
        .collect::<Vec<_>>();
    let xml = std::fs::read(scenes.join(format!("{name}.xml"))).unwrap();
    let path = out.join(format!("{name}.wav"));
    let format = Format {
        channels,
        sample_rate: number(&meta, "sample_rate") as u32,
        sample_format: SampleFormat::Pcm24,
        channel_mask: None,
    };
    let options = WriterOptions {
        container: None,
        axml: Some(xml.clone()),
        chna: Some(Chna {
            tracks: entries.len() as u16,
            entries,
        }),
    };
    let mut writer = Writer::new(File::create(&path).unwrap(), format, options).unwrap();
    writer
        .write_frames(&vec![0.0; usize::from(channels) * 480])
        .unwrap();
    writer.finish().unwrap();
    (path, xml)
}

struct Corpus {
    scenes: PathBuf,
    cases: Vec<Value>,
    wavs: HashMap<String, (PathBuf, Vec<u8>)>,
}

impl Corpus {
    fn new(name: &str) -> Self {
        let cases = std::fs::read_to_string(tests().join(name).join("policies.jsonl"))
            .unwrap()
            .lines()
            .map(|l| mradm_json::parse(l.as_bytes()).unwrap())
            .collect();
        Self {
            scenes: tests().join(name).join("scenes"),
            cases,
            wavs: HashMap::new(),
        }
    }

    fn scene(&mut self, name: &str, temp: &Path) -> (RenderScene, Vec<u8>) {
        let scenes = self.scenes.clone();
        let (path, xml) = self
            .wavs
            .entry(name.to_owned())
            .or_insert_with(|| scene_wav(&scenes, name, temp));
        let scene = mradm_scene::import_adm(path.to_str().unwrap().as_bytes()).unwrap();
        (scene, xml.clone())
    }
}

#[test]
fn exports_reproduce_the_cpp_goldens() {
    let temp = std::env::temp_dir().join(format!("mradm-scene-export-{}", std::process::id()));
    let mut corpora = HashMap::new();
    let (mut changed, mut errors, mut rejected) = (0, 0, 0);
    let expected = std::fs::read_to_string(tests().join("export-v1/expected.jsonl")).unwrap();
    for line in expected.lines() {
        let want = mradm_json::parse(line.as_bytes()).unwrap();
        let corpus_name = text(&want, "corpus").to_owned();
        let dir = temp.join(&corpus_name);
        std::fs::create_dir_all(&dir).unwrap();
        let corpus = corpora
            .entry(corpus_name.clone())
            .or_insert_with(|| Corpus::new(&corpus_name));
        let (case, name) = match field(&want, "i") {
            Some(i) => {
                let case = corpus.cases[i.as_f64().unwrap() as usize].clone();
                let name = text(&case, "scene").to_owned();
                (Some(case), name)
            }
            None => (None, text(&want, "scene").to_owned()),
        };
        let what = format!(
            "{corpus_name} {name} {:?}",
            field(&want, "i").map(|v| v.as_f64())
        );
        let (mut scene, xml) = corpus.scene(&name, &dir);
        if let Some(text_value) = case
            .as_ref()
            .and_then(|c| field(c, "policy").unwrap().as_str())
        {
            match policy::parse(text_value.as_bytes()) {
                Ok(parsed) => {
                    scene.apply_policy(parsed);
                }
                Err(_) => {
                    assert!(field(&want, "rejected").is_some(), "{what} must parse");
                    rejected += 1;
                    continue;
                }
            }
        }
        assert!(
            field(&want, "rejected").is_none(),
            "{what} must be rejected"
        );
        match writeback::patched_axml(&scene, &xml) {
            Ok(patched) => {
                assert_eq!(
                    (patched.len() as i64, fnv(patched.as_bytes())),
                    (number(&want, "len"), text(&want, "fnv").to_owned()),
                    "{what}"
                );
                if patched.as_bytes() != xml {
                    changed += 1;
                }
            }
            Err(error) => {
                let want =
                    field(&want, "error").unwrap_or_else(|| panic!("{what}: {}", error.message));
                assert_eq!(
                    (i64::from(error.code), error.message.as_str()),
                    (number(want, "code"), text(want, "message")),
                    "{what}"
                );
                errors += 1;
            }
        }
    }
    assert_eq!((changed, errors, rejected), (282, 20, 160));
    std::fs::remove_dir_all(&temp).unwrap();
}

/// Edits no policy makes, written back and imported again: block gain and the DirectSpeakers
/// fields; a source the original scene disagrees with is refused.
#[test]
fn record_edits_round_trip_through_the_axml() {
    let temp = std::env::temp_dir().join(format!("mradm-scene-export-edit-{}", std::process::id()));
    std::fs::create_dir_all(&temp).unwrap();
    let mut corpus = Corpus::new("policy-v1");
    let reimport = |scene: &RenderScene, xml: &str| {
        let doc = Document::parse(xml).unwrap();
        Snapshot::extract(&doc, scene.info.sample_rate, &[]).unwrap()
    };

    let (mut scene, xml) = corpus.scene("policy-objects", &temp);
    scene.original = Some((scene.snapshot.clone(), scene.overlay.clone()));
    let before = scene.snapshot.object_blocks[0];
    scene.snapshot.object_blocks[0].gain = before.gain * 0.5;
    scene.snapshot.object_blocks[0].interpolation = 1200;
    let patched = writeback::patched_axml(&scene, &xml).unwrap();
    let after = reimport(&scene, &patched).object_blocks[0];
    assert!(
        (after.gain - before.gain * 0.5).abs() < 1e-6,
        "block gain written back"
    );
    assert_eq!(
        after.interpolation, 1200,
        "interpolationLength written back"
    );
    assert_eq!(after.position, before.position, "position unchanged");

    let (mut scene, xml) = corpus.scene("directspeakers", &temp);
    scene.original = Some((scene.snapshot.clone(), scene.overlay.clone()));
    scene.snapshot.direct_blocks[1].gain = 0.25;
    scene.snapshot.direct_blocks[1].head_locked = 1;
    let patched = writeback::patched_axml(&scene, &xml).unwrap();
    let after = reimport(&scene, &patched);
    assert!((after.direct_blocks[1].gain - 0.25).abs() < 1e-6);
    assert_eq!(after.direct_blocks[1].head_locked, 1);
    assert_eq!(
        after.direct_blocks[0].gain,
        scene.snapshot.direct_blocks[0].gain
    );

    let (mut scene, xml) = corpus.scene("policy-objects", &temp);
    let mut original = scene.snapshot.clone();
    original.object_blocks[0].diffuse += 0.125;
    scene.original = Some((original, scene.overlay.clone()));
    let error = writeback::patched_axml(&scene, &xml).unwrap_err();
    assert_eq!(
        (error.code, error.message.as_str()),
        (1, "原场景与源 AXML 语义不一致")
    );
    std::fs::remove_dir_all(&temp).unwrap();
}
