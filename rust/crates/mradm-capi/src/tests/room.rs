//! The room-binaural Scene stream through the C ABI (from `room_render_test`, formerly driven by
//! `room_fixture.py`, whose bundle is `tests/fixtures/room/synthetic`): room loading and
//! inspection, both qualities, the screen and quality options, distance collections and SOFA,
//! the Scene session with its tail and room status, and an HRIR / SOFA conflict.
use super::common::*;
use crate::options::*;
use crate::room::*;
use crate::scene::*;
use mradm_json::Value;
use std::ffi::{CStr, CString};
use std::path::{Path, PathBuf};
use std::ptr;

const SCREEN: &CStr = c"{\"schema\":\"mradm.screen-config\",\"version\":1,\"reproduction\":{\"coordinates\":\"polar\",\"centre\":[0,0,1],\"width\":90,\"aspect_ratio\":1.78}}";
const EXACT: i32 = 0;
const REALTIME: i32 = 1;
const ROOM_BINAURAL: i32 = 8;

fn fixtures() -> PathBuf {
    Path::new(env!("CARGO_MANIFEST_DIR")).join("../../../tests/fixtures")
}

/// The synthetic bundle copied into `dir`, with `bad.json` (a corrupt asset hash) beside it.
fn bundle(dir: &Dir) -> PathBuf {
    let source = fixtures().join("room/synthetic");
    for entry in std::fs::read_dir(&source).unwrap() {
        let entry = entry.unwrap();
        std::fs::copy(entry.path(), dir.file(entry.file_name().to_str().unwrap())).unwrap();
    }
    let room = std::fs::read_to_string(dir.file("room.json")).unwrap();
    let eq = room.find("\"eq\"").unwrap();
    let hash = eq + room[eq..].find("\"sha256\": \"").unwrap() + "\"sha256\": \"".len();
    let bad = format!("{}{}{}", &room[..hash], "0".repeat(64), &room[hash + 64..]);
    std::fs::write(
        dir.file("bad.json"),
        bad.replace("\"synthetic\"", "\"bad\""),
    )
    .unwrap();
    dir.0.clone()
}

fn json(document: String) -> Value {
    mradm_json::parse(document.as_bytes()).unwrap()
}

fn get<'a>(value: &'a Value, key: &str) -> &'a Value {
    mradm_json::at(value, key).unwrap()
}

fn number(value: &Value, key: &str) -> f64 {
    match get(value, key) {
        Value::Unsigned(v) => *v as f64,
        Value::Integer(v) => *v as f64,
        Value::Float(v) => *v,
        other => panic!("{key}: {}", other.type_name()),
    }
}

fn string<'a>(value: &'a Value, key: &str) -> &'a str {
    get(value, key).as_str().unwrap()
}

/// A loaded room, destroyed on drop.
struct Room {
    context: Context,
    room: *mut adm_room_t,
}

impl Room {
    fn load(manifest: &Path) -> Self {
        let context = Context::new();
        let mut room = ptr::null_mut();
        assert_eq!(
            unsafe { adm_room_load(context.0, c(manifest).as_ptr(), &mut room) },
            OK
        );
        Self { context, room }
    }

    fn config(&self, hrir: Option<&CString>) -> adm_room_config_t {
        adm_room_config_t {
            struct_size: size_of::<adm_room_config_t>() as u32,
            reserved: 0,
            room: self.room,
            hrir_manifest: hrir.map_or(ptr::null(), |h| h.as_ptr()),
            metres_per_unit: 1.5,
        }
    }

    /// Destroys the room handle early; the Scene sessions created from it keep its assets.
    fn release(&mut self) {
        unsafe { adm_room_destroy(self.room) };
        self.room = ptr::null_mut();
    }

    fn inspect(&self, hrir: Option<&CString>, quality: i32) -> Value {
        let config = self.config(hrir);
        let mut report = ptr::null_mut();
        let code = unsafe {
            if quality == EXACT {
                adm_room_inspect(self.context.0, &config, &mut report)
            } else {
                adm_room_inspect_with_quality(self.context.0, &config, quality, &mut report)
            }
        };
        assert_eq!(code, OK);
        json(unsafe { taken(report) }.unwrap())
    }
}

