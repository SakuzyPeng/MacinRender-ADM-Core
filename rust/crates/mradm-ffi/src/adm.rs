//! Private, borrowed ADM views. All owning allocations are released on this side.
use mradm_adm::{Document, Error, GeneratedMetadata, Result, Snapshot, records::*};
use std::{
    panic::{AssertUnwindSafe, catch_unwind},
    ptr, slice, str,
};

#[repr(C)]
#[derive(Clone, Copy)]
pub struct Text {
    data: *const u8,
    len: usize,
}
impl Text {
    fn new(s: &str) -> Self {
        Self {
            data: s.as_ptr(),
            len: s.len(),
        }
    }
}
#[repr(C)]
pub struct Chna {
    uid: Text,
    channel: u16,
}
#[repr(C)]
pub struct OutputChna {
    track: u16,
    uid: Text,
    format: Text,
    pack: Text,
}
#[repr(C)]
pub struct View {
    strings: *const Text,
    strings_len: usize,
    indices: *const u32,
    indices_len: usize,
    programmes: *const Programme,
    programmes_len: usize,
    contents: *const Content,
    contents_len: usize,
    objects: *const Object,
    objects_len: usize,
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
    warnings: *const u32,
    warnings_len: usize,
}
pub struct Handle {
    document: Document,
    snapshot: Snapshot,
    texts: Vec<Text>,
    rate: u32,
}
pub struct Buffer {
    metadata: GeneratedMetadata,
    chna: Vec<OutputChna>,
}
impl Buffer {
    fn new(metadata: GeneratedMetadata) -> Self {
        let chna = metadata
            .chna
            .iter()
            .map(|v| OutputChna {
                track: v.track,
                uid: Text::new(&v.uid),
                format: Text::new(&v.format),
                pack: Text::new(&v.pack),
            })
            .collect();
        Self { metadata, chna }
    }
}
fn boundary(message: *mut u8, capacity: usize, f: impl FnOnce() -> Result<()>) -> i32 {
    let error = match catch_unwind(AssertUnwindSafe(f)) {
        Ok(Ok(())) => None,
        Ok(Err(e)) => Some(e),
        Err(_) => Some(Error {
            code: 6,
            message: "Rust ADM panic".to_owned(),
        }),
    };
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
unsafe fn input<'a, T>(data: *const T, len: usize) -> Result<&'a [T]> {
    if len == 0 {
        return Ok(&[]);
    }
    if data.is_null()
        || !(data as usize).is_multiple_of(align_of::<T>())
        || len > isize::MAX as usize / size_of::<T>()
    {
        return Err(Error::invalid("无效 ADM 缓冲区"));
    }
    Ok(unsafe { slice::from_raw_parts(data, len) })
}
unsafe fn text<'a>(value: Text) -> Result<&'a str> {
    str::from_utf8(unsafe { input(value.data, value.len)? })
        .map_err(|_| Error::invalid("无效 UTF-8 字符串"))
}

#[unsafe(no_mangle)]
pub unsafe extern "C" fn mradm_adm_parse(
    xml: *const u8,
    xml_len: usize,
    rate: u32,
    chna: *const Chna,
    chna_len: usize,
    output: *mut *mut Handle,
    message: *mut u8,
    capacity: usize,
) -> i32 {
    boundary(message, capacity, || {
        if output.is_null() {
            return Err(Error::invalid("缺少 ADM 输出句柄"));
        }
        unsafe {
            *output = ptr::null_mut();
        }
        let xml = str::from_utf8(unsafe { input(xml, xml_len)? })
            .map_err(|_| Error::xml("AXML 必须使用 UTF-8"))?;
        let entries = unsafe { input(chna, chna_len)? }
            .iter()
            .map(|c| Ok((unsafe { text(c.uid)? }.to_owned(), c.channel)))
            .collect::<Result<Vec<_>>>()?;
        let document = Document::parse(xml)?;
        let snapshot = Snapshot::extract(&document, rate, &entries)?;
        let texts = snapshot.strings.iter().map(|s| Text::new(s)).collect();
        unsafe {
            *output = Box::into_raw(Box::new(Handle {
                document,
                snapshot,
                texts,
                rate,
            }));
        }
        Ok(())
    })
}
#[unsafe(no_mangle)]
pub unsafe extern "C" fn mradm_adm_view(
    handle: *const Handle,
    output: *mut View,
    message: *mut u8,
    capacity: usize,
) -> i32 {
    boundary(message, capacity, || {
        if handle.is_null() || output.is_null() {
            return Err(Error::invalid("无效 ADM view 参数"));
        }
        let handle = unsafe { &*handle };
        let s = &handle.snapshot;
        unsafe {
            *output = View {
                strings: handle.texts.as_ptr(),
                strings_len: handle.texts.len(),
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
            };
        }
        Ok(())
    })
}
#[unsafe(no_mangle)]
pub unsafe extern "C" fn mradm_adm_destroy(handle: *mut Handle) {
    if !handle.is_null() {
        unsafe {
            drop(Box::from_raw(handle));
        }
    }
}
#[unsafe(no_mangle)]
pub unsafe extern "C" fn mradm_adm_patch(
    handle: *const Handle,
    patches: *const Patch,
    len: usize,
    output: *mut *mut Buffer,
    message: *mut u8,
    capacity: usize,
) -> i32 {
    boundary(message, capacity, || {
        if output.is_null() {
            return Err(Error::invalid("缺少 ADM 输出句柄"));
        }
        unsafe {
            *output = ptr::null_mut();
        }
        if handle.is_null() {
            return Err(Error::invalid("无效 ADM 句柄"));
        }
        let h = unsafe { &*handle };
        let axml = h
            .document
            .apply_patches(unsafe { input(patches, len)? }, h.rate)?;
        unsafe {
            *output = Box::into_raw(Box::new(Buffer::new(GeneratedMetadata {
                axml,
                chna: Vec::new(),
            })));
        }
        Ok(())
    })
}
#[unsafe(no_mangle)]
pub unsafe extern "C" fn mradm_adm_generate(
    kind: u32,
    name: Text,
    speakers: *const AdmSpeaker,
    labels: *const Text,
    count: usize,
    output: *mut *mut Buffer,
    message: *mut u8,
    capacity: usize,
) -> i32 {
    boundary(message, capacity, || {
        if output.is_null() {
            return Err(Error::invalid("缺少 ADM 输出句柄"));
        }
        unsafe {
            *output = ptr::null_mut();
        }
        let name = unsafe { text(name)? };
        let raw = unsafe { input(speakers, count)? };
        let labels = unsafe { input(labels, count)? };
        let speakers = raw
            .iter()
            .map(|s| {
                let label = labels
                    .get(s.label as usize)
                    .ok_or_else(|| Error::invalid("无效扬声器标签索引"))?;
                Ok(mradm_adm::Speaker {
                    label: unsafe { text(*label)? }.to_owned(),
                    azimuth: s.azimuth,
                    elevation: s.elevation,
                    lfe: s.lfe != 0,
                    azimuth_range: (s.ranges & 1 != 0).then_some((s.azimuth_min, s.azimuth_max)),
                    elevation_range: (s.ranges & 2 != 0)
                        .then_some((s.elevation_min, s.elevation_max)),
                })
            })
            .collect::<Result<Vec<_>>>()?;
        let metadata = mradm_adm::generate(kind, name, &speakers)?;
        unsafe {
            *output = Box::into_raw(Box::new(Buffer::new(metadata)));
        }
        Ok(())
    })
}
#[unsafe(no_mangle)]
pub unsafe extern "C" fn mradm_adm_buffer_view(
    handle: *const Buffer,
    xml: *mut Text,
    chna: *mut *const OutputChna,
    len: *mut usize,
    message: *mut u8,
    capacity: usize,
) -> i32 {
    boundary(message, capacity, || {
        if handle.is_null() || xml.is_null() || chna.is_null() || len.is_null() {
            return Err(Error::invalid("无效 ADM 输出参数"));
        }
        let h = unsafe { &*handle };
        unsafe {
            *xml = Text::new(&h.metadata.axml);
            *chna = h.chna.as_ptr();
            *len = h.chna.len();
        }
        Ok(())
    })
}
#[unsafe(no_mangle)]
pub unsafe extern "C" fn mradm_adm_buffer_destroy(handle: *mut Buffer) {
    if !handle.is_null() {
        unsafe {
            drop(Box::from_raw(handle));
        }
    }
}

#[cfg(test)]
mod tests {
    use super::*;
    #[test]
    fn failed_parse_clears_handle_and_errors_are_utf8() {
        let mut handle = ptr::dangling_mut();
        let mut error = [0u8; 16];
        let code = unsafe {
            mradm_adm_parse(
                ptr::null(),
                1,
                48000,
                ptr::null(),
                0,
                &mut handle,
                error.as_mut_ptr(),
                error.len(),
            )
        };
        assert_eq!(code, 1);
        assert!(handle.is_null());
        let end = error.iter().position(|b| *b == 0).unwrap();
        assert!(str::from_utf8(&error[..end]).is_ok());
        unsafe {
            mradm_adm_destroy(handle);
            mradm_adm_buffer_destroy(ptr::null_mut());
        }
    }
    #[test]
    fn panic_is_contained() {
        assert_eq!(boundary(ptr::null_mut(), 0, || panic!("test")), 6);
    }
    #[test]
    fn generated_buffer_has_a_bounded_lifetime() {
        unsafe {
            let mut handle = ptr::null_mut();
            assert_eq!(
                mradm_adm_generate(
                    5,
                    Text::new("Binaural"),
                    ptr::null(),
                    ptr::null(),
                    0,
                    &mut handle,
                    ptr::null_mut(),
                    0
                ),
                0
            );
            let mut xml = Text::new("");
            let mut chna = ptr::null();
            let mut len = 0;
            assert_eq!(
                mradm_adm_buffer_view(handle, &mut xml, &mut chna, &mut len, ptr::null_mut(), 0),
                0
            );
            assert_eq!(len, 2);
            assert!(text(xml).unwrap().contains("Binaural"));
            assert_eq!((*chna).track, 1);
            mradm_adm_buffer_destroy(handle);
        }
    }
}
