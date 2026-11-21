//! The render orchestration boundary (`mradm-engine`): the backend
//! capability records and the discovery JSON documents. Island facts (the Apple backend, the
//! system-spatial layouts, encoder availability) are read from the islands' [`PlatformIslands`].
use crate::adm::Text;
use mradm_engine::catalog::{Backend, Layout, Mode, Platform, SpatialLayout};
use std::{ptr, slice};

pub const LAYOUT_3D: u32 = 1;
pub const LAYOUT_SPREAD: u32 = 2;
pub const LAYOUT_BINAURAL: u32 = 4;

#[repr(C)]
#[derive(Clone, Copy)]
pub struct EngineLayout {
    id: Text,
    display_name: Text,
    channel_count: u16,
    lfe_count: u16,
    /// `LAYOUT_3D | LAYOUT_SPREAD | LAYOUT_BINAURAL`.
    flags: u32,
}

#[repr(C)]
#[derive(Clone, Copy)]
pub struct EngineMode {
    id: Text,
    display_name: Text,
    layouts: *const Text,
    layouts_len: usize,
    sample_rates: *const u32,
    sample_rates_len: usize,
    supports_realtime: u32,
}

/// Feature bits: 1 objects, 2 DirectSpeakers, 4 HOA, 8 channelLock, 16 divergence,
/// 32 screenRef, 64 diffuse, 128 render window.
#[repr(C)]
#[derive(Clone, Copy)]
pub struct EngineBackend {
    renderer: Text,
    name: Text,
    version: Text,
    layouts: *const EngineLayout,
    layouts_len: usize,
    modes: *const EngineMode,
    modes_len: usize,
    qualities: *const EngineMode,
    qualities_len: usize,
    hrtf_sources: *const Text,
    hrtf_sources_len: usize,
    flags: u32,
}

#[repr(C)]
#[derive(Clone, Copy)]
pub struct EngineSpatialLayout {
    id: Text,
    display_name: Text,
    channel_count: u16,
    reserved: u16,
}

/// The islands' platform facts (`src/adm_engine/platform_island.h`): APAC availability, the
/// Apple backend's process-lifetime record (null without it) and LFE self-test
/// (-1 without it), and the system-spatial layouts (process-lifetime records). Entries may be
/// null (the fact is absent).
#[repr(C)]
pub struct PlatformIslands {
    apac: Option<unsafe extern "C" fn() -> u32>,
    apple: Option<unsafe extern "C" fn() -> *const EngineBackend>,
    apple_lfe_routing_ok: Option<unsafe extern "C" fn() -> i32>,
    system_spatial: Option<unsafe extern "C" fn(*mut *const EngineSpatialLayout, *mut usize)>,
}

/// What a caller needs from the islands: a render or stream opening (IAMF availability and the
/// Apple backend) or discovery document `kind`.
#[derive(Clone, Copy)]
pub enum Facts {
    Render,
    Document(u32),
}

fn base_platform() -> Platform {
    Platform {
        sofa: cfg!(feature = "sofa"),
        ..Platform::default()
    }
}

unsafe fn items<'a, T>(data: *const T, len: usize) -> Option<&'a [T]> {
    if len == 0 {
        return Some(&[]);
    }
    if data.is_null() || !(data as usize).is_multiple_of(align_of::<T>()) {
        return None;
    }
    Some(unsafe { slice::from_raw_parts(data, len) })
}

pub(crate) unsafe fn text(value: Text) -> Option<String> {
    let bytes = unsafe { items(value.data, value.len)? };
    Some(String::from_utf8_lossy(bytes).into_owned())
}

pub(crate) unsafe fn texts(data: *const Text, len: usize) -> Option<Vec<String>> {
    unsafe { items(data, len)? }
        .iter()
        .map(|&t| unsafe { text(t) })
        .collect()
}

unsafe fn modes(data: *const EngineMode, len: usize) -> Option<Vec<Mode>> {
    unsafe { items(data, len)? }
        .iter()
        .map(|m| {
            Some(Mode {
                id: unsafe { text(m.id)? },
                display_name: unsafe { text(m.display_name)? },
                layouts: unsafe { texts(m.layouts, m.layouts_len)? },
                sample_rates: unsafe { items(m.sample_rates, m.sample_rates_len)? }.to_vec(),
                supports_realtime: m.supports_realtime != 0,
            })
        })
        .collect()
}

unsafe fn backend(b: &EngineBackend) -> Option<Backend> {
    let flag = |bit: u32| b.flags & (1 << bit) != 0;
    Some(Backend {
        renderer: unsafe { text(b.renderer)? },
        name: unsafe { text(b.name)? },
        version: unsafe { text(b.version)? },
        layouts: unsafe { items(b.layouts, b.layouts_len)? }
            .iter()
            .map(|l| {
                Some(Layout {
                    id: unsafe { text(l.id)? },
                    display_name: unsafe { text(l.display_name)? },
                    channel_count: l.channel_count,
                    is_3d: l.flags & LAYOUT_3D != 0,
                    lfe_count: l.lfe_count,
                    supports_spread: l.flags & LAYOUT_SPREAD != 0,
                    is_binaural: l.flags & LAYOUT_BINAURAL != 0,
                })
            })
            .collect::<Option<_>>()?,
        modes: unsafe { modes(b.modes, b.modes_len)? },
        qualities: unsafe { modes(b.qualities, b.qualities_len)? },
        supports_objects: flag(0),
        supports_direct_speakers: flag(1),
        supports_hoa: flag(2),
        supports_channel_lock: flag(3),
        supports_object_divergence: flag(4),
        supports_screen_ref: flag(5),
        supports_diffuse: flag(6),
        hrtf_sources: unsafe { texts(b.hrtf_sources, b.hrtf_sources_len)? },
        supports_render_window: flag(7),
    })
}

