//! Private render-scene boundary (PCM consistency 4.3). Layout tables are process-lifetime views of
//! `mradm-scene` static data and are never freed.
use crate::boundary;
use mradm_dsp::Error;
use mradm_scene::layouts::{Geometry, Layout, layouts};
use std::sync::OnceLock;

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
