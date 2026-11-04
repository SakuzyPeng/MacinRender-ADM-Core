//! The backend preparation inputs shared by the offline and stream tests: policy-v1 scenes as
//! float32 BW64 files, a platform layer without the floating-point scope and CUDA, and requests.
use super::{field, fixed};
use mradm_dsp::room::loading::{self, Package};
use mradm_dsp::room::quality::Quality;
use mradm_dsp::room::screen::{Mapping, RoomScreenGeometry, RoomScreens};
use mradm_engine::backend::{
    self, BinauralSpreadMode, Host, Kind, Prepared, RoomRequest, Routing, Screens, SpeakerSpread,
};
use mradm_engine::request::{Error, Level};
use mradm_render::room::PolarConversion;
use mradm_render::room_runtime::{Config, Prepare, Resources, Scope};
use mradm_scene::RenderScene;
use mradm_wav::{Chna, ChnaEntry, Format, SampleFormat, Writer, WriterOptions};
use std::fs::File;
use std::path::{Path, PathBuf};
use std::sync::Arc;

pub fn repo() -> PathBuf {
    Path::new(env!("CARGO_MANIFEST_DIR")).join("../../..")
}

pub fn policy_scenes() -> PathBuf {
    Path::new(env!("CARGO_MANIFEST_DIR")).join("../mradm-scene/tests/policy-v1/scenes")
}

/// A test directory removed on drop.
pub struct Dir(pub PathBuf);
impl Dir {
    pub fn new(tag: &str) -> Self {
        let path = std::env::temp_dir().join(format!("mradm-engine-{tag}-{}", std::process::id()));
        let _ = std::fs::remove_dir_all(&path);
        std::fs::create_dir_all(&path).unwrap();
        Self(path)
    }
    pub fn file(&self, name: &str) -> PathBuf {
        self.0.join(name)
    }
}
impl Drop for Dir {
    fn drop(&mut self) {
        let _ = std::fs::remove_dir_all(&self.0);
    }
}

/// A policy-v1 scene as a float32 BW64 file of `frames` sine frames at `rate`.
pub fn scene_wav(name: &str, path: &Path, frames: usize, rate: u32) {
    let dir = policy_scenes();
    let meta =
        mradm_json::parse(&std::fs::read(dir.join(format!("{name}.json"))).unwrap()).unwrap();
    let channels = field(&meta, "channels").as_f64().unwrap() as u16;
    let entries = field(&meta, "chna")
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
    let format = Format {
        channels,
        sample_rate: rate,
        sample_format: SampleFormat::Float32,
        channel_mask: None,
    };
    let options = WriterOptions {
        container: None,
        axml: Some(std::fs::read(dir.join(format!("{name}.xml"))).unwrap()),
        chna: Some(Chna {
            tracks: entries.len() as u16,
            entries,
        }),
    };
    let mut writer = Writer::new(File::create(path).unwrap(), format, options).unwrap();
    let samples: Vec<f32> = (0..frames * usize::from(channels))
        .map(|i| 0.25 * ((i as f32) * 0.013).sin())
        .collect();
    writer.write_frames(&samples).unwrap();
    writer.finish().unwrap();
}

pub fn import(path: &Path) -> RenderScene {
    mradm_scene::import(path.to_str().unwrap().as_bytes(), Default::default()).unwrap()
}

pub fn scope(call: &mut dyn FnMut() -> mradm_dsp::Result<()>) -> Result<(), (i32, &'static str)> {
    call().map_err(|e| match e {
        mradm_dsp::Error::InvalidArgument(s) => (1, s),
        mradm_dsp::Error::Unsupported(s) => (2, s),
        mradm_dsp::Error::Io(s) => (3, s),
        mradm_dsp::Error::RenderFailed(s) => (4, s),
    })
}

/// The platform layer without the floating-point scope and CUDA.
pub struct TestHost;
impl Host for TestHost {
    fn validate_screens(&self, screens: &RoomScreens) -> Result<(), (i32, String)> {
        scope(&mut || Mapping::new(*screens).map(drop)).map_err(|(c, m)| (c, m.to_owned()))
    }
    fn prepare_room(
        &self,
        package: &Package,
        hrir: &[u8],
        sofa: &[u8],
        quality: u32,
        _metres: f64,
    ) -> Result<Resources, (i32, String)> {
        let quality = Quality::try_from(quality).map_err(|_| (1, "quality".to_owned()))?;
        loading::prepare(package, hrir, sofa, None, quality)
            .map(|set| Resources { set, cuda: false })
            .map_err(|e| (1, e.to_string()))
    }
    fn room_polar(&self) -> PolarConversion {
        loading::polar
    }
    fn room_scope(&self) -> Scope {
        scope
    }
    fn room_switch(&self) -> Prepare {
        Arc::new(|_: &Config, reuse: &Resources| {
            Ok(Resources {
                set: Arc::clone(&reuse.set),
                cuda: false,
            })
        })
    }
}

pub fn request<'a>(kind: Kind, layout: &'a [u8], input: &'a [u8]) -> backend::Request<'a> {
    backend::Request {
        kind,
        layout_id: layout,
        apple_geometry: false,
        routing: Routing::Automatic,
        matrix: None,
        speaker_spread: SpeakerSpread::Automatic,
        binaural_spread: BinauralSpreadMode::Automatic,
        triple_balance_d: false,
        split_power: false,
        smoothing_frames: 0,
        default_interp_ms: 5,
        input_path: input,
        sofa: None,
        screens: Screens {
            has_reference: false,
            reference: RoomScreenGeometry {
                cartesian: 0,
                reserved: 0,
                aspect_ratio: 1.78,
                centre: [0., 0., 1.],
                width: 58.,
            },
            has_reproduction: false,
            reproduction: RoomScreenGeometry::default(),
        },
        room: RoomRequest {
            package: None,
            hrir: b"",
            metres: 0.,
            quality: 0,
        },
    }
}

pub fn prepared(
    scene: &RenderScene,
    r: &backend::Request,
) -> (Vec<(Level, String)>, Arc<Prepared>) {
    let p = backend::prepare(scene, r, &TestHost);
    let logs = p
        .outcome
        .logs
        .iter()
        .map(|l| (l.level, l.message.clone()))
        .collect();
    (logs, p.outcome.result.unwrap())
}

pub fn rejection(scene: &RenderScene, r: &backend::Request) -> (Vec<String>, Error) {
    let p = backend::prepare(scene, r, &TestHost);
    let logs = p.outcome.logs.iter().map(|l| l.message.clone()).collect();
    let Err(e) = p.outcome.result else {
        panic!(
            "{:?} {} prepared",
            r.kind,
            String::from_utf8_lossy(r.layout_id)
        );
    };
    (logs, e)
}
