//! Input import (PCM consistency 4.3): WAVE format, the CHNA channel policy and the ADM projection,
//! or a synthesized DirectSpeakers channel bed. Moved from the C++ scene and channel-bed importers
//! with the same checks, order, messages and locators.
use crate::error::{Context, Error, Result};
use crate::input::{
    InputChannel, MAX_INPUT_CHANNELS, channel_catalog, find_input_layout, input_layouts, lower,
    resolve_input_channel_labels,
};
use crate::policy::{Overlay, Policy};
use mradm_adm::{Document, Snapshot, records::*};
use mradm_wav::{Chna, ChunkReader, Reader, has_chunk};
use std::collections::BTreeMap;
use std::fs::File;
use std::io::BufReader;

#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub enum SourceKind {
    Adm,
    ChannelBed,
}

#[derive(Clone, Debug, PartialEq)]
pub struct Info {
    pub sample_rate: u32,
    pub channels: u16,
    pub frames: u64,
    pub source_kind: SourceKind,
    pub input_layout: String,
    pub input_channel_labels: Vec<String>,
}

/// The imported scene. Element records use the `mradm-adm` projection layout; a channel bed has
/// no ADM provenance, so its source records are left at their defaults and must not be read.
/// A semantic policy edits the records in place and the state they do not carry in `overlay`;
/// the pre-policy scene is kept for the report and the export patches.
#[derive(Debug)]
pub struct RenderScene {
    pub info: Info,
    pub snapshot: Snapshot,
    pub overlay: Overlay,
    pub original: Option<(Snapshot, Overlay)>,
    pub policy: Option<Policy>,
    pub policy_warnings: Vec<String>,
}

impl RenderScene {
    pub fn new(info: Info, snapshot: Snapshot) -> Self {
        Self {
            overlay: Overlay::of(&snapshot),
            info,
            snapshot,
            original: None,
            policy: None,
            policy_warnings: Vec::new(),
        }
    }

    /// Applies a semantic policy (once per import) and returns its warnings.
    pub fn apply_policy(&mut self, policy: Policy) -> &[String] {
        if self.original.is_none() {
            self.original = Some((self.snapshot.clone(), self.overlay.clone()));
        }
        self.policy_warnings = crate::policy::apply(self, &policy);
        self.policy = Some(policy);
        &self.policy_warnings
    }
}

#[derive(Clone, Copy, Debug, Default)]
pub struct ImportOptions<'a> {
    /// `None` / empty / "auto" selects ADM when AXML exists, otherwise the WAVE channel mask.
    pub input_layout: Option<&'a str>,
    /// Custom labels in file-channel order; mutually exclusive with an explicit layout.
    pub input_channel_labels: &'a [String],
}

/// Opens a path the way the WAVE boundary does: UTF-8 without NUL.
fn open(path: &[u8]) -> mradm_wav::Result<BufReader<File>> {
    if path.is_empty() {
        return Err(mradm_wav::Error::invalid("WAVE 路径为空"));
    }
    let text = std::str::from_utf8(path)
        .map_err(|_| mradm_wav::Error::invalid("WAVE 路径必须使用 UTF-8"))?;
    if text.contains('\0') {
        return Err(mradm_wav::Error::invalid("WAVE 路径包含 NUL"));
    }
    Ok(BufReader::new(File::open(text)?))
}

fn reader(path: &[u8]) -> Result<Reader<BufReader<File>>> {
    open(path)
        .and_then(Reader::new)
        .map_err(|e| Error::wav(e, Context::Path))
}

/// AXML payload and CHNA records; container errors carry the importer's locator.
fn adm_chunks(path: &[u8]) -> Result<(Vec<u8>, Option<Chna>)> {
    let input = |e| Error::wav(e, Context::Input);
    let mut chunks = ChunkReader::new(open(path).map_err(input)?).map_err(input)?;
    let mut axml = Vec::new();
    if let Some(chunk) = chunks.chunks().iter().find(|c| c.id == *b"axml").copied() {
        let size = usize::try_from(chunk.size)
            .map_err(|_| Error::unsupported("WAVE chunk is too large", Context::Input))?;
        if size != 0 {
            axml.resize(size, 0);
            chunks.read_chunk(*b"axml", &mut axml).map_err(input)?;
        }
    }
    let chna = chunks
        .metadata(*b"chna", u64::MAX)
        .map_err(input)?
        .map(|data| Chna::decode_import(&data))
        .transpose()
        .map_err(input)?;
    Ok((axml, chna))
}

