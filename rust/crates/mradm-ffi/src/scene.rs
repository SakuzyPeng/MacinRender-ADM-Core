//! Private render-scene boundary (PCM consistency 4.3). Layout and input-preset tables are
//! process-lifetime views of `mradm-scene` static data and are never freed; an imported scene is
//! owned by its handle and its views are borrowed until `mradm_scene_destroy`.
use crate::adm::{Text, View};
use crate::boundary;
use mradm_dsp::Error;
use mradm_scene::{
    Context, ImportOptions, RenderScene, SourceKind,
    input::{InputChannel, channel_catalog, input_layouts, resolve_input_channel_labels},
    layouts::{Geometry, Layout, layouts},
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

/// Like `boundary`, plus the locator kind: 0 none, 1 `input=<path>`, 2 `path=<path>`.
fn scene_boundary(
    context: *mut u32,
    message: *mut u8,
    capacity: usize,
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
        };
        unsafe {
            *context = kind;
        }
    }
    let detail = error.as_ref().map_or("", |e| e.message.as_str());
    if !message.is_null() && capacity != 0 {
        let mut len = detail.len().min(capacity - 1);
        while !detail.is_char_boundary(len) {
            len -= 1;
        }
        unsafe {
            ptr::copy_nonoverlapping(detail.as_ptr(), message, len);
            *message.add(len) = 0;
        }
    }
    error.map_or(0, |e| e.code)
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
        let texts = scene
            .snapshot
            .strings
            .iter()
            .map(|s| Text::new(s))
            .collect();
        let labels = scene
            .info
            .input_channel_labels
            .iter()
            .map(|s| Text::new(s))
            .collect();
        unsafe {
            *output = Box::into_raw(Box::new(SceneHandle {
                scene,
                texts,
                labels,
            }));
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

#[cfg(test)]
mod tests {
    use super::*;
    use std::{ptr, slice, str};

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
