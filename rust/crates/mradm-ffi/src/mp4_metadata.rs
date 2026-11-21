//! MP4 metadata (PCM consistency 7.2 C): the iTunes-style `udta/meta/ilst` text items (©too, ©day,
//! ©cmt) appended to the top-level `moov` of an APAC `.m4a` or an IAMF `.mp4`, with the atoms,
//! checks and bytes of the C++ island it replaces (`write_mp4_metadata`, `metadata_io.cpp`).
//!
//! `moov` grows in place: it must end the file, or be followed by a `free` atom large enough to
//! give up the bytes, so no chunk offset (`stco` / `co64`) moves. The file is scanned atom header
//! by atom header and rewritten through a temporary file beside it.
use crate::adm::Text;
use crate::metadata_temp::MetadataTemp;
use mradm_engine::render::{MetadataFields, metadata_comment};
use mradm_engine::request::Error;
use std::fs::File;
use std::io::{self, BufReader, BufWriter, Read, Seek, SeekFrom, Write};
use std::path::Path;

const UNSUPPORTED: i32 = 2;
const IO_ERROR: i32 = 3;

type FourCC = [u8; 4];

const MOOV: &FourCC = b"moov";
const FREE: &FourCC = b"free";
const TOOL: &FourCC = b"\xA9too";
const DAY: &FourCC = b"\xA9day";
const COMMENT: &FourCC = b"\xA9cmt";

fn failure(code: i32, message: &str, path: &str) -> Error {
    Error::new(code, message, format!("path={path}"))
}

/// Whether `path` names an MP4 file (`.m4a` or `.mp4`, ASCII case-insensitive).
pub(crate) fn is_mp4(path: &str) -> bool {
    Path::new(path)
        .extension()
        .is_some_and(|e| e.eq_ignore_ascii_case("m4a") || e.eq_ignore_ascii_case("mp4"))
}

fn atom(out: &mut Vec<u8>, kind: &FourCC, payload: &[u8]) {
    out.extend_from_slice(&((payload.len() + 8) as u32).to_be_bytes());
    out.extend_from_slice(kind);
    out.extend_from_slice(payload);
}

/// A text item: `<kind>{ data{ type 1 (UTF-8), locale 0, value } }`.
fn text_item(out: &mut Vec<u8>, kind: &FourCC, value: &str) {
    if value.is_empty() {
        return;
    }
    let mut data = Vec::with_capacity(8 + value.len());
    data.extend_from_slice(&1u32.to_be_bytes());
    data.extend_from_slice(&0u32.to_be_bytes());
    data.extend_from_slice(value.as_bytes());
    let mut item = Vec::new();
    atom(&mut item, b"data", &data);
    atom(out, kind, &item);
}

/// The `udta` atom, or nothing when every item is empty.
fn udta(m: &MetadataFields) -> Vec<u8> {
    let mut items = Vec::new();
    text_item(&mut items, TOOL, &m.encoder);
    text_item(&mut items, DAY, &m.date_utc);
    text_item(&mut items, COMMENT, &metadata_comment(m));
    if items.is_empty() {
        return Vec::new();
    }
    // hdlr: version and flags, pre-defined, `mdir`, `appl`, two reserved words, an empty name.
    let mut hdlr = Vec::new();
    hdlr.extend_from_slice(&[0; 8]);
    hdlr.extend_from_slice(b"mdirappl");
    hdlr.extend_from_slice(&[0; 9]);
    let mut meta = vec![0; 4]; // FullBox version and flags.
    atom(&mut meta, b"hdlr", &hdlr);
    atom(&mut meta, b"ilst", &items);
    let mut meta_atom = Vec::new();
    atom(&mut meta_atom, b"meta", &meta);
    let mut out = Vec::new();
    atom(&mut out, b"udta", &meta_atom);
    out
}

#[derive(Clone, Copy, Debug, PartialEq)]
struct Atom {
    offset: u64,
    size: u64,
    header: u64,
    kind: FourCC,
}

impl Atom {
    fn end(&self) -> u64 {
        self.offset + self.size
    }
}

