//! Fixtures and handles shared by the C ABI tests: ADM WAVE inputs authored with `mradm-testkit`,
//! scratch directories, and owning wrappers for contexts, options and results.
use crate::context::{adm_context_t, adm_create_context, adm_destroy_context};
use crate::options::{adm_create_render_options, adm_destroy_render_options, adm_render_options_t};
use crate::render::*;
use mradm_testkit::adm::*;
use mradm_testkit::fixtures::wire_channel;
use mradm_testkit::wave::{AudioId, Wave};
use std::ffi::{CStr, CString, c_char, c_void};
use std::fs::File;
use std::path::{Path, PathBuf};
use std::ptr;
use std::sync::atomic::{AtomicUsize, Ordering};

pub const OK: i32 = 0;
pub const INVALID_ARGUMENT: i32 = 1;
pub const UNSUPPORTED: i32 = 2;
pub const IO: i32 = 3;
pub const CANCELLED: i32 = 5;

pub const STAGE_UNKNOWN: i32 = 0;
pub const STAGE_POST_PROCESSING: i32 = 6;
pub const STAGE_FINISHED: i32 = 7;
pub const OPERATION_UNKNOWN: i32 = 0;
pub const OPERATION_RENDER_AUDIO: i32 = 7;
pub const OPERATION_FINISH: i32 = 17;

/// A directory unique to this call, removed with its contents on drop.
pub struct Dir(pub PathBuf);

impl Dir {
    pub fn new(tag: &str) -> Self {
        static NEXT: AtomicUsize = AtomicUsize::new(0);
        let n = NEXT.fetch_add(1, Ordering::Relaxed);
        // Exercise file paths and diagnostics in the caller's code page even with an ASCII TEMP.
        let suffix = ["中文", "café"]
            .into_iter()
            .find(|name| native_path(name.as_bytes()).is_some())
            .unwrap_or("ascii");
        let path = std::env::temp_dir().join(format!(
            "mradm-capi-{tag}-{}-{n}-{suffix}",
            std::process::id()
        ));
        let _ = std::fs::remove_dir_all(&path);
        std::fs::create_dir_all(&path).unwrap();
        Self(path)
    }

    pub fn file(&self, name: &str) -> PathBuf {
        self.0.join(name)
    }

    /// Whether a file whose name starts with `stem.purpose.` remains next to `path`.
    pub fn has_sidecar(&self, path: &Path, purpose: &str) -> bool {
        let stem = path.file_stem().unwrap().to_string_lossy();
        let prefix = format!("{stem}.{purpose}.");
        std::fs::read_dir(&self.0).unwrap().any(|e| {
            e.unwrap()
                .file_name()
                .to_string_lossy()
                .starts_with(&prefix)
        })
    }
}

impl Drop for Dir {
    fn drop(&mut self) {
        let _ = std::fs::remove_dir_all(&self.0);
    }
}

/// File rendering, preview, queries and Scene SOFA accept the caller's native narrow paths.
pub fn native_c_path(path: &Path) -> CString {
    let bytes = native_path(path.as_os_str().as_encoded_bytes()).unwrap_or_else(|| {
        panic!("cannot represent fixture path in the native file code page: {path:?}")
    });
    CString::new(bytes).unwrap()
}

/// One channel of `ty` with `block` under object `name`Object, content and programme.
fn single(ty: Type, kind: BlockKind, block: Properties, name: &str) -> (String, String) {
    let mut doc = Document::new();
    let channel = doc.channel(ty, [channel_name(format!("{name}CF"))]);
    doc.block(channel, kind, block);
    doc.add(channel);
    let pack = doc.pack(ty, [pack_name(format!("{name}PF"))]);
    doc.reference(pack, channel);
    doc.add(pack);
    let uid = wire_channel(&mut doc, channel, pack, name);
    let object = doc.object([object_name(format!("{name}Object"))]);
    doc.reference(object, uid);
    doc.add(object);
    let content = doc.content([content_name(format!("{name}Content"))]);
    doc.reference(content, object);
    doc.add(content);
    let programme = doc.programme([programme_name(format!("{name}Programme"))]);
    doc.reference(programme, content);
    doc.add(programme);
    doc.reassign_ids();
    (doc.xml(), doc.id(uid).to_owned())
}

