//! Private render-scene boundary (PCM consistency 4.3, the semantic policy, DirectSpeakers matrix
//! and screen configuration since 4.4). Layout and
//! input-preset tables are process-lifetime views of `mradm-scene` static data and are never freed;
//! an imported scene is owned by its handle and its views are borrowed until the next call that
//! changes it (`mradm_scene_apply_policy`, `mradm_scene_report`, `mradm_scene_template`) or until
//! `mradm_scene_destroy`.
use crate::adm::{Text, View};
use crate::boundary;
use mradm_dsp::{Error, room::screen::RoomScreenGeometry};
use mradm_scene::{
    Context, ImportOptions, RenderScene, SourceKind, direct_matrix,
    input::{InputChannel, channel_catalog, input_layouts, resolve_input_channel_labels},
    layouts::{Geometry, Layout, layouts},
    live_policy,
    policy::{self, UserLevel},
    report, screen, writeback,
};
use std::{
    borrow::Cow,
    panic::{AssertUnwindSafe, catch_unwind},
    ptr, slice,
    sync::OnceLock,
};

#[repr(C)]
pub struct LayoutSpeaker {
    label: *const u8,
    label_len: usize,
    azimuth: f32,
    elevation: f32,
    is_lfe: u32,
    has_azimuth_range: u32,
    azimuth_range: [f32; 2],
    has_elevation_range: u32,
    elevation_range: [f32; 2],
}

#[repr(C)]
pub struct LayoutEntry {
    id: *const u8,
    id_len: usize,
    display_name: *const u8,
    display_name_len: usize,
    speakers: *const LayoutSpeaker,
    speakers_len: usize,
}

struct Table {
    // Owns the speaker records that `entries` point into.
    _speakers: Vec<Vec<LayoutSpeaker>>,
    entries: Vec<LayoutEntry>,
}
// The records only point at immutable process-lifetime data.
unsafe impl Send for Table {}
unsafe impl Sync for Table {}

fn table(source: &'static [Layout]) -> Table {
    let speakers: Vec<Vec<LayoutSpeaker>> = source
        .iter()
        .map(|layout| {
            layout
                .speakers
                .iter()
                .map(|s| LayoutSpeaker {
                    label: s.label.as_ptr(),
                    label_len: s.label.len(),
                    azimuth: s.azimuth,
                    elevation: s.elevation,
                    is_lfe: u32::from(s.is_lfe),
                    has_azimuth_range: u32::from(s.azimuth_range.is_some()),
                    azimuth_range: s.azimuth_range.unwrap_or_default(),
                    has_elevation_range: u32::from(s.elevation_range.is_some()),
                    elevation_range: s.elevation_range.unwrap_or_default(),
                })
                .collect()
        })
        .collect();
    let entries = source
        .iter()
        .zip(&speakers)
        .map(|(layout, records)| LayoutEntry {
            id: layout.id.as_ptr(),
            id_len: layout.id.len(),
            display_name: layout.display_name.as_ptr(),
            display_name_len: layout.display_name.len(),
            speakers: records.as_ptr(),
            speakers_len: records.len(),
        })
        .collect();
    Table {
        _speakers: speakers,
        entries,
    }
}

/// Geometry 0 is the standard ADM nominal profile, 1 the Apple effective-position profile.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn mradm_scene_layouts(
    geometry: u32,
    entries: *mut *const LayoutEntry,
    entries_len: *mut usize,
    message: *mut u8,
    capacity: usize,
) -> i32 {
    static STANDARD: OnceLock<Table> = OnceLock::new();
    static APPLE: OnceLock<Table> = OnceLock::new();
    boundary(message, capacity, || {
        if entries.is_null() || entries_len.is_null() {
            return Err(Error::InvalidArgument("Null layout table output"));
        }
        let table = match geometry {
            0 => STANDARD.get_or_init(|| table(layouts(Geometry::Standard))),
            1 => APPLE.get_or_init(|| table(layouts(Geometry::Apple))),
            _ => return Err(Error::InvalidArgument("Unknown speaker geometry")),
        };
        unsafe {
            *entries = table.entries.as_ptr();
            *entries_len = table.entries.len();
        }
        Ok(())
    })
}

#[repr(C)]
pub struct SceneImportInfo {
    sample_rate: u32,
    channels: u32,
    frames: u64,
    /// 0 = ADM, 1 = channel bed.
    source_kind: u32,
    input_layout: Text,
    input_channel_labels: *const Text,
    input_channel_labels_len: usize,
}

pub struct SceneHandle {
    scene: RenderScene,
    texts: Vec<Text>,
    labels: Vec<Text>,
    objects: Vec<SceneObjectState>,
    direct_blocks: Vec<SceneDirectState>,
    warnings: Vec<Text>,
    output: String,
}

/// `user_mute`: 0 unset, 1 false, 2 true.
#[repr(C)]
pub struct SceneObjectState {
    user_gain: f32,
    user_mute: u32,
    mute: u32,
}