impl Drop for Room {
    fn drop(&mut self) {
        unsafe { adm_room_destroy(self.room) };
    }
}

fn scene_config(output_rate: u32, sofa: Option<&CString>) -> adm_scene_stream_config_t {
    // SAFETY: plain data whose all-zero value is valid.
    let mut config: adm_scene_stream_config_t = unsafe { std::mem::zeroed() };
    config.struct_size = size_of::<adm_scene_stream_config_t>() as u32;
    config.rendering.struct_size = size_of::<adm_scene_renderer_config_t>() as u32;
    config.rendering.renderer = ROOM_BINAURAL;
    config.rendering.output_layout = c"binaural".as_ptr();
    config.rendering.sofa_path = sofa.map_or(ptr::null(), |s| s.as_ptr());
    config.input_sample_rate = 48000;
    config.output_sample_rate = output_rate;
    config.output_ring_frames = 32768;
    config.startup_watermark_frames = 1;
    config
}

#[test]
fn rooms_load_and_options_check_screen_and_quality() {
    let dir = Dir::new("room-options");
    let root = bundle(&dir);
    let context = Context::new();
    let mut bad = ptr::null_mut();
    assert_ne!(
        unsafe { adm_room_load(context.0, c(&root.join("bad.json")).as_ptr(), &mut bad) },
        OK
    );
    assert!(bad.is_null(), "a corrupt hash is rejected");
    let options = Options::new();
    unsafe {
        assert_eq!(
            adm_render_options_set_screen_config_json(options.0, SCREEN.as_ptr()),
            OK
        );
        assert_eq!(
            adm_render_options_set_screen_config_json(options.0, c"{}".as_ptr()),
            INVALID_ARGUMENT
        );
        assert_eq!(
            adm_render_options_set_screen_config_json(options.0, ptr::null()),
            OK
        );
        assert_eq!(adm_render_options_set_room_quality(options.0, REALTIME), OK);
        assert_eq!(
            adm_render_options_set_room_quality(options.0, 2),
            INVALID_ARGUMENT
        );
        assert_eq!(adm_render_options_set_room(options.0, ptr::null()), OK);
    }
}

/// Both qualities prepare from the same raw HRIR and report their processing policy and the
/// session-fixed reflection pose budget; a distance collection reports every layer and a direct
/// SOFA uses the native preparation.
#[test]
fn inspections_report_the_quality_and_hrir() {
    let dir = Dir::new("room-inspect");
    let root = bundle(&dir);
    let room = Room::load(&root.join("room.json"));
    let exact = room.inspect(None, EXACT);
    let fast = room.inspect(None, REALTIME);
    assert_eq!(
        (string(&exact, "quality"), string(&fast, "quality")),
        ("exact", "realtime")
    );
    assert_eq!(string(&exact, "hrir_hash"), string(&fast, "hrir_hash"));
    assert_eq!(string(&exact, "processing_policy"), "room-exact-v1");
    assert_eq!(string(&fast, "processing_policy"), "room-realtime-v2");
    assert_eq!(number(&exact, "reflection_pose_limit_degrees"), 0.0);
    assert_eq!(number(&exact, "reflection_pose_update_frames"), 0.0);
    assert_eq!(number(&fast, "reflection_pose_limit_degrees"), 0.5);
    assert_eq!(number(&fast, "reflection_pose_update_frames"), 1024.0);
    let collection = c(&fixtures().join("sofa/distance/distance_index.json"));
    let info = room.inspect(Some(&collection), EXACT);
    assert_eq!(
        get(get(&info, "hrtf_collection"), "radii_m")
            .as_array()
            .unwrap()
            .len(),
        2
    );
    let sofa = c(&fixtures().join("sofa/simple.sofa"));
    assert_eq!(
        number(&room.inspect(Some(&sofa), EXACT), "hrir_directions"),
        6.0
    );
}