/// The platform of `facts`, probing only what it needs as the C++ catalog did: the output-format
/// documents probe APAC and IAMF (and the MP4 packager in IAMF builds), the capabilities document
/// runs the Apple LFE self-test and lists the system-spatial layouts, and every caller gets the
/// Apple backend.
///
/// # Safety
///
/// Every non-null entry of `p` must be callable from this thread, write only through the pointers
/// it is given, and return records that live for the process (see [`PlatformIslands`]).
pub unsafe fn platform(p: &PlatformIslands, facts: Facts) -> Option<Platform> {
    let iamf = || crate::iamf::AVAILABLE;
    unsafe { platform_with(p, facts, &iamf, &crate::mp4_packager::available) }
}

/// [`platform`] with the IAMF and MP4 packager facts (in Rust since PCM consistency 7.2 C2 / C3)
/// given.
unsafe fn platform_with(
    p: &PlatformIslands,
    facts: Facts,
    iamf: &dyn Fn() -> bool,
    packager: &dyn Fn() -> bool,
) -> Option<Platform> {
    let flag = |f: Option<unsafe extern "C" fn() -> u32>| f.is_some_and(|f| unsafe { f() } != 0);
    let (formats, capabilities) = match facts {
        Facts::Render => (false, false),
        Facts::Document(kind) => (kind == 2 || kind == 4, kind == 0),
    };
    let (apac, iamf, iamf_mp4_packager) = if formats {
        let apac = flag(p.apac);
        let iamf = iamf();
        // The packager probe runs only in IAMF builds.
        (apac, iamf, iamf && packager())
    } else {
        (false, matches!(facts, Facts::Render) && iamf(), false)
    };
    let record = p.apple.map_or(ptr::null(), |f| unsafe { f() });
    let apple = if record.is_null() {
        None
    } else {
        Some(unsafe { backend(&*record)? })
    };
    let (mut layouts, mut len) = (ptr::null(), 0);
    if capabilities && let Some(f) = p.system_spatial {
        unsafe { f(&mut layouts, &mut len) };
    }
    let system_spatial_layouts = unsafe { items(layouts, len)? }
        .iter()
        .map(|l| {
            Some(SpatialLayout {
                id: unsafe { text(l.id)? },
                display_name: unsafe { text(l.display_name)? },
                channel_count: l.channel_count,
            })
        })
        .collect::<Option<_>>()?;
    let lfe = match p.apple_lfe_routing_ok {
        Some(f) if capabilities => unsafe { f() },
        _ => -1,
    };
    Some(Platform {
        apac,
        iamf,
        iamf_mp4_packager,
        apple,
        system_spatial_layouts,
        apple_system_spatial_lfe_routing_ok: (lfe >= 0).then_some(lfe != 0),
        ..base_platform()
    })
}

// ── Request resolution ─────────────────────────────────────────────

#[cfg(test)]
mod tests {
    use super::*;

    static PROBED: std::sync::Mutex<Vec<&'static str>> = std::sync::Mutex::new(Vec::new());

    fn probed(name: &'static str) {
        PROBED.lock().unwrap().push(name);
    }

    unsafe extern "C" fn yes_apac() -> u32 {
        probed("apac");
        1
    }
    fn yes_iamf() -> bool {
        probed("iamf");
        true
    }
    fn yes_packager() -> bool {
        probed("packager");
        true
    }
    unsafe extern "C" fn lfe() -> i32 {
        probed("lfe");
        0
    }
    unsafe extern "C" fn spatial(layouts: *mut *const EngineSpatialLayout, len: *mut usize) {
        probed("spatial");
        unsafe {
            *layouts = ptr::null();
            *len = 0;
        }
    }

    #[test]
    fn each_caller_probes_only_its_facts() {
        let islands = PlatformIslands {
            apac: Some(yes_apac),
            apple: None,
            apple_lfe_routing_ok: Some(lfe),
            system_spatial: Some(spatial),
        };
        let mut seen = Vec::new();
        for facts in [
            Facts::Render,
            Facts::Document(0),
            Facts::Document(1),
            Facts::Document(2),
            Facts::Document(3),
            Facts::Document(4),
        ] {
            let p = unsafe { platform_with(&islands, facts, &yes_iamf, &yes_packager) }.unwrap();
            seen.push((
                std::mem::take(&mut *PROBED.lock().unwrap()),
                p.apac,
                p.iamf,
                p.iamf_mp4_packager,
                p.apple_system_spatial_lfe_routing_ok,
            ));
        }
        assert_eq!(
            seen,
            [
                (vec!["iamf"], false, true, false, None),
                (vec!["spatial", "lfe"], false, false, false, Some(false)),
                (vec![], false, false, false, None),
                (vec!["apac", "iamf", "packager"], true, true, true, None),
                (vec![], false, false, false, None),
                (vec!["apac", "iamf", "packager"], true, true, true, None),
            ]
        );
    }
}