#[repr(C)]
pub struct SceneDirectState {
    user_gain: f32,
    user_mute: u32,
    position_override: u32,
    has_position: u32,
}

/// The per-element state beside the records, aligned with the view's objects and DirectSpeakers
/// blocks.
#[repr(C)]
pub struct SceneOverlay {
    objects: *const SceneObjectState,
    objects_len: usize,
    direct_blocks: *const SceneDirectState,
    direct_blocks_len: usize,
}

#[repr(C)]
pub struct SceneReportOptions {
    renderer: Text,
    policy_path: Text,
    supports_channel_lock: u32,
    supports_object_divergence: u32,
    supports_diffuse: u32,
    hrtf_sources: *const Text,
    hrtf_sources_len: usize,
    renderer_effective: Text,
}

fn user_mute(user: UserLevel) -> u32 {
    match user.mute {
        None => 0,
        Some(false) => 1,
        Some(true) => 2,
    }
}

impl SceneHandle {
    pub(crate) fn scene(&self) -> &RenderScene {
        &self.scene
    }
    fn new(scene: RenderScene) -> Self {
        let mut handle = Self {
            texts: scene
                .snapshot
                .strings
                .iter()
                .map(|s| Text::new(s))
                .collect(),
            labels: scene
                .info
                .input_channel_labels
                .iter()
                .map(|s| Text::new(s))
                .collect(),
            scene,
            objects: Vec::new(),
            direct_blocks: Vec::new(),
            warnings: Vec::new(),
            output: String::new(),
        };
        handle.refresh();
        handle
    }

    /// Rebuilds the C records of the overlay and the warnings after the scene changed.
    fn refresh(&mut self) {
        let overlay = &self.scene.overlay;
        self.objects = overlay
            .objects
            .iter()
            .map(|o| SceneObjectState {
                user_gain: o.user.gain_multiplier,
                user_mute: user_mute(o.user),
                mute: u32::from(o.mute),
            })
            .collect();
        self.direct_blocks = overlay
            .direct_blocks
            .iter()
            .map(|b| SceneDirectState {
                user_gain: b.user.gain_multiplier,
                user_mute: user_mute(b.user),
                position_override: u32::from(b.position_override),
                has_position: u32::from(b.has_position),
            })
            .collect();
        self.warnings = self
            .scene
            .policy_warnings
            .iter()
            .map(|w| Text::new(w))
            .collect();
    }
}

#[repr(C)]
pub struct InputChannelRecord {
    token: Text,
    speaker_label: Text,
    azimuth: f32,
    elevation: f32,
    is_lfe: u32,
}

#[repr(C)]
pub struct InputLayoutRecord {
    id: Text,
    internal_id: Text,
    display_name: Text,
    wave_channel_mask: u32,
    channels: *const InputChannelRecord,
    channels_len: usize,
    wave_mask_channels: *const InputChannelRecord,
    wave_mask_channels_len: usize,
}

struct Static<T>(T);
// The records only point at immutable process-lifetime data.
unsafe impl<T> Send for Static<T> {}
unsafe impl<T> Sync for Static<T> {}

/// Like `boundary`, plus the locator kind: 0 none, 1 `input=<path>`, 2 `path=<path>`, 3 the
/// caller's source label.
fn scene_boundary_with_message_len(
    context: *mut u32,
    message: *mut u8,
    capacity: usize,
    message_len: *mut usize,
    f: impl FnOnce() -> mradm_scene::Result<()>,
) -> i32 {
    let error = match catch_unwind(AssertUnwindSafe(f)) {
        Ok(Ok(())) => None,
        Ok(Err(error)) => Some(error),
        Err(_) => Some(mradm_scene::Error::new(
            6,
            "Rust scene panic",
            Context::None,
        )),
    };
    if !context.is_null() {
        let kind = match error.as_ref().map(|e| e.context) {
            None | Some(Context::None) => 0,
            Some(Context::Input) => 1,
            Some(Context::Path) => 2,
            Some(Context::Label) => 3,
        };
        unsafe {
            *context = kind;
        }
    }
    let detail = error.as_ref().map_or("", |e| e.message.as_str());
    let mut written = 0;
    if !message.is_null() && capacity != 0 {
        let mut len = detail.len().min(capacity - 1);
        while !detail.is_char_boundary(len) {
            len -= 1;
        }
        unsafe {
            ptr::copy_nonoverlapping(detail.as_ptr(), message, len);
            *message.add(len) = 0;
        }
        written = len;
    }
    if !message_len.is_null() {
        unsafe {
            *message_len = written;
        }
    }
    error.map_or(0, |e| e.code)
}

fn scene_boundary(
    context: *mut u32,
    message: *mut u8,
    capacity: usize,
    f: impl FnOnce() -> mradm_scene::Result<()>,
) -> i32 {
    scene_boundary_with_message_len(context, message, capacity, ptr::null_mut(), f)
}

fn invalid(message: &str) -> mradm_scene::Error {
    mradm_scene::Error::invalid(message, Context::None)
}