/// One room Scene session: an invalid quality is refused, the room switch inherits the session
/// quality, the session keeps the room after its handle goes, the output carries the whole tail
/// rounded once, and the room status and source report the material, quality and real distance.
fn scene(root: &Path, output_rate: u32, screen: bool, hrir: Option<&Path>, quality: i32) {
    let mut room = Room::load(&root.join("room.json"));
    let hrir_text = hrir.map(c);
    let inspection = room.inspect(hrir_text.as_ref(), quality);
    let expected_quality = if quality == EXACT {
        "exact"
    } else {
        "realtime"
    };
    assert_eq!(string(&inspection, "quality"), expected_quality);
    let tail = number(&inspection, "tail_frames") as u64;
    let is_sofa = hrir.is_some_and(|p| p.extension().is_some_and(|e| e == "sofa"));
    let config = scene_config(output_rate, if is_sofa { hrir_text.as_ref() } else { None });
    let room_config = room.config(if is_sofa { None } else { hrir_text.as_ref() });
    let mut stream = ptr::null_mut();
    unsafe {
        assert_eq!(
            adm_create_scene_stream_with_room_quality(
                room.context.0,
                &config,
                &room_config,
                99,
                ptr::null(),
                &mut stream
            ),
            INVALID_ARGUMENT
        );
        assert!(stream.is_null());
        let screen_json = if screen { SCREEN.as_ptr() } else { ptr::null() };
        let code = if quality == EXACT {
            adm_create_scene_stream_with_room_ex(
                room.context.0,
                &config,
                &room_config,
                screen_json,
                &mut stream,
            )
        } else {
            adm_create_scene_stream_with_room_quality(
                room.context.0,
                &config,
                &room_config,
                quality,
                screen_json,
                &mut stream,
            )
        };
        assert_eq!(code, OK);
        let mut request = 0u64;
        let mut error = ptr::null_mut();
        assert_eq!(
            adm_scene_stream_switch_room(stream, &room_config, &mut request, &mut error),
            OK
        );
        crate::string::adm_free_string(error);
    }
    // The session retains the room and HRIR assets.
    room.release();

    unsafe {
        assert_eq!(adm_scene_stream_begin_epoch(stream, 1, 0), OK);
        let mut d: adm_scene_element_descriptor_t = std::mem::zeroed();
        d.struct_size = size_of::<adm_scene_element_descriptor_t>() as u32;
        d.element_id = 7;
        d.has_position = 1;
        // A distance of 2 units (3 m) must survive the Scene boundary.
        (d.position_x, d.position_y) = if screen {
            (-0.969_619_24_f64 as f32, 1.749_239_44_f64 as f32)
        } else {
            (0.0, 2.0)
        };
        assert_eq!(
            adm_scene_stream_configure_generation(stream, 1, 1, &d, 1),
            OK
        );
        let mut samples = [0.0f32; 1001];
        samples[0] = 0.1;
        samples[913] = 0.05;
        let mut plane: adm_scene_pcm_plane_t = std::mem::zeroed();
        plane.struct_size = size_of::<adm_scene_pcm_plane_t>() as u32;
        plane.element_id = 7;
        plane.samples = samples.as_ptr();
        plane.sample_count = samples.len() as u32;
        plane.stride = 1;
        plane.has_signal = 1;
        let mut initial: adm_scene_initial_state_t = std::mem::zeroed();
        initial.struct_size = size_of::<adm_scene_initial_state_t>() as u32;
        initial.element_id = 7;
        initial.state.struct_size = size_of::<adm_scene_object_state_t>() as u32;
        initial.state.valid_fields = 1 | 2 | 4;
        initial.state.active = 1;
        initial.state.linear_gain = 1.0;
        initial.state.position_x = d.position_x;
        initial.state.position_y = d.position_y;
        if screen {
            initial.state.valid_fields |= 1 << 7;
            initial.state.screen_reference = 1;
        }
        let mut frame: adm_scene_frame_t = std::mem::zeroed();
        frame.struct_size = size_of::<adm_scene_frame_t>() as u32;
        frame.flags = 1;
        frame.epoch_id = 1;
        frame.generation_id = 1;
        frame.duration_samples = samples.len() as u32;
        frame.pcm_count = 1;
        frame.pcm = &plane;
        frame.initial_state_count = 1;
        frame.initial_states = &initial;
        let mut submitted = -1;
        assert_eq!(
            adm_scene_stream_submit_frame(stream, &frame, 1000, &mut submitted),
            OK
        );
        assert_eq!(submitted, 0);
        assert_eq!(
            adm_scene_stream_signal_end(stream, 1, samples.len() as i64),
            OK
        );
        let (mut frames, mut ended, mut signal) = (0u64, false, false);
        for _ in 0..5000 {
            let mut output = [0.0f32; 514];
            let mut pulled: adm_scene_pull_result_t = std::mem::zeroed();
            pulled.struct_size = size_of::<adm_scene_pull_result_t>() as u32;
            assert_eq!(
                adm_scene_stream_pull(stream, output.as_mut_ptr(), 257, &mut pulled),
                OK
            );
            assert_eq!(pulled.flags & 8, 0, "the room worker failed");
            frames += u64::from(pulled.media_frames);
            signal |= output.iter().any(|v| v.abs() > 1e-6);
            ended = pulled.flags & 4 != 0;
            if ended {
                break;
            }
            if pulled.media_frames == 0 {
                std::thread::sleep(std::time::Duration::from_millis(1));
            }
        }
        let expected = ((samples.len() as u64 + tail) * u64::from(output_rate)).div_ceil(48000);
        assert!(ended && signal);
        assert_eq!(frames, expected, "the whole tail, rounded once");
        let mut report = ptr::null_mut();
        assert_eq!(adm_scene_stream_room_status_json(stream, &mut report), OK);
        let status = json(taken(report).unwrap());
        assert_eq!(number(&status, "material_frames"), samples.len() as f64);
        assert_eq!(string(&status, "quality"), expected_quality);
        assert_eq!(
            number(&status, "reflection_update_frames"),
            number(&inspection, "reflection_update_frames")
        );
        let mut report = ptr::null_mut();
        assert_eq!(
            adm_scene_stream_room_source_json(stream, 7, &mut report),
            OK
        );
        let source = json(taken(report).unwrap());
        assert!((number(&source, "distance_metres") - 3.0).abs() < 1e-6);
        if screen {
            let direction = get(&source, "direction").as_array().unwrap();
            let value = |v: &Value| match v {
                Value::Float(f) => *f,
                Value::Integer(i) => *i as f64,
                Value::Unsigned(u) => *u as f64,
                _ => panic!(),
            };
            assert!(
                (value(&direction[0]) - value(&direction[1])).abs() < 1e-6,
                "the screen edge maps to 45 degrees"
            );
        }
        adm_destroy_scene_stream(stream);
    }
}