/// CHNA UIDs are fixed 12-byte fields padded with spaces or NULs; matching ignores the padding and
/// the case of ASCII letters. A repeated UID keeps its last track.
fn uid_channels(chna: Option<Chna>) -> Result<BTreeMap<Vec<u8>, u16>> {
    let mut channels = BTreeMap::new();
    for entry in chna.map(|c| c.entries).unwrap_or_default() {
        if entry.track_index == 0 {
            return Err(Error::io("CHNA trackIndex 必须从 1 开始", Context::Input));
        }
        let mut uid = entry.uid.to_vec();
        while uid.last().is_some_and(|&b| b == b' ' || b == 0) {
            uid.pop();
        }
        uid.make_ascii_uppercase();
        if !uid.is_empty() {
            channels.insert(uid, entry.track_index - 1);
        }
    }
    Ok(channels)
}

/// ADM BW64 import: the AXML projection over the CHNA track mapping.
pub fn import_adm(path: &[u8]) -> Result<RenderScene> {
    let info = reader(path)?.info();
    let (axml, chna) = adm_chunks(path)?;
    let channels = uid_channels(chna)?;
    if axml.is_empty() {
        return Err(Error::io("axml chunk 缺失或为空", Context::Input));
    }
    let xml = std::str::from_utf8(&axml)
        .map_err(|_| Error::adm(mradm_adm::Error::xml("AXML 必须使用 UTF-8")))?;
    let entries = channels
        .into_iter()
        .map(|(uid, channel)| {
            String::from_utf8(uid)
                .map(|uid| (uid, channel))
                .map_err(|_| Error::adm(mradm_adm::Error::invalid("无效 UTF-8 字符串")))
        })
        .collect::<Result<Vec<_>>>()?;
    let document = Document::parse(xml).map_err(Error::adm)?;
    let rate = info.format.sample_rate;
    let snapshot = Snapshot::extract(&document, rate, &entries).map_err(Error::adm)?;
    Ok(RenderScene::new(
        Info {
            sample_rate: rate,
            channels: info.format.channels,
            frames: info.frames,
            source_kind: SourceKind::Adm,
            input_layout: String::new(),
            input_channel_labels: Vec::new(),
        },
        snapshot,
    ))
}

fn explicit_layout(layout: Option<&str>) -> Option<&str> {
    layout.filter(|l| !l.is_empty() && lower(l) != "auto")
}

/// Rendering-oriented import: ADM BW64, or an ordinary channel-based WAVE as a DirectSpeakers bed.
pub fn import(path: &[u8], options: ImportOptions<'_>) -> Result<RenderScene> {
    let forced_channel_bed =
        explicit_layout(options.input_layout).is_some() || !options.input_channel_labels.is_empty();
    let axml = open(path)
        .and_then(|file| has_chunk(file, *b"axml"))
        .map_err(|_| Error::io("input is not a RIFF/RF64/BW64 WAVE file", Context::Input))?;
    if !forced_channel_bed && axml {
        return import_adm(path);
    }
    channel_bed(path, options, forced_channel_bed && axml)
}

fn channel_bed(path: &[u8], options: ImportOptions<'_>, ignored_axml: bool) -> Result<RenderScene> {
    let info = reader(path)?.info();
    let channels = u32::from(info.format.channels);
    if channels == 0 || channels as usize > MAX_INPUT_CHANNELS {
        return Err(Error::invalid(
            format!(
                "channel-bed input must contain 1-{MAX_INPUT_CHANNELS} channels; got {channels}"
            ),
            Context::Input,
        ));
    }
    let bits = info.format.sample_format.bits();
    let integer = info.format.sample_format.is_integer();
    if !(integer && matches!(bits, 16 | 24 | 32) || !integer && bits == 32) {
        return Err(Error::unsupported(
            format!(
                "channel-bed input supports PCM16/24/32 or float32 WAVE; got {bits}-bit format"
            ),
            Context::Input,
        ));
    }
    if info.format.sample_rate == 0 || info.frames == 0 {
        return Err(Error::invalid(
            "channel-bed input has no audio frames",
            Context::Input,
        ));
    }
    let (layout, mapping) =
        input_mapping(info.format.channel_mask.unwrap_or(0), channels, options)?;
    if mapping.len() != channels as usize {
        return Err(Error::invalid(
            format!(
                "input mapping '{layout}' defines {} channels but the file contains {channels}",
                mapping.len()
            ),
            Context::Input,
        ));
    }
    let info = Info {
        sample_rate: info.format.sample_rate,
        channels: info.format.channels,
        frames: info.frames,
        source_kind: SourceKind::ChannelBed,
        input_layout: layout,
        input_channel_labels: mapping.iter().map(|c| c.token.clone()).collect(),
    };
    let snapshot = channel_bed_snapshot(&mapping, info.frames, ignored_axml);
    Ok(RenderScene::new(info, snapshot))
}