unsafe fn bytes<'a, T>(data: *const T, len: usize) -> mradm_scene::Result<&'a [T]> {
    if len == 0 {
        return Ok(&[]);
    }
    if data.is_null()
        || !(data as usize).is_multiple_of(align_of::<T>())
        || len > isize::MAX as usize / size_of::<T>()
    {
        return Err(invalid("无效场景缓冲区"));
    }
    Ok(unsafe { slice::from_raw_parts(data, len) })
}

/// Labels and layout names are matched as ASCII; bytes that are not UTF-8 cannot match and are
/// reported with replacement characters.
unsafe fn lossy<'a>(value: Text) -> mradm_scene::Result<Cow<'a, str>> {
    Ok(String::from_utf8_lossy(unsafe {
        bytes(value.data, value.len)?
    }))
}

unsafe fn labels(data: *const Text, len: usize) -> mradm_scene::Result<Vec<String>> {
    unsafe { bytes(data, len)? }
        .iter()
        .map(|&label| Ok(unsafe { lossy(label)? }.into_owned()))
        .collect()
}

/// `routed` 0 imports ADM BW64 only; 1 routes to ADM or a channel bed by the options. `path` is
/// UTF-8 (the boundary converts the native spelling first).
#[unsafe(no_mangle)]
pub unsafe extern "C" fn mradm_scene_import(
    path: *const u8,
    path_len: usize,
    routed: u32,
    has_layout: u32,
    layout: Text,
    input_labels: *const Text,
    input_labels_len: usize,
    output: *mut *mut SceneHandle,
    context: *mut u32,
    message: *mut u8,
    capacity: usize,
) -> i32 {
    scene_boundary(context, message, capacity, || {
        if output.is_null() {
            return Err(invalid("缺少场景输出句柄"));
        }
        unsafe {
            *output = ptr::null_mut();
        }
        let path = unsafe { bytes(path, path_len)? };
        let scene = if routed == 0 {
            mradm_scene::import_adm(path)?
        } else {
            let layout = if has_layout != 0 {
                Some(unsafe { lossy(layout)? })
            } else {
                None
            };
            let labels = unsafe { labels(input_labels, input_labels_len)? };
            mradm_scene::import(
                path,
                ImportOptions {
                    input_layout: layout.as_deref(),
                    input_channel_labels: &labels,
                },
            )?
        };
        unsafe {
            *output = Box::into_raw(Box::new(SceneHandle::new(scene)));
        }
        Ok(())
    })
}

#[unsafe(no_mangle)]
pub unsafe extern "C" fn mradm_scene_view(
    handle: *const SceneHandle,
    info: *mut SceneImportInfo,
    view: *mut View,
    message: *mut u8,
    capacity: usize,
) -> i32 {
    scene_boundary(ptr::null_mut(), message, capacity, || {
        if handle.is_null() || info.is_null() || view.is_null() {
            return Err(invalid("无效场景 view 参数"));
        }
        let handle = unsafe { &*handle };
        let scene = &handle.scene.info;
        unsafe {
            *info = SceneImportInfo {
                sample_rate: scene.sample_rate,
                channels: u32::from(scene.channels),
                frames: scene.frames,
                source_kind: match scene.source_kind {
                    SourceKind::Adm => 0,
                    SourceKind::ChannelBed => 1,
                },
                input_layout: Text::new(&scene.input_layout),
                input_channel_labels: handle.labels.as_ptr(),
                input_channel_labels_len: handle.labels.len(),
            };
            *view = View::new(&handle.scene.snapshot, &handle.texts);
        }
        Ok(())
    })
}

#[unsafe(no_mangle)]
pub unsafe extern "C" fn mradm_scene_overlay(
    handle: *const SceneHandle,
    overlay: *mut SceneOverlay,
    message: *mut u8,
    capacity: usize,
) -> i32 {
    scene_boundary(ptr::null_mut(), message, capacity, || {
        if handle.is_null() || overlay.is_null() {
            return Err(invalid("无效场景 overlay 参数"));
        }
        let handle = unsafe { &*handle };
        unsafe {
            *overlay = SceneOverlay {
                objects: handle.objects.as_ptr(),
                objects_len: handle.objects.len(),
                direct_blocks: handle.direct_blocks.as_ptr(),
                direct_blocks_len: handle.direct_blocks.len(),
            };
        }
        Ok(())
    })
}

/// Parses `mradm.semantic-policy.v1` JSON and applies it to the scene. Errors carry context 3:
/// the caller attaches its source label (`<memory>` or the policy file path).
#[unsafe(no_mangle)]
pub unsafe extern "C" fn mradm_scene_apply_policy(
    handle: *mut SceneHandle,
    json: *const u8,
    json_len: usize,
    context: *mut u32,
    message: *mut u8,
    capacity: usize,
) -> i32 {
    scene_boundary(context, message, capacity, || {
        if handle.is_null() {
            return Err(invalid("无效场景句柄"));
        }
        let handle = unsafe { &mut *handle };
        let policy = policy::parse(unsafe { bytes(json, json_len)? })?;
        handle.scene.apply_policy(policy);
        handle.refresh();
        Ok(())
    })
}

