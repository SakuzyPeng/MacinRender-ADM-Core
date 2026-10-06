//! Plain metadata records shared with the private C adapter. String IDs index Snapshot::strings.
pub const NONE: u32 = u32::MAX;

#[repr(C)]
#[derive(Debug, Clone, Copy, Default, PartialEq)]
pub struct Span {
    pub offset: usize,
    pub len: usize,
}

#[repr(C)]
#[derive(Debug, Clone, Copy, Default, PartialEq)]
pub struct AdmPosition {
    pub cartesian: u32,
    pub azimuth: f32,
    pub elevation: f32,
    pub distance: f32,
    pub x: f32,
    pub y: f32,
    pub z: f32,
}

#[repr(C)]
#[derive(Debug, Clone, Copy, Default, PartialEq)]
pub struct Gain {
    pub present: u32,
    pub decibels: u32,
    pub value: f64,
    pub linear: f32,
}

#[repr(C)]
#[derive(Debug, Clone, Copy, Default, PartialEq)]
pub struct BlockSource {
    pub gain: Gain,
    pub rtime_present: u32,
    pub rtime_samples: u64,
    pub duration_present: u32,
    pub duration_samples: u64,
}

#[repr(C)]
#[derive(Debug, Clone, Copy, Default, PartialEq)]
pub struct Loudness {
    pub present: u32,
    pub fields: u32,
    pub integrated: f32,
    pub true_peak: f32,
    pub range: f32,
    pub momentary: f32,
    pub short_term: f32,
    pub dialogue: f32,
    pub method: u32,
}

#[repr(C)]
#[derive(Debug, Clone, Copy, Default, PartialEq)]
pub struct Programme {
    pub id: u32,
    pub name: u32,
    pub language: u32,
    pub content_ids: Span,
    pub labels: Span,
    pub start: u64,
    pub end_present: u32,
    pub end: u64,
    pub loudness: Loudness,
    pub reference_screen: u32,
}

#[repr(C)]
#[derive(Debug, Clone, Copy, Default, PartialEq)]
pub struct Content {
    pub id: u32,
    pub name: u32,
    pub language: u32,
    pub object_ids: Span,
    pub labels: Span,
    pub loudness: Loudness,
    pub dialogue_kind: u32,
    pub content_kind: u32,
}

#[repr(C)]
#[derive(Debug, Clone, Copy, Default, PartialEq)]
pub struct Object {
    pub node: u32,
    pub id: u32,
    pub name: u32,
    pub gain: f32,
    pub mute: u32,
    pub head_locked: u32,
    pub end: u64,
    pub offset_present: u32,
    pub position_offset: AdmPosition,
    pub tracks: Span,
    pub labels: Span,
    pub importance_present: u32,
    pub importance: i32,
    pub dialogue_present: u32,
    pub dialogue: u32,
    pub source_gain: Gain,
    pub mute_present: u32,
    pub start_present: u32,
    pub start: u64,
    pub absolute_start: u64,
    pub duration_present: u32,
    pub duration: u64,
    pub child_objects: Span,
    pub has_parent: u32,
}

#[repr(C)]
#[derive(Debug, Clone, Copy, Default, PartialEq)]
pub struct Track {
    pub uid: u32,
    pub channel: i32,
    pub blocks: Span,
    pub ds_blocks: Span,
}

#[repr(C)]
#[derive(Debug, Clone, Copy, Default, PartialEq)]
pub struct ObjectBlock {
    pub node: u32,
    pub position: AdmPosition,
    pub gain: f32,
    pub diffuse: f32,
    pub width: f32,
    pub height: f32,
    pub depth: f32,
    pub start: u64,
    pub end: u64,
    pub jump: u32,
    pub interpolation_present: u32,
    pub interpolation: u64,
    pub channel_lock: u32,
    pub max_distance_present: u32,
    pub max_distance: f32,
    pub divergence: f32,
    pub azimuth_range: f32,
    pub position_range: f32,
    pub screen_ref: u32,
    pub head_locked: u32,
    pub source: BlockSource,
}

#[repr(C)]
#[derive(Debug, Clone, Copy, Default, PartialEq)]
pub struct DirectBlock {
    pub node: u32,
    pub labels: Span,
    pub pack_id: u32,
    pub has_position: u32,
    pub position: AdmPosition,
    pub azimuth: f32,
    pub elevation: f32,
    pub distance: f32,
    pub bounds_present: u32,
    pub azimuth_min: f32,
    pub azimuth_max: f32,
    pub elevation_min: f32,
    pub elevation_max: f32,
    pub distance_min: f32,
    pub distance_max: f32,
    pub gain: f32,
    pub low_pass_present: u32,
    pub low_pass: f32,
    pub start: u64,
    pub end: u64,
    pub head_locked: u32,
    pub source: BlockSource,
}

#[repr(C)]
#[derive(Debug, Clone, Copy, Default, PartialEq)]
pub struct Hoa {
    pub object_id: u32,
    pub pack_id: u32,
    pub normalization: u32,
    pub nfc_ref_dist: f64,
    pub screen_ref: u32,
    pub gain: f32,
    pub mute: u32,
    pub head_locked: u32,
    pub start: u64,
    pub end: u64,
    pub channels: Span,
}

#[repr(C)]
#[derive(Debug, Clone, Copy, Default, PartialEq)]
pub struct HoaChannel {
    pub uid: u32,
    pub channel: i32,
    pub order: i32,
    pub degree: i32,
    pub blocks: Span,
}

#[repr(C)]
#[derive(Debug, Clone, Copy, Default, PartialEq)]
pub struct HoaBlock {
    pub node: u32,
    pub gain: f32,
    pub head_locked: u32,
    pub start: u64,
    pub end: u64,
}

#[repr(C)]
#[derive(Debug, Clone, Copy, Default, PartialEq)]
pub struct Patch {
    pub node: u32,
    pub field: u32,
    pub present: u32,
    pub original_present: u32,
    pub value: f64,
    pub original: f64,
    pub samples: u64,
    pub original_samples: u64,
}

#[repr(C)]
#[derive(Debug, Clone, Copy, Default, PartialEq)]
pub struct AdmSpeaker {
    pub label: u32,
    pub azimuth: f32,
    pub elevation: f32,
    pub lfe: u32,
    pub ranges: u32,
    pub azimuth_min: f32,
    pub azimuth_max: f32,
    pub elevation_min: f32,
    pub elevation_max: f32,
}