fn input_mapping(
    mask: u32,
    channels: u32,
    options: ImportOptions<'_>,
) -> Result<(String, Vec<InputChannel>)> {
    let explicit = explicit_layout(options.input_layout);
    if explicit.is_some() && !options.input_channel_labels.is_empty() {
        return Err(Error::invalid(
            "input layout and custom input-channel labels are mutually exclusive",
            Context::None,
        ));
    }
    if let Some(raw) = explicit {
        let layout = find_input_layout(raw).ok_or_else(|| {
            Error::invalid(
                format!("unknown input layout '{raw}'; use mradm input-layouts to list presets"),
                Context::None,
            )
        })?;
        return Ok((layout.id.to_owned(), layout.channels.clone()));
    }
    if !options.input_channel_labels.is_empty() {
        let catalog = channel_catalog();
        let indices = resolve_input_channel_labels(options.input_channel_labels)?;
        return Ok((
            "custom".to_owned(),
            indices.into_iter().map(|i| catalog[i].clone()).collect(),
        ));
    }
    if mask == 0 {
        return Err(Error::invalid(
            "channel-bed input has no recognised WAVE channel mask; pass --input-layout or --input-channels",
            Context::Input,
        ));
    }
    let positions = mask.count_ones();
    if positions != channels {
        return Err(Error::invalid(
            format!(
                "WAVE channel mask 0x{mask:X} contains {positions} positions but the file has {channels} channels"
            ),
            Context::Input,
        ));
    }
    let layout = input_layouts()
        .iter()
        .find(|layout| layout.wave_channel_mask == mask)
        .ok_or_else(|| {
            Error::unsupported(
                format!("WAVE channel mask 0x{mask:X} is not recognised; pass --input-layout or --input-channels"),
                Context::Input,
            )
        })?;
    let channels = if layout.wave_mask_channels.is_empty() {
        &layout.channels
    } else {
        &layout.wave_mask_channels
    };
    Ok((layout.id.to_owned(), channels.clone()))
}

#[derive(Default)]
struct Strings {
    snapshot: Snapshot,
}
impl Strings {
    fn id(&mut self, value: &str) -> u32 {
        let id = self.snapshot.strings.len() as u32;
        self.snapshot.strings.push(value.to_owned());
        id
    }
    fn list(&mut self, values: &[&str]) -> Span {
        let offset = self.snapshot.indices.len();
        for value in values {
            let id = self.id(value);
            self.snapshot.indices.push(id);
        }
        Span {
            offset,
            len: values.len(),
        }
    }
}

/// One `AO_CHANNEL_BED` object with a DirectSpeakers track per file channel; LFE channels carry a
/// 120 Hz low-pass and no position.
fn channel_bed_snapshot(channels: &[InputChannel], frames: u64, ignored_axml: bool) -> Snapshot {
    let mut s = Strings::default();
    let name = s.id("Channel Bed");
    let tracks_offset = s.snapshot.tracks.len();
    for (index, channel) in channels.iter().enumerate() {
        let uid = s.id(&format!("ATU_CHANNEL_BED_{:02}", index + 1));
        let labels = s.list(&[channel.speaker_label]);
        let pack_id = s.id("AP_CHANNEL_BED");
        s.snapshot.tracks.push(Track {
            uid,
            channel: index as i32,
            blocks: Span::default(),
            ds_blocks: Span {
                offset: s.snapshot.direct_blocks.len(),
                len: 1,
            },
        });
        s.snapshot.direct_blocks.push(DirectBlock {
            node: NONE,
            labels,
            pack_id,
            has_position: u32::from(!channel.is_lfe),
            azimuth: channel.azimuth,
            elevation: channel.elevation,
            distance: 1.0,
            gain: 1.0,
            low_pass_present: u32::from(channel.is_lfe),
            low_pass: if channel.is_lfe { 120.0 } else { 0.0 },
            start: 0,
            end: frames,
            ..DirectBlock::default()
        });
    }
    let object = Object {
        node: NONE,
        id: s.id("AO_CHANNEL_BED"),
        name,
        gain: 1.0,
        end: frames,
        tracks: Span {
            offset: tracks_offset,
            len: channels.len(),
        },
        labels: s.list(&["channel-bed"]),
        ..Object::default()
    };
    s.snapshot.objects.push(object);
    let content = Content {
        id: s.id("ACO_CHANNEL_BED"),
        name,
        language: NONE,
        object_ids: s.list(&["AO_CHANNEL_BED"]),
        dialogue_kind: s.id("non-dialogue"),
        content_kind: NONE,
        ..Content::default()
    };
    s.snapshot.contents.push(content);
    let programme = Programme {
        id: s.id("APR_CHANNEL_BED"),
        name,
        language: NONE,
        content_ids: s.list(&["ACO_CHANNEL_BED"]),
        start: 0,
        end_present: 1,
        end: frames,
        ..Programme::default()
    };
    s.snapshot.programmes.push(programme);
    if ignored_axml {
        let warning = s.id(
            "explicit channel-bed input mapping selected; embedded ADM AXML metadata was ignored",
        );
        s.snapshot.warnings.push(warning);
    }
    s.snapshot
}