/// The "rule did not match" warnings of the applied policy, in rule order.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn mradm_scene_policy_warnings(
    handle: *const SceneHandle,
    warnings: *mut *const Text,
    warnings_len: *mut usize,
    message: *mut u8,
    capacity: usize,
) -> i32 {
    scene_boundary(ptr::null_mut(), message, capacity, || {
        if handle.is_null() || warnings.is_null() || warnings_len.is_null() {
            return Err(invalid("无效场景 warnings 参数"));
        }
        let handle = unsafe { &*handle };
        unsafe {
            *warnings = handle.warnings.as_ptr();
            *warnings_len = handle.warnings.len();
        }
        Ok(())
    })
}

/// The semantic report (`mradm.semantic-report.v1`, nlohmann `dump(2)` bytes, no trailing newline).
#[unsafe(no_mangle)]
pub unsafe extern "C" fn mradm_scene_report(
    handle: *mut SceneHandle,
    options: *const SceneReportOptions,
    output: *mut Text,
    message: *mut u8,
    capacity: usize,
) -> i32 {
    scene_boundary(ptr::null_mut(), message, capacity, || {
        if handle.is_null() || options.is_null() || output.is_null() {
            return Err(invalid("无效语义报告参数"));
        }
        let handle = unsafe { &mut *handle };
        let o = unsafe { &*options };
        let text = |value: Text| -> mradm_scene::Result<String> {
            Ok(unsafe { lossy(value)? }.into_owned())
        };
        let options = report::ReportOptions {
            renderer: text(o.renderer)?,
            policy_path: text(o.policy_path)?,
            supports_channel_lock: o.supports_channel_lock != 0,
            supports_object_divergence: o.supports_object_divergence != 0,
            supports_diffuse: o.supports_diffuse != 0,
            hrtf_sources: unsafe { labels(o.hrtf_sources, o.hrtf_sources_len)? },
            renderer_effective: text(o.renderer_effective)?,
        };
        handle.output = report::report(&handle.scene, &options)?;
        unsafe {
            *output = Text::new(&handle.output);
        }
        Ok(())
    })
}

/// The neutral policy template of the scene (nlohmann `dump(2)` bytes, no trailing newline).
#[unsafe(no_mangle)]
pub unsafe extern "C" fn mradm_scene_template(
    handle: *mut SceneHandle,
    output: *mut Text,
    message: *mut u8,
    capacity: usize,
) -> i32 {
    scene_boundary(ptr::null_mut(), message, capacity, || {
        if handle.is_null() || output.is_null() {
            return Err(invalid("无效策略模板参数"));
        }
        let handle = unsafe { &mut *handle };
        handle.output = report::template(&handle.scene);
        unsafe {
            *output = Text::new(&handle.output);
        }
        Ok(())
    })
}

/// The source AXML `xml` with this scene's semantic edits patched in, held by the handle until
/// the next report, template or export call.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn mradm_scene_export_axml(
    handle: *mut SceneHandle,
    xml: *const u8,
    xml_len: usize,
    output: *mut Text,
    message: *mut u8,
    capacity: usize,
) -> i32 {
    scene_boundary(ptr::null_mut(), message, capacity, || {
        if handle.is_null() || output.is_null() {
            return Err(invalid("无效导出参数"));
        }
        let handle = unsafe { &mut *handle };
        handle.output = writeback::patched_axml(&handle.scene, unsafe { bytes(xml, xml_len)? })?;
        unsafe {
            *output = Text::new(&handle.output);
        }
        Ok(())
    })
}

#[unsafe(no_mangle)]
pub unsafe extern "C" fn mradm_scene_destroy(handle: *mut SceneHandle) {
    if !handle.is_null() {
        unsafe {
            drop(Box::from_raw(handle));
        }
    }
}

/// Writes the version-1 render-scene checkpoint `render-scene/offline.<stage>.i32` of this scene
/// (diagnostic builds only; a no-op otherwise).
#[unsafe(no_mangle)]
pub unsafe extern "C" fn mradm_scene_checkpoint(
    handle: *const SceneHandle,
    stage: Text,
    message: *mut u8,
    capacity: usize,
) -> i32 {
    scene_boundary(ptr::null_mut(), message, capacity, || {
        if handle.is_null() {
            return Err(invalid("无效场景句柄"));
        }
        let stage = unsafe { lossy(stage)? };
        mradm_scene::checkpoint::dump(&stage, unsafe { &(*handle).scene })
            .map_err(|e| mradm_scene::Error::io(e.to_string(), Context::None))
    })
}

fn channel_record(channel: &InputChannel) -> InputChannelRecord {
    InputChannelRecord {
        token: Text::new(&channel.token),
        speaker_label: Text::new(channel.speaker_label),
        azimuth: channel.azimuth,
        elevation: channel.elevation,
        is_lfe: u32::from(channel.is_lfe),
    }
}