/// The atom at `pos` of a file of `len` bytes; `None` when its header is malformed (a size 0 atom
/// runs to the end of the file, size 1 has a 64-bit size).
fn next_atom(source: &mut (impl Read + Seek), pos: u64, len: u64) -> io::Result<Option<Atom>> {
    if pos + 8 > len {
        return Ok(None);
    }
    source.seek(SeekFrom::Start(pos))?;
    let mut header = [0; 8];
    source.read_exact(&mut header)?;
    let mut size = u64::from(u32::from_be_bytes([
        header[0], header[1], header[2], header[3],
    ]));
    let mut header_size = 8;
    if size == 1 {
        if pos + 16 > len {
            return Ok(None);
        }
        let mut large = [0; 8];
        source.read_exact(&mut large)?;
        size = u64::from_be_bytes(large);
        header_size = 16;
    } else if size == 0 {
        size = len - pos;
    }
    if size < header_size || size > len - pos {
        return Ok(None);
    }
    Ok(Some(Atom {
        offset: pos,
        size,
        header: header_size,
        kind: [header[4], header[5], header[6], header[7]],
    }))
}

/// The last top-level `moov` and the `free` atom right after it, if any.
enum Scan {
    Malformed,
    Found(Option<Atom>, Option<Atom>),
}

fn scan(source: &mut (impl Read + Seek), len: u64) -> io::Result<Scan> {
    let mut moov: Option<Atom> = None;
    let mut pos = 0;
    while pos < len {
        let Some(a) = next_atom(source, pos, len)? else {
            return Ok(Scan::Malformed);
        };
        if moov.is_some_and(|m| a.offset == m.end()) && &a.kind == FREE {
            return Ok(Scan::Found(moov, Some(a)));
        }
        if &a.kind == MOOV {
            moov = Some(a);
        }
        pos = a.end();
    }
    Ok(Scan::Found(moov, None))
}

fn copy_range(
    source: &mut (impl Read + Seek),
    dest: &mut impl Write,
    start: u64,
    end: u64,
) -> io::Result<()> {
    source.seek(SeekFrom::Start(start))?;
    let length = end - start;
    if io::copy(&mut source.take(length), dest)? != length {
        return Err(io::ErrorKind::UnexpectedEof.into());
    }
    Ok(())
}

/// `write_mp4_metadata`: appends the `udta` to the top-level `moov`, taking its bytes from the
/// following `free` atom unless `moov` ends the file. Nothing is read or written when every item is
/// empty.
pub(crate) fn write_metadata(path: &str, m: &MetadataFields) -> Result<(), Error> {
    let udta = udta(m);
    if udta.is_empty() {
        return Ok(());
    }
    let file = File::open(path)
        .map_err(|_| failure(IO_ERROR, "cannot open MP4 for metadata read", path))?;
    let read_error = |_| failure(IO_ERROR, "failed to read MP4 for metadata update", path);
    let len = file.metadata().map_err(read_error)?.len();
    let mut source = BufReader::new(file);
    let (moov, free) = match scan(&mut source, len).map_err(read_error)? {
        Scan::Malformed => return Err(failure(IO_ERROR, "malformed MP4 atom table", path)),
        Scan::Found(moov, free) => (moov, free),
    };
    let moov = moov.ok_or_else(|| failure(IO_ERROR, "MP4 metadata: missing moov atom", path))?;
    let insert = udta.len() as u64;
    if moov.header != 8 || moov.size + insert > u64::from(u32::MAX) {
        return Err(failure(
            UNSUPPORTED,
            "MP4 metadata: unsupported large moov atom",
            path,
        ));
    }
    let at_eof = moov.end() == len;
    let free = match free {
        _ if at_eof => None,
        Some(f) if f.header == 8 && f.size >= insert + 8 => Some(f),
        _ => {
            return Err(failure(
                UNSUPPORTED,
                "MP4 metadata: no adjacent free atom large enough for safe in-place moov growth",
                path,
            ));
        }
    };

    let output = Path::new(path);
    let (temporary, dest) = MetadataTemp::create(output)
        .map_err(|_| failure(IO_ERROR, "cannot open MP4 for metadata rewrite", path))?;
    let mut dest = BufWriter::new(dest);
    let rewrite = (|| -> io::Result<()> {
        copy_range(&mut source, &mut dest, 0, moov.offset)?;
        dest.write_all(&((moov.size + insert) as u32).to_be_bytes())?;
        copy_range(&mut source, &mut dest, moov.offset + 4, moov.end())?;
        dest.write_all(&udta)?;
        if let Some(f) = free {
            let shrunk = f.size - insert;
            dest.write_all(&(shrunk as u32).to_be_bytes())?;
            copy_range(&mut source, &mut dest, f.offset + 4, f.offset + shrunk)?;
            copy_range(&mut source, &mut dest, f.end(), len)?;
        }
        dest.flush()
    })();
    drop(dest);
    drop(source);
    let write_error = |_| failure(IO_ERROR, "failed to rewrite MP4 metadata", path);
    rewrite.map_err(write_error)?;
    temporary.install(output).map_err(write_error)
}

