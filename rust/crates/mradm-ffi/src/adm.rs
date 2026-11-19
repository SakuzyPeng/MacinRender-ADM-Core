//! Private, borrowed ADM views. All owning allocations are released on this side.
use mradm_adm::{Snapshot, records::*};

#[repr(C)]
#[derive(Clone, Copy)]
pub struct Text {
    pub data: *const u8,
    pub len: usize,
}
impl Text {
    pub fn new(s: &str) -> Self {
        Self::bytes(s.as_bytes())
    }
    pub fn bytes(s: &[u8]) -> Self {
        Self {
            data: s.as_ptr(),
            len: s.len(),
        }
    }
}
#[repr(C)]
pub struct View {
    pub strings: *const Text,
    pub strings_len: usize,
    indices: *const u32,
    indices_len: usize,
    programmes: *const Programme,
    pub programmes_len: usize,
    contents: *const Content,
    contents_len: usize,
    objects: *const Object,
    pub objects_len: usize,
    tracks: *const Track,
    tracks_len: usize,
    object_blocks: *const ObjectBlock,
    object_blocks_len: usize,
    direct_blocks: *const DirectBlock,
    direct_blocks_len: usize,
    hoa: *const Hoa,
    hoa_len: usize,
    hoa_channels: *const HoaChannel,
    hoa_channels_len: usize,
    hoa_blocks: *const HoaBlock,
    hoa_blocks_len: usize,
    pub warnings: *const u32,
    pub warnings_len: usize,
}
impl View {
    /// Borrows `snapshot` and its string views; valid while both stay alive and unmodified.
    pub(crate) fn new(s: &Snapshot, texts: &[Text]) -> Self {
        View {
            strings: texts.as_ptr(),
            strings_len: texts.len(),
            indices: s.indices.as_ptr(),
            indices_len: s.indices.len(),
            programmes: s.programmes.as_ptr(),
            programmes_len: s.programmes.len(),
            contents: s.contents.as_ptr(),
            contents_len: s.contents.len(),
            objects: s.objects.as_ptr(),
            objects_len: s.objects.len(),
            tracks: s.tracks.as_ptr(),
            tracks_len: s.tracks.len(),
            object_blocks: s.object_blocks.as_ptr(),
            object_blocks_len: s.object_blocks.len(),
            direct_blocks: s.direct_blocks.as_ptr(),
            direct_blocks_len: s.direct_blocks.len(),
            hoa: s.hoa.as_ptr(),
            hoa_len: s.hoa.len(),
            hoa_channels: s.hoa_channels.as_ptr(),
            hoa_channels_len: s.hoa_channels.len(),
            hoa_blocks: s.hoa_blocks.as_ptr(),
            hoa_blocks_len: s.hoa_blocks.len(),
            warnings: s.warnings.as_ptr(),
            warnings_len: s.warnings.len(),
        }
    }
}