fn write(path: &Path, (axml, uid): (String, String), rate: u32, frames: usize, value: f32) {
    let mut wave = Wave::new(1, rate, 24);
    wave.chna = Some(vec![AudioId::new(1, &uid, "", "")]);
    wave.axml = Some(axml);
    wave.write(path, &vec![value; frames]).unwrap();
}

/// The C++ `write_fixture`: one Object at az 30 / el 10 with gain 0.8 (or Cartesian (0, 1, 0.5)
/// with a 0.3 extent), 0.2 s of 0.5.
pub fn object_input(dir: &Dir, rate: u32, cartesian: bool) -> PathBuf {
    let block = if cartesian {
        let mut b = objects_block([cartesian_position([x(0.0f32), y(1.0f32), z(0.5f32)])]);
        b.set(width(0.3f32));
        b.set(height(0.3f32));
        b.set(depth(0.3f32));
        b
    } else {
        objects_block([spherical([azimuth(30.0f32), elevation(10.0f32)])])
    };
    let block = block.with([gain_linear(0.8)]);
    let path = dir.file(&format!("object-{rate}-{cartesian}.wav"));
    let doc = single(Type::Objects, BlockKind::Objects, block, "Test");
    write(&path, doc, rate, rate as usize / 5, 0.5);
    path
}

/// The C++ `write_fixture_1s`: one Object straight ahead with gain 1, 1 s of 0.5 at 48 kHz.
pub fn object_input_1s(dir: &Dir) -> PathBuf {
    let block =
        objects_block([spherical([azimuth(0.0f32), elevation(0.0f32)])]).with([gain_linear(1.0)]);
    let path = dir.file("object-1s.wav");
    let doc = single(Type::Objects, BlockKind::Objects, block, "Test");
    write(&path, doc, 48000, 48000, 0.5);
    path
}

/// The C++ `write_direct_speakers_fixture`: one DirectSpeakers channel M+000, 4096 frames of 0.25.
pub fn direct_speakers_input(dir: &Dir) -> PathBuf {
    let mut block = direct_speakers_block([spherical([
        azimuth(0.0f32),
        elevation(0.0f32),
        distance(1.0f32),
    ])]);
    block.add(speaker_label("M+000"));
    let path = dir.file("direct-speakers.wav");
    let doc = single(
        Type::DirectSpeakers,
        BlockKind::DirectSpeakers,
        block,
        "Matrix",
    );
    write(&path, doc, 48000, 4096, 0.25);
    path
}

/// The channel and frame counts of a WAVE output.
pub fn wav_shape(path: &Path) -> (usize, u64) {
    let reader =
        mradm_wav::Reader::new(std::io::BufReader::new(File::open(path).unwrap())).unwrap();
    let info = reader.info();
    (usize::from(info.format.channels), info.frames)
}

pub struct Context(pub *mut adm_context_t);

impl Context {
    pub fn new() -> Self {
        let context = adm_create_context();
        assert!(!context.is_null());
        Self(context)
    }

    pub fn has_error(&self) -> bool {
        let message = unsafe { crate::context::adm_context_last_error_message(self.0) };
        !message.is_null() && unsafe { *message != 0 }
    }
}

impl Drop for Context {
    fn drop(&mut self) {
        unsafe { adm_destroy_context(self.0) };
    }
}

pub struct Options(pub *mut adm_render_options_t);

impl Options {
    pub fn new() -> Self {
        let options = adm_create_render_options();
        assert!(!options.is_null());
        Self(options)
    }
}

impl Drop for Options {
    fn drop(&mut self) {
        unsafe { adm_destroy_render_options(self.0) };
    }
}

pub unsafe fn text(s: *const c_char) -> Option<String> {
    (!s.is_null()).then(|| unsafe { CStr::from_ptr(s) }.to_str().unwrap().to_owned())
}

/// A string the library allocated, freed with `adm_free_string`.
pub unsafe fn taken(s: *mut c_char) -> Option<String> {
    let value = unsafe { text(s) };
    unsafe { crate::string::adm_free_string(s) };
    value
}

/// A render result, destroyed on drop.
pub struct Res(pub *mut adm_render_result_t);

pub struct Log {
    pub level: i32,
    pub module: String,
    pub message: String,
}