/// `MradmRustTestServices::mp4_write_metadata`: [`write_metadata`] for the APAC smoke test, which
/// checks that a real AudioToolbox `.m4a` still decodes afterwards (macOS). The texts are UTF-8;
/// returns the error code and copies the message, truncated and NUL-terminated, into `message`.
pub(crate) unsafe extern "C" fn mradm_mp4_write_metadata(
    path: Text,
    encoder: Text,
    date_utc: Text,
    renderer: Text,
    output_layout: Text,
    message: *mut u8,
    capacity: usize,
) -> i32 {
    let text = |t: Text| -> String {
        if t.len == 0 || t.data.is_null() {
            return String::new();
        }
        String::from_utf8_lossy(unsafe { std::slice::from_raw_parts(t.data, t.len) }).into_owned()
    };
    let run = || {
        let fields = MetadataFields {
            encoder: text(encoder),
            date_utc: text(date_utc),
            renderer: text(renderer),
            output_layout: text(output_layout),
            lufs: None,
            peak_dbtp: None,
        };
        write_metadata(&text(path), &fields)
    };
    let error = match std::panic::catch_unwind(std::panic::AssertUnwindSafe(run)) {
        Ok(Ok(())) => None,
        Ok(Err(e)) => Some((e.code, e.message)),
        Err(_) => Some((6, "MP4 metadata panic".to_owned())),
    };
    let text = error.as_ref().map_or("", |(_, m)| m.as_str());
    if !message.is_null() && capacity != 0 {
        let mut len = text.len().min(capacity - 1);
        while !text.is_char_boundary(len) {
            len -= 1;
        }
        unsafe {
            std::ptr::copy_nonoverlapping(text.as_ptr(), message, len);
            *message.add(len) = 0;
        }
    }
    error.map_or(0, |(code, _)| code)
}

#[cfg(test)]
mod tests {
    use super::*;
    use std::fs;
    use std::path::PathBuf;

    fn scratch(name: &str) -> PathBuf {
        let dir =
            std::env::temp_dir().join(format!("mradm-mp4-metadata-{}-{name}", std::process::id()));
        let _ = fs::remove_dir_all(&dir);
        fs::create_dir_all(&dir).unwrap();
        dir
    }

    fn fields() -> MetadataFields {
        MetadataFields {
            encoder: "MacinRender Test".into(),
            date_utc: "2026-05-23T03:42:00Z".into(),
            renderer: "binaural-hrtf".into(),
            output_layout: "binaural".into(),
            lufs: Some(-23.04),
            peak_dbtp: Some(-0.0),
        }
    }

    fn raw(kind: &[u8; 4], payload: &[u8]) -> Vec<u8> {
        let mut out = Vec::new();
        atom(&mut out, kind, payload);
        out
    }

    /// An independent spelling of the expected `udta` for [`fields`].
    fn expected_udta() -> Vec<u8> {
        let item = |kind: &[u8; 4], value: &str| {
            let mut data = vec![0, 0, 0, 1, 0, 0, 0, 0];
            data.extend_from_slice(value.as_bytes());
            raw(kind, &raw(b"data", &data))
        };
        let mut ilst = item(b"\xA9too", "MacinRender Test");
        ilst.extend(item(b"\xA9day", "2026-05-23T03:42:00Z"));
        ilst.extend(item(
            b"\xA9cmt",
            "renderer=binaural-hrtf layout=binaural loudness=-23.0LUFS peak=-0.00dBTP",
        ));
        let mut hdlr = vec![0; 8];
        hdlr.extend_from_slice(b"mdirappl");
        hdlr.extend_from_slice(&[0; 9]);
        let mut meta = vec![0; 4];
        meta.extend(raw(b"hdlr", &hdlr));
        meta.extend(raw(b"ilst", &ilst));
        raw(b"udta", &raw(b"meta", &meta))
    }