#[test]
fn room_scene_sessions_render_with_the_default_hrir() {
    let dir = Dir::new("room-scene");
    let root = bundle(&dir);
    scene(&root, 48000, false, None, EXACT);
    scene(&root, 44100, false, None, EXACT);
    scene(&root, 48000, true, None, EXACT);
    scene(&root, 48000, false, None, REALTIME);
    scene(&root, 44100, true, None, REALTIME);
}

#[test]
fn room_scene_sessions_render_with_a_distance_collection() {
    let dir = Dir::new("room-collection");
    let root = bundle(&dir);
    let collection = fixtures().join("sofa/distance/distance_index.json");
    scene(&root, 48000, false, Some(&collection), EXACT);
    scene(&root, 44100, true, Some(&collection), REALTIME);
}

/// With the default HRIR moved away, a direct SOFA prepares natively; an explicit prepared bank
/// must still authenticate an accompanying SOFA.
#[test]
fn room_scene_sessions_render_with_a_sofa_and_reject_a_conflict() {
    let dir = Dir::new("room-sofa");
    let root = bundle(&dir);
    std::fs::rename(root.join("hrir.json"), root.join("stored-hrir.json")).unwrap();
    let sofa = fixtures().join("sofa/simple.sofa");
    scene(&root, 48000, false, Some(&sofa), EXACT);
    scene(&root, 48000, false, Some(&sofa), REALTIME);
    scene(
        &root,
        44100,
        false,
        Some(&fixtures().join("sofa/room-delayed.sofa")),
        EXACT,
    );

    let room = Room::load(&root.join("room.json"));
    let stored = c(&root.join("stored-hrir.json"));
    let sofa = c(&sofa);
    let config = scene_config(48000, Some(&sofa));
    let room_config = room.config(Some(&stored));
    let mut stream = ptr::null_mut();
    assert_ne!(
        unsafe {
            adm_create_scene_stream_with_room_ex(
                room.context.0,
                &config,
                &room_config,
                ptr::null(),
                &mut stream,
            )
        },
        OK
    );
    assert!(stream.is_null());
}