/// Channel-bed presets in their documented order.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn mradm_scene_input_layouts(
    entries: *mut *const InputLayoutRecord,
    entries_len: *mut usize,
    message: *mut u8,
    capacity: usize,
) -> i32 {
    type Records = (Vec<Vec<InputChannelRecord>>, Vec<InputLayoutRecord>);
    static TABLE: OnceLock<Static<Records>> = OnceLock::new();
    scene_boundary(ptr::null_mut(), message, capacity, || {
        if entries.is_null() || entries_len.is_null() {
            return Err(invalid("Null input layout table output"));
        }
        let table = TABLE.get_or_init(|| {
            let channels: Vec<Vec<InputChannelRecord>> = input_layouts()
                .iter()
                .flat_map(|l| [&l.channels, &l.wave_mask_channels])
                .map(|c| c.iter().map(channel_record).collect())
                .collect();
            let records = input_layouts()
                .iter()
                .zip(channels.chunks(2))
                .map(|(l, c)| InputLayoutRecord {
                    id: Text::new(l.id),
                    internal_id: Text::new(l.internal_id),
                    display_name: Text::new(l.display_name),
                    wave_channel_mask: l.wave_channel_mask,
                    channels: c[0].as_ptr(),
                    channels_len: c[0].len(),
                    wave_mask_channels: c[1].as_ptr(),
                    wave_mask_channels_len: c[1].len(),
                })
                .collect();
            Static((channels, records))
        });
        unsafe {
            *entries = table.0.1.as_ptr();
            *entries_len = table.0.1.len();
        }
        Ok(())
    })
}

/// The custom-label catalog; `mradm_scene_resolve_labels` returns indices into it.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn mradm_scene_channel_catalog(
    entries: *mut *const InputChannelRecord,
    entries_len: *mut usize,
    message: *mut u8,
    capacity: usize,
) -> i32 {
    static TABLE: OnceLock<Static<Vec<InputChannelRecord>>> = OnceLock::new();
    scene_boundary(ptr::null_mut(), message, capacity, || {
        if entries.is_null() || entries_len.is_null() {
            return Err(invalid("Null channel catalog output"));
        }
        let table =
            TABLE.get_or_init(|| Static(channel_catalog().iter().map(channel_record).collect()));
        unsafe {
            *entries = table.0.as_ptr();
            *entries_len = table.0.len();
        }
        Ok(())
    })
}

/// Resolves custom labels in file-channel order; `indices` must hold `labels_len` entries.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn mradm_scene_resolve_labels(
    input_labels: *const Text,
    input_labels_len: usize,
    indices: *mut u32,
    indices_len: usize,
    message: *mut u8,
    capacity: usize,
) -> i32 {
    scene_boundary(ptr::null_mut(), message, capacity, || {
        let labels = unsafe { labels(input_labels, input_labels_len)? };
        if indices_len < labels.len() || (indices.is_null() && indices_len != 0) {
            return Err(invalid("无效标签索引缓冲区"));
        }
        let resolved = resolve_input_channel_labels(&labels)?;
        let out = unsafe { slice::from_raw_parts_mut(indices, resolved.len()) };
        for (slot, index) in out.iter_mut().zip(resolved) {
            *slot = index as u32;
        }
        Ok(())
    })
}

// ---------------------------------------------------------------------------------------------
// Scene stream semantic policy (`mradm_scene::live_policy`). Each handle is immutable once made;
// the stream swaps them by pointer. Strings are the producer's bytes; bytes that are not UTF-8 are
// compared as replacement characters.

pub struct LivePolicy(policy::Policy);
pub struct LiveGeneration(live_policy::Generation);
pub struct LiveResolution(live_policy::Resolution);

#[repr(C)]
pub struct LivePolicyEntity {
    id: Text,
    name: Text,
}

#[repr(C)]
pub struct LivePolicyIdentity {
    present: u32,
    object_id: Text,
    object_name: Text,
    track_uid: Text,
    has_importance: u32,
    importance: i32,
    has_dialogue_id: u32,
    dialogue_id: i32,
    contents: *const LivePolicyEntity,
    contents_len: usize,
    programmes: *const LivePolicyEntity,
    programmes_len: usize,
}

/// `role`: 0 object, 1 DirectSpeakers, 2 LFE.
#[repr(C)]
pub struct LivePolicyElement {
    role: u32,
    speaker_label: Text,
    has_position: u32,
    position: [f32; 3],
    identity: LivePolicyIdentity,
}

#[repr(C)]
#[derive(Clone, Copy)]
pub struct LivePolicyState {
    has_position: u32,
    linear_gain: f32,
    position: [f32; 3],
    width: f32,
    height: f32,
    depth: f32,
    diffuse: f32,
    divergence: f32,
    channel_lock: u32,
    screen_reference: u32,
    head_locked: u32,
    divergence_azimuth_range: f32,
    divergence_position_range: f32,
    has_max_distance: u32,
    max_distance: f32,
}