    fn write(dir: &Path, name: &str, bytes: &[u8]) -> String {
        let path = dir.join(name);
        fs::write(&path, bytes).unwrap();
        path.to_str().unwrap().to_owned()
    }

    fn leftovers(dir: &Path) -> Vec<String> {
        fs::read_dir(dir)
            .unwrap()
            .map(|e| e.unwrap().file_name().into_string().unwrap())
            .filter(|n| n.ends_with(".tmp"))
            .collect()
    }

    #[test]
    fn udta_matches_the_item_layout() {
        assert_eq!(udta(&fields()), expected_udta());
        // Empty items are left out; the comment alone remains.
        let m = MetadataFields {
            output_layout: "7.1.4".into(),
            ..MetadataFields::default()
        };
        let u = udta(&m);
        assert!(u.windows(4).any(|w| w == COMMENT));
        assert!(!u.windows(4).any(|w| w == TOOL || w == DAY));
        assert!(udta(&MetadataFields::default()).is_empty());
    }

    #[test]
    fn moov_at_the_end_of_the_file_grows() {
        let dir = scratch("eof");
        let ftyp = raw(b"ftyp", b"M4A \0\0\0\0");
        let mdat = raw(b"mdat", &[7; 100]);
        let moov = raw(b"moov", &raw(b"mvhd", &[1; 20]));
        let source = [ftyp.clone(), mdat.clone(), moov.clone()].concat();
        let path = write(&dir, "a.m4a", &source);
        write_metadata(&path, &fields()).unwrap();
        let u = expected_udta();
        let mut grown = moov.clone();
        grown[..4].copy_from_slice(&((moov.len() + u.len()) as u32).to_be_bytes());
        grown.extend(&u);
        assert_eq!(fs::read(&path).unwrap(), [ftyp, mdat, grown].concat());
        assert!(leftovers(&dir).is_empty());
    }

    #[test]
    fn a_following_free_atom_gives_up_its_bytes() {
        let dir = scratch("free");
        let u = expected_udta();
        let ftyp = raw(b"ftyp", b"mp42\0\0\0\0");
        let moov = raw(b"moov", &raw(b"trak", &[2; 30]));
        let free_payload: Vec<u8> = (0..u.len() + 40).map(|i| i as u8).collect();
        let free = raw(b"free", &free_payload);
        let mdat = raw(b"mdat", &[9; 64]);
        let source = [ftyp.clone(), moov.clone(), free.clone(), mdat.clone()].concat();
        let path = write(&dir, "a.mp4", &source);
        write_metadata(&path, &fields()).unwrap();
        let out = fs::read(&path).unwrap();
        assert_eq!(out.len(), source.len(), "the file keeps its size");
        let mut grown = moov.clone();
        grown[..4].copy_from_slice(&((moov.len() + u.len()) as u32).to_be_bytes());
        grown.extend(&u);
        let shrunk = free.len() - u.len();
        let mut rest = (shrunk as u32).to_be_bytes().to_vec();
        rest.extend_from_slice(b"free");
        // The free atom keeps the start of its payload.
        rest.extend_from_slice(&free[8..shrunk]);
        assert_eq!(out, [ftyp, grown, rest, mdat.clone()].concat());
        // A free atom exactly the size of the insert shrinks to its header.
        let exact = raw(b"free", &vec![0; u.len()]);
        let source = [raw(b"moov", &[]), exact, mdat.clone()].concat();
        let path = write(&dir, "exact.mp4", &source);
        write_metadata(&path, &fields()).unwrap();
        let mut moov = ((8 + u.len()) as u32).to_be_bytes().to_vec();
        moov.extend_from_slice(b"moov");
        moov.extend(&u);
        let free = [&[0, 0, 0, 8][..], b"free"].concat();
        assert_eq!(
            fs::read(&path).unwrap(),
            [moov, free, mdat.clone()].concat()
        );
        // A free atom that would shrink below its header is refused.
        let small = raw(b"free", &vec![0; u.len() - 1]);
        let source = [raw(b"moov", &[]), small, raw(b"mdat", &[])].concat();
        let path = write(&dir, "small.mp4", &source);
        let e = write_metadata(&path, &fields()).unwrap_err();
        assert_eq!(e.code, UNSUPPORTED);
        assert_eq!(
            e.message,
            "MP4 metadata: no adjacent free atom large enough for safe in-place moov growth"
        );
        assert_eq!(fs::read(&path).unwrap(), source);
    }