impl Res {
    pub fn output_path(&self) -> Option<&CStr> {
        let path = unsafe { adm_render_result_output_path(self.0) };
        (!path.is_null()).then(|| unsafe { CStr::from_ptr(path) })
    }
    pub fn code(&self) -> i32 {
        unsafe { adm_render_result_error_code(self.0) }
    }
    pub fn message(&self) -> String {
        unsafe { text(adm_render_result_message(self.0)) }.unwrap()
    }
    pub fn report(&self) -> Option<String> {
        unsafe { text(adm_render_result_semantic_report_json(self.0)) }
    }
    pub fn lufs(&self) -> Option<f64> {
        let mut v = 0.0;
        (unsafe { adm_render_result_loudness_lufs(self.0, &mut v) } == 1).then_some(v)
    }
    pub fn peak(&self) -> Option<f64> {
        let mut v = 0.0;
        (unsafe { adm_render_result_peak_dbtp(self.0, &mut v) } == 1).then_some(v)
    }
    pub fn logs(&self) -> Vec<Log> {
        (0..unsafe { adm_render_result_log_count(self.0) })
            .map(|i| {
                let (mut level, mut module, mut message) = (-1, ptr::null(), ptr::null());
                assert_eq!(
                    unsafe {
                        adm_render_result_log_entry(
                            self.0,
                            i,
                            &mut level,
                            &mut module,
                            &mut message,
                        )
                    },
                    1
                );
                unsafe {
                    Log {
                        level,
                        module: text(module).unwrap(),
                        message: text(message).unwrap(),
                    }
                }
            })
            .collect()
    }
    pub fn logs_contain(&self, needle: &str) -> bool {
        self.logs().iter().any(|l| l.message.contains(needle))
    }
}

impl Drop for Res {
    fn drop(&mut self) {
        unsafe { adm_destroy_render_result(self.0) };
    }
}

/// `adm_render_file_ex` with no progress callback.
pub fn render(
    context: &Context,
    input: &Path,
    output: &Path,
    options: Option<&Options>,
) -> (i32, Res) {
    let mut result = ptr::null_mut();
    let code = unsafe {
        adm_render_file_ex(
            context.0,
            native_c_path(input).as_ptr(),
            native_c_path(output).as_ptr(),
            options.map_or(ptr::null(), |o| o.0),
            None,
            ptr::null_mut(),
            &mut result,
        )
    };
    let result = Res(result);
    if code != OK && !result.0.is_null() {
        eprintln!("render {}: {code} {}", output.display(), result.message());
    }
    (code, result)
}

/// One progress v2 event, copied out of the callback.
#[derive(Clone, Debug)]
pub struct Event {
    pub struct_size: u32,
    pub stage: i32,
    pub operation: i32,
    pub overall: f64,
    pub stage_fraction: f64,
    pub total_frames: u64,
    pub message: Option<String>,
}

pub unsafe extern "C" fn collect_v2(event: *const adm_progress_event_v2_t, user: *mut c_void) {
    let events = unsafe { &mut *user.cast::<Vec<Event>>() };
    let e = unsafe { &*event };
    events.push(Event {
        struct_size: e.struct_size,
        stage: e.stage,
        operation: e.operation,
        overall: e.overall_fraction,
        stage_fraction: e.stage_fraction,
        total_frames: e.total_frames,
        message: unsafe { text(e.message) },
    });
}

/// The progress v2 contract every render keeps: known stages and operations, a non-decreasing
/// overall fraction, render events with a frame total, and a final FINISH at ~1.
pub fn check_v2(events: &[Event], post_processing: bool) {
    assert!(!events.is_empty());
    let mut previous = -1.0;
    for e in events {
        assert_eq!(e.struct_size as usize, size_of::<adm_progress_event_v2_t>());
        assert!(e.message.is_some());
        assert_ne!(e.stage, STAGE_UNKNOWN);
        assert_ne!(e.operation, OPERATION_UNKNOWN);
        assert!(
            e.overall >= previous && (0.0..=1.0).contains(&e.overall),
            "{e:?}"
        );
        assert!((0.0..=1.0).contains(&e.stage_fraction));
        previous = e.overall;
    }
    assert!(
        events
            .iter()
            .any(|e| e.operation == OPERATION_RENDER_AUDIO && e.total_frames > 0)
    );
    if post_processing {
        assert!(events.iter().any(|e| e.stage == STAGE_POST_PROCESSING));
    }
    let last = events.last().unwrap();
    assert_eq!(
        (last.stage, last.operation),
        (STAGE_FINISHED, OPERATION_FINISH)
    );
    assert!(last.overall >= 0.99);
}