#[repr(C)]
#[derive(Clone, Copy)]
pub struct LivePolicyUpdate {
    jump_position: u32,
    ramp_duration_samples: u32,
}

fn live_flag(value: u32) -> mradm_scene::Result<bool> {
    match value {
        0 => Ok(false),
        1 => Ok(true),
        _ => Err(invalid("无效场景策略标志")),
    }
}

unsafe fn entities(
    data: *const LivePolicyEntity,
    len: usize,
) -> mradm_scene::Result<Vec<(String, String)>> {
    unsafe { bytes(data, len)? }
        .iter()
        .map(|e| {
            Ok((
                unsafe { lossy(e.id)? }.into_owned(),
                unsafe { lossy(e.name)? }.into_owned(),
            ))
        })
        .collect()
}

unsafe fn live_element(e: &LivePolicyElement) -> mradm_scene::Result<live_policy::Element> {
    let role = match e.role {
        0 => live_policy::Role::Object,
        1 => live_policy::Role::DirectSpeakers,
        2 => live_policy::Role::Lfe,
        _ => return Err(invalid("无效场景元素角色")),
    };
    let i = &e.identity;
    let identity = if live_flag(i.present)? {
        Some(live_policy::SourceIdentity {
            object_id: unsafe { lossy(i.object_id)? }.into_owned(),
            object_name: unsafe { lossy(i.object_name)? }.into_owned(),
            track_uid: unsafe { lossy(i.track_uid)? }.into_owned(),
            importance: live_flag(i.has_importance)?.then_some(i.importance),
            dialogue_id: live_flag(i.has_dialogue_id)?.then_some(i.dialogue_id),
            contents: unsafe { entities(i.contents, i.contents_len)? },
            programmes: unsafe { entities(i.programmes, i.programmes_len)? },
        })
    } else {
        None
    };
    Ok(live_policy::Element {
        role,
        speaker_label: unsafe { lossy(e.speaker_label)? }.into_owned(),
        position: live_flag(e.has_position)?.then_some(e.position),
        identity,
    })
}

/// Parses `mradm.semantic-policy.v1`; errors have context 3 (the caller's label).
#[unsafe(no_mangle)]
pub unsafe extern "C" fn mradm_live_policy_parse(
    json: *const u8,
    json_len: usize,
    output: *mut *mut LivePolicy,
    context: *mut u32,
    message: *mut u8,
    capacity: usize,
) -> i32 {
    scene_boundary(context, message, capacity, || {
        if output.is_null() {
            return Err(invalid("无效场景策略输出"));
        }
        unsafe {
            *output = ptr::null_mut();
        }
        let parsed = policy::parse(unsafe { bytes(json, json_len)? })?;
        unsafe {
            *output = Box::into_raw(Box::new(LivePolicy(parsed)));
        }
        Ok(())
    })
}

#[unsafe(no_mangle)]
pub unsafe extern "C" fn mradm_live_policy_destroy(handle: *mut LivePolicy) {
    if !handle.is_null() {
        unsafe {
            drop(Box::from_raw(handle));
        }
    }
}

/// The elements of one Scene generation with their merged semantic identities.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn mradm_live_policy_generation(
    elements: *const LivePolicyElement,
    elements_len: usize,
    output: *mut *mut LiveGeneration,
    message: *mut u8,
    capacity: usize,
) -> i32 {
    scene_boundary(ptr::null_mut(), message, capacity, || {
        if output.is_null() {
            return Err(invalid("无效场景策略输出"));
        }
        unsafe {
            *output = ptr::null_mut();
        }
        let elements = unsafe { bytes(elements, elements_len)? }
            .iter()
            .map(|e| unsafe { live_element(e) })
            .collect::<mradm_scene::Result<Vec<_>>>()?;
        let generation = live_policy::Generation::new(elements)?;
        unsafe {
            *output = Box::into_raw(Box::new(LiveGeneration(generation)));
        }
        Ok(())
    })
}

#[unsafe(no_mangle)]
pub unsafe extern "C" fn mradm_live_policy_generation_destroy(handle: *mut LiveGeneration) {
    if !handle.is_null() {
        unsafe {
            drop(Box::from_raw(handle));
        }
    }
}

/// Resolves `policy` for every element of `generation`; `unmatched` then points into the
/// resolution at the indices of the rules no element matched, in order.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn mradm_live_policy_resolve(
    policy: *const LivePolicy,
    generation: *const LiveGeneration,
    output: *mut *mut LiveResolution,
    unmatched: *mut *const usize,
    unmatched_len: *mut usize,
    message: *mut u8,
    capacity: usize,
) -> i32 {
    scene_boundary(ptr::null_mut(), message, capacity, || {
        if policy.is_null()
            || generation.is_null()
            || output.is_null()
            || unmatched.is_null()
            || unmatched_len.is_null()
        {
            return Err(invalid("无效场景策略句柄"));
        }
        unsafe {
            *output = ptr::null_mut();
        }
        let resolution = Box::new(LiveResolution(live_policy::Resolution::new(
            unsafe { &(*policy).0 },
            unsafe { &(*generation).0 },
        )));
        unsafe {
            *unmatched = resolution.0.unmatched().as_ptr();
            *unmatched_len = resolution.0.unmatched().len();
            *output = Box::into_raw(resolution);
        }
        Ok(())
    })
}