    #[test]
    fn layout_errors_leave_the_file_alone() {
        let dir = scratch("errors");
        let moov = raw(b"moov", &[]);
        let mdat = raw(b"mdat", &[1; 8]);
        let cases: Vec<(&str, Vec<u8>, i32, &str)> = vec![
            (
                "no-free",
                [moov.clone(), mdat.clone()].concat(),
                UNSUPPORTED,
                "MP4 metadata: no adjacent free atom large enough for safe in-place moov growth",
            ),
            (
                "missing",
                [raw(b"ftyp", &[]), mdat.clone()].concat(),
                IO_ERROR,
                "MP4 metadata: missing moov atom",
            ),
            (
                "short",
                [moov.clone(), vec![0, 0, 0, 4, b'f', b'r', b'e', b'e']].concat(),
                IO_ERROR,
                "malformed MP4 atom table",
            ),
            (
                "truncated",
                [moov.clone(), vec![0, 0, 0]].concat(),
                IO_ERROR,
                "malformed MP4 atom table",
            ),
            (
                "large-moov",
                [&[0, 0, 0, 1][..], b"moov", &16u64.to_be_bytes()].concat(),
                UNSUPPORTED,
                "MP4 metadata: unsupported large moov atom",
            ),
            (
                "large-free",
                [
                    moov.clone(),
                    [
                        &[0, 0, 0, 1][..],
                        b"free",
                        &1016u64.to_be_bytes(),
                        &[0; 1000],
                    ]
                    .concat(),
                ]
                .concat(),
                UNSUPPORTED,
                "MP4 metadata: no adjacent free atom large enough for safe in-place moov growth",
            ),
        ];
        for (name, bytes, code, message) in cases {
            let path = write(&dir, &format!("{name}.m4a"), &bytes);
            let e = write_metadata(&path, &fields()).unwrap_err();
            assert_eq!((e.code, e.message.as_str()), (code, message), "{name}");
            assert_eq!(e.context, format!("path={path}"), "{name}");
            assert_eq!(fs::read(&path).unwrap(), bytes, "{name}");
        }
        let missing = dir.join("gone.m4a").to_str().unwrap().to_owned();
        let e = write_metadata(&missing, &fields()).unwrap_err();
        assert_eq!(
            (e.code, e.message.as_str()),
            (IO_ERROR, "cannot open MP4 for metadata read")
        );
        // Empty fields touch nothing, not even a missing file.
        write_metadata(&missing, &MetadataFields::default()).unwrap();
        assert!(leftovers(&dir).is_empty());
    }

    #[test]
    fn atom_sizes_follow_the_header_rules() {
        let dir = scratch("sizes");
        // A 64-bit mdat before moov, the last moov wins, and a size-0 moov runs to the end.
        let large_mdat = [&[0, 0, 0, 1][..], b"mdat", &24u64.to_be_bytes(), &[5; 8]].concat();
        let first = raw(b"moov", &[1; 4]);
        let last = [&[0, 0, 0, 0][..], b"moov", &[3; 12]].concat();
        let source = [large_mdat.clone(), first.clone(), last.clone()].concat();
        let path = write(&dir, "a.M4A", &source);
        write_metadata(&path, &fields()).unwrap();
        let u = expected_udta();
        let mut grown = last.clone();
        grown[..4].copy_from_slice(&((last.len() + u.len()) as u32).to_be_bytes());
        grown.extend(&u);
        assert_eq!(
            fs::read(&path).unwrap(),
            [large_mdat, first, grown].concat()
        );
    }

    #[test]
    fn extensions() {
        assert!(is_mp4("a.m4a") && is_mp4("b.MP4") && is_mp4("c.M4a"));
        assert!(!is_mp4("a.mka") && !is_mp4("m4a") && !is_mp4("a.m4a.tmp"));
    }
}
