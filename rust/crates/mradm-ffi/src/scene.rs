//! Private render-scene boundary with the semantic policy, the DirectSpeakers matrix and the
//! screen configuration. An imported scene is owned by its handle and its views are borrowed until
//! the next call that changes it (`mradm_engine_apply_policy`, `mradm_scene_template`) or until
//! `mradm_scene_destroy`.
use crate::adm::Text;
use mradm_scene::{Context, ImportOptions, RenderScene, direct_matrix, policy::UserLevel};
use std::{
    borrow::Cow,
    panic::{AssertUnwindSafe, catch_unwind},
    ptr, slice,
};

pub struct SceneHandle {
    pub(crate) scene: RenderScene,
    objects: Vec<SceneObjectState>,
    direct_blocks: Vec<SceneDirectState>,
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

fn user_mute(user: UserLevel) -> u32 {
    match user.mute {
        None => 0,
        Some(false) => 1,
        Some(true) => 2,
    }
}

impl SceneHandle {
    pub fn scene(&self) -> &RenderScene {
        &self.scene
    }
    /// A render request's semantic policy applied to the scene (`mradm_engine_apply_policy`): the
    /// in-memory JSON preferred over the file, with the warnings as log lines.
    pub fn apply_policy(
        &mut self,
        json: Option<&[u8]>,
        path: Option<&mradm_engine::render::PathText>,
        display: &str,
    ) -> mradm_engine::request::Outcome<()> {
        let outcome = mradm_engine::render::apply_policy(&mut self.scene, json, path, display);
        self.refresh();
        outcome
    }

    /// The import `mradm_scene_import` makes: `routed` false imports ADM BW64 only; true routes to
    /// ADM or a channel bed by the layout and labels. `path` is UTF-8.
    pub fn import(
        path: &[u8],
        routed: bool,
        layout: Option<&str>,
        labels: &[String],
    ) -> mradm_scene::Result<Box<Self>> {
        let scene = if routed {
            mradm_scene::import(
                path,
                ImportOptions {
                    input_layout: layout,
                    input_channel_labels: labels,
                },
            )?
        } else {
            mradm_scene::import_adm(path)?
        };
        Ok(Box::new(Self::new(scene)))
    }

    pub(crate) fn new(scene: RenderScene) -> Self {
        let mut handle = Self {
            scene,
            objects: Vec::new(),
            direct_blocks: Vec::new(),
        };
        handle.refresh();
        handle
    }

    /// Rebuilds the C records of the overlay after the scene changed.
    pub(crate) fn refresh(&mut self) {
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
    }
}

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
pub(crate) unsafe extern "C" fn mradm_scene_import(
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
        let handle = if routed == 0 {
            SceneHandle::import(path, false, None, &[])?
        } else {
            let layout = if has_layout != 0 {
                Some(unsafe { lossy(layout)? })
            } else {
                None
            };
            let labels = unsafe { labels(input_labels, input_labels_len)? };
            SceneHandle::import(path, true, layout.as_deref(), &labels)?
        };
        unsafe {
            *output = Box::into_raw(handle);
        }
        Ok(())
    })
}

pub(crate) unsafe extern "C" fn mradm_scene_destroy(handle: *mut SceneHandle) {
    if !handle.is_null() {
        unsafe {
            drop(Box::from_raw(handle));
        }
    }
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

impl DirectMatrix {
    /// Owns `matrix` and the records its view points at.
    pub(crate) fn new(matrix: direct_matrix::Matrix) -> Box<Self> {
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
        Box::new(Self {
            matrix,
            _targets: targets,
            routes,
        })
    }

    /// No matrix.
    pub(crate) fn empty_view() -> DirectMatrixView {
        DirectMatrixView {
            output_layout: Text::new(""),
            routes: ptr::null(),
            routes_len: 0,
        }
    }

    pub(crate) fn view(&self) -> DirectMatrixView {
        DirectMatrixView {
            output_layout: Text::new(&self.matrix.output_layout),
            routes: self.routes.as_ptr(),
            routes_len: self.routes.len(),
        }
    }
}

/// Parses `mradm.direct-speakers-matrix.v1`; the caller attaches its source label to errors.
pub(crate) unsafe extern "C" fn mradm_scene_direct_matrix_parse(
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
        let handle = DirectMatrix::new(matrix);
        unsafe {
            *view = handle.view();
            *output = Box::into_raw(handle);
        }
        Ok(())
    })
}

pub(crate) unsafe extern "C" fn mradm_scene_direct_matrix_destroy(handle: *mut DirectMatrix) {
    if !handle.is_null() {
        unsafe {
            drop(Box::from_raw(handle));
        }
    }
}

#[cfg(test)]
mod tests {
    use super::*;
    use std::{ptr, str};

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
}