#[unsafe(no_mangle)]
pub unsafe extern "C" fn mradm_live_policy_resolution_destroy(handle: *mut LiveResolution) {
    if !handle.is_null() {
        unsafe {
            drop(Box::from_raw(handle));
        }
    }
}

/// Applies element `index`'s policy to its producer state in place; `update` is null outside a
/// metadata update. Does not allocate.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn mradm_live_policy_apply(
    resolution: *const LiveResolution,
    index: usize,
    sample_rate: u32,
    state: *mut LivePolicyState,
    update: *mut LivePolicyUpdate,
) -> i32 {
    scene_boundary(ptr::null_mut(), ptr::null_mut(), 0, || {
        if resolution.is_null() || state.is_null() {
            return Err(invalid("无效场景策略句柄"));
        }
        let raw = unsafe { &mut *state };
        let mut s = live_policy::State {
            has_position: live_flag(raw.has_position)?,
            linear_gain: raw.linear_gain,
            position: raw.position,
            width: raw.width,
            height: raw.height,
            depth: raw.depth,
            diffuse: raw.diffuse,
            divergence: raw.divergence,
            channel_lock: live_flag(raw.channel_lock)?,
            screen_reference: live_flag(raw.screen_reference)?,
            head_locked: live_flag(raw.head_locked)?,
            divergence_azimuth_range: raw.divergence_azimuth_range,
            divergence_position_range: raw.divergence_position_range,
            max_distance: live_flag(raw.has_max_distance)?.then_some(raw.max_distance),
        };
        let mut u = if update.is_null() {
            None
        } else {
            let raw = unsafe { &*update };
            Some(live_policy::Update {
                jump_position: live_flag(raw.jump_position)?,
                ramp_duration_samples: raw.ramp_duration_samples,
            })
        };
        unsafe { &(*resolution).0 }
            .apply(index, sample_rate, &mut s, u.as_mut())
            .ok_or_else(|| invalid("场景策略元素越界"))?;
        *raw = LivePolicyState {
            has_position: u32::from(s.has_position),
            linear_gain: s.linear_gain,
            position: s.position,
            width: s.width,
            height: s.height,
            depth: s.depth,
            diffuse: s.diffuse,
            divergence: s.divergence,
            channel_lock: u32::from(s.channel_lock),
            screen_reference: u32::from(s.screen_reference),
            head_locked: u32::from(s.head_locked),
            divergence_azimuth_range: s.divergence_azimuth_range,
            divergence_position_range: s.divergence_position_range,
            has_max_distance: u32::from(s.max_distance.is_some()),
            max_distance: s.max_distance.unwrap_or(0.),
        };
        if let Some(u) = u {
            unsafe {
                *update = LivePolicyUpdate {
                    jump_position: u32::from(u.jump_position),
                    ramp_duration_samples: u.ramp_duration_samples,
                };
            }
        }
        Ok(())
    })
}

#[repr(C)]
pub struct DirectMatrixTarget {
    label: Text,
    weight: f32,
    gain: f32,
}

#[repr(C)]
pub struct DirectMatrixRoute {
    source_label: Text,
    mute: u32,
    targets: *const DirectMatrixTarget,
    targets_len: usize,
}

#[repr(C)]
pub struct DirectMatrixView {
    output_layout: Text,
    routes: *const DirectMatrixRoute,
    routes_len: usize,
}

/// A parsed DirectSpeakers matrix; its view borrows from it until `mradm_scene_direct_matrix_destroy`.
pub struct DirectMatrix {
    matrix: direct_matrix::Matrix,
    _targets: Vec<Vec<DirectMatrixTarget>>,
    routes: Vec<DirectMatrixRoute>,
}

/// Parses `mradm.direct-speakers-matrix.v1`; the caller attaches its source label to errors.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn mradm_scene_direct_matrix_parse(
    json: *const u8,
    json_len: usize,
    output: *mut *mut DirectMatrix,
    view: *mut DirectMatrixView,
    message: *mut u8,
    message_len: *mut usize,
) -> i32 {
    let capacity = unsafe { message_len.as_ref() }.copied().unwrap_or(0);
    scene_boundary_with_message_len(ptr::null_mut(), message, capacity, message_len, || {
        if output.is_null() || view.is_null() {
            return Err(invalid("无效 DirectSpeakers 矩阵输出"));
        }
        unsafe {
            *output = ptr::null_mut();
        }
        let matrix = direct_matrix::parse(unsafe { bytes(json, json_len)? })?;
        let targets: Vec<Vec<DirectMatrixTarget>> = matrix
            .routes
            .iter()
            .map(|route| {
                route
                    .targets
                    .iter()
                    .map(|t| DirectMatrixTarget {
                        label: Text::new(&t.label),
                        weight: t.weight,
                        gain: t.gain,
                    })
                    .collect()
            })
            .collect();
        let routes = matrix
            .routes
            .iter()
            .zip(&targets)
            .map(|(route, records)| DirectMatrixRoute {
                source_label: Text::new(&route.source_label),
                mute: u32::from(route.mute),
                targets: records.as_ptr(),
                targets_len: records.len(),
            })
            .collect();
        let handle = Box::new(DirectMatrix {
            matrix,
            _targets: targets,
            routes,
        });
        unsafe {
            *view = DirectMatrixView {
                output_layout: Text::new(&handle.matrix.output_layout),
                routes: handle.routes.as_ptr(),
                routes_len: handle.routes.len(),
            };
            *output = Box::into_raw(handle);
        }
        Ok(())
    })
}

#[unsafe(no_mangle)]
pub unsafe extern "C" fn mradm_scene_direct_matrix_destroy(handle: *mut DirectMatrix) {
    if !handle.is_null() {
        unsafe {
            drop(Box::from_raw(handle));
        }
    }
}

#[repr(C)]
pub struct ScreenConfigView {
    has_reference: u32,
    has_reproduction: u32,
    reference: RoomScreenGeometry,
    reproduction: RoomScreenGeometry,
}

/// Parses `mradm.screen-config` version 1 without validating the geometry; errors have no context.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn mradm_scene_screen_parse(
    json: *const u8,
    json_len: usize,
    output: *mut ScreenConfigView,
    message: *mut u8,
    message_len: *mut usize,
) -> i32 {
    let capacity = unsafe { message_len.as_ref() }.copied().unwrap_or(0);
    scene_boundary_with_message_len(ptr::null_mut(), message, capacity, message_len, || {
        if output.is_null() {
            return Err(invalid("无效屏幕配置输出"));
        }
        let screens = screen::parse(unsafe { bytes(json, json_len)? })?;
        unsafe {
            *output = ScreenConfigView {
                has_reference: u32::from(screens.reference.is_some()),
                has_reproduction: u32::from(screens.reproduction.is_some()),
                reference: screens.reference.unwrap_or_default(),
                reproduction: screens.reproduction.unwrap_or_default(),
            };
        }
        Ok(())
    })
}

#[cfg(test)]
mod tests {
    use super::*;
    use std::{ptr, slice, str};

    #[test]
    fn config_error_lengths_preserve_nul_and_bound_utf8_copies() {
        let json = "{\"音\\u0000尾\":1}".as_bytes();
        let expected = direct_matrix::parse(json).unwrap_err().message;
        for capacity in [0, 1, 2, 58, 60, 256] {
            let mut message = [0xCC; 256];
            let mut len = capacity;
            let mut handle = ptr::null_mut();
            let mut view = std::mem::MaybeUninit::uninit();
            let code = unsafe {
                mradm_scene_direct_matrix_parse(
                    json.as_ptr(),
                    json.len(),
                    &mut handle,
                    view.as_mut_ptr(),
                    message.as_mut_ptr(),
                    &mut len,
                )
            };
            assert_eq!(code, 1);
            assert!(handle.is_null());
            assert!(len <= capacity.saturating_sub(1));
            assert_eq!(str::from_utf8(&message[..len]).unwrap(), &expected[..len]);
            if capacity != 0 {
                assert_eq!(message[len], 0);
            }
            if capacity > expected.len() {
                assert_eq!(len, expected.len());
            }
        }
    }

    #[test]
    fn layout_views_round_trip_and_reject_unknown_geometry() {
        for geometry in [0, 1] {
            let (mut entries, mut len) = (ptr::null(), 0);
            let code = unsafe {
                mradm_scene_layouts(geometry, &mut entries, &mut len, ptr::null_mut(), 0)
            };
            assert_eq!(code, 0);
            let source = layouts(if geometry == 0 {
                Geometry::Standard
            } else {
                Geometry::Apple
            });
            let views = unsafe { slice::from_raw_parts(entries, len) };
            assert_eq!(views.len(), source.len());
            for (view, layout) in views.iter().zip(source) {
                let id =
                    unsafe { str::from_utf8(slice::from_raw_parts(view.id, view.id_len)).unwrap() };
                assert_eq!(id, layout.id);
                let speakers = unsafe { slice::from_raw_parts(view.speakers, view.speakers_len) };
                for (record, speaker) in speakers.iter().zip(&layout.speakers) {
                    assert_eq!(record.azimuth.to_bits(), speaker.azimuth.to_bits());
                    assert_eq!(
                        record.has_azimuth_range != 0,
                        speaker.azimuth_range.is_some()
                    );
                }
            }
        }
        let (mut entries, mut len) = (ptr::null(), 0);
        let mut message = [0u8; 64];
        let code = unsafe {
            mradm_scene_layouts(
                2,
                &mut entries,
                &mut len,
                message.as_mut_ptr(),
                message.len(),
            )
        };
        assert_eq!(code, 1);
        assert!(entries.is_null());
    }
}
