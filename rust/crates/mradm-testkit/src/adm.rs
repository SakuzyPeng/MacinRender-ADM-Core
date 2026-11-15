//! Test-only ADM XML authoring (formerly `tests/support/adm_fixture.h`). This is not an ADM parser
//! or production model: values are written independently of `macin-adm`, so a fixture cannot
//! inherit a bug of the code it tests. The text, attribute order, element order and identifiers
//! are those of the C++ author, byte for byte.
use std::collections::BTreeMap;
use std::fmt::Write;

/// A value as `std::ostream << std::setprecision(17)` wrote it: floats as `%.17g` of the value
/// widened to `f64`, integers in decimal, `bool` as `1` / `0`.
pub trait Value {
    fn text(&self) -> String;
}
impl Value for f64 {
    fn text(&self) -> String {
        g17(*self)
    }
}
impl Value for f32 {
    fn text(&self) -> String {
        g17(f64::from(*self))
    }
}
impl Value for i32 {
    fn text(&self) -> String {
        self.to_string()
    }
}
impl Value for u32 {
    fn text(&self) -> String {
        self.to_string()
    }
}
impl Value for bool {
    fn text(&self) -> String {
        u8::from(*self).to_string()
    }
}
impl Value for &str {
    fn text(&self) -> String {
        (*self).to_owned()
    }
}
impl Value for String {
    fn text(&self) -> String {
        self.clone()
    }
}

/// `printf("%.17g", value)`.
pub fn g17(value: f64) -> String {
    g(value, 17)
}

/// `printf("%.*g", precision, value)` (`precision` at least 1): `std::ostream` writes floats this
/// way, with precision 6 by default.
pub fn g(value: f64, precision: usize) -> String {
    let precision = precision.max(1);
    if value == 0.0 {
        return if value.is_sign_negative() { "-0" } else { "0" }.to_owned();
    }
    if !value.is_finite() {
        return if value.is_nan() {
            "nan".to_owned()
        } else if value < 0.0 {
            "-inf".to_owned()
        } else {
            "inf".to_owned()
        };
    }
    // `{:.Ne}` rounds to `precision` significant digits and gives the exponent `%g` decides on.
    let scientific = format!("{value:.*e}", precision - 1);
    let (mantissa, exponent) = scientific.split_once('e').expect("exponent");
    let exponent: i32 = exponent.parse().expect("exponent value");
    let trim = |text: &str| -> String {
        if text.contains('.') {
            text.trim_end_matches('0').trim_end_matches('.').to_owned()
        } else {
            text.to_owned()
        }
    };
    if exponent < -4 || exponent >= precision as i32 {
        let sign = if exponent < 0 { '-' } else { '+' };
        format!("{}e{sign}{:02}", trim(mantissa), exponent.abs())
    } else {
        trim(&format!(
            "{value:.*}",
            (precision as i32 - 1 - exponent) as usize
        ))
    }
}

/// XML text escaping of the C++ author (all five predefined entities).
pub fn escaped(value: &str) -> String {
    let mut out = String::with_capacity(value.len());
    for c in value.chars() {
        match c {
            '&' => out.push_str("&amp;"),
            '<' => out.push_str("&lt;"),
            '>' => out.push_str("&gt;"),
            '"' => out.push_str("&quot;"),
            '\'' => out.push_str("&apos;"),
            _ => out.push(c),
        }
    }
    out
}

/// A time as `HH:MM:SS.nnnnnnnnn`.
pub fn time(ns: i64) -> String {
    let seconds = ns / 1_000_000_000;
    format!(
        "{:02}:{:02}:{:02}.{:09}",
        seconds / 3600,
        (seconds / 60) % 60,
        seconds % 60,
        ns % 1_000_000_000
    )
}

/// One parameter: an attribute (`attribute`), an element with text, attributes and children, or
/// one of the markers `#text`, `#position`, `#offset` and `#list`.
#[derive(Clone, Debug, Default, PartialEq)]
pub struct Param {
    pub name: String,
    pub text: String,
    pub attribute: bool,
    pub attrs: BTreeMap<String, String>,
    pub children: Vec<Param>,
}

impl Param {
    /// An element parameter.
    pub fn element(name: &str, value: impl Value) -> Self {
        Self {
            name: name.to_owned(),
            text: value.text(),
            ..Self::default()
        }
    }
    /// An attribute parameter.
    pub fn attribute(name: &str, value: impl Value) -> Self {
        Self {
            name: name.to_owned(),
            text: value.text(),
            attribute: true,
            ..Self::default()
        }
    }
    /// The element text of a compound parameter (`ChannelLockFlag`, `Divergence`, ...).
    pub fn text(value: impl Value) -> Self {
        Self::element("#text", value)
    }
    /// An empty compound parameter (`ChannelLock`, `ObjectDivergence`, `JumpPosition`, ...).
    pub fn compound(name: &str, values: impl IntoIterator<Item = Param>) -> Self {
        let mut p = Self {
            name: name.to_owned(),
            ..Self::default()
        };
        for v in values {
            p.set(v);
        }
        p
    }
    /// `Param::set_parameter`.
    pub fn set(&mut self, value: Param) {
        if value.name == "#text" {
            self.text = value.text;
            return;
        }
        if value.attribute {
            self.attrs.insert(value.name, value.text);
            return;
        }
        match self
            .children
            .iter_mut()
            .find(|p| p.name == value.name && !p.attrs.contains_key("coordinate"))
        {
            Some(p) => *p = value,
            None => self.children.push(value),
        }
    }
    /// The same parameter with an extra attribute.
    pub fn with_attr(mut self, key: &str, value: &str) -> Self {
        self.attrs.insert(key.to_owned(), value.to_owned());
        self
    }
}

/// A position or offset (`SphericalPosition`, `CartesianSpeakerPosition`, ...): one `position`
/// element per coordinate, defaults first.
fn position(cartesian: bool, offset: bool, values: impl IntoIterator<Item = Param>) -> Param {
    let mut p = Param {
        name: if offset { "#offset" } else { "#position" }.to_owned(),
        ..Param::default()
    };
    let set = |p: &mut Param, value: Param| {
        let element = Param::element(
            if p.name == "#offset" {
                "positionOffset"
            } else {
                "position"
            },
            value.text,
        )
        .with_attr("coordinate", &value.name);
        match p
            .children
            .iter_mut()
            .find(|q| q.attrs.get("coordinate") == Some(&value.name))
        {
            Some(q) => *q = element,
            None => p.children.push(element),
        }
    };
    if !offset {
        let defaults = if cartesian {
            [x(0), y(0), z(0)]
        } else {
            [azimuth(0), elevation(0), distance(1)]
        };
        for d in defaults {
            set(&mut p, d);
        }
    }
    for v in values {
        set(&mut p, v);
    }
    p
}

pub fn spherical(values: impl IntoIterator<Item = Param>) -> Param {
    position(false, false, values)
}
pub fn cartesian_position(values: impl IntoIterator<Item = Param>) -> Param {
    position(true, false, values)
}
pub fn spherical_offset(values: impl IntoIterator<Item = Param>) -> Param {
    position(false, true, values)
}
pub fn cartesian_offset(values: impl IntoIterator<Item = Param>) -> Param {
    position(true, true, values)
}

macro_rules! elements {
    ($($fn:ident => $name:literal),* $(,)?) => {
        $(pub fn $fn(value: impl Value) -> Param { Param::element($name, value) })*
    };
}
macro_rules! attributes {
    ($($fn:ident => $name:literal),* $(,)?) => {
        $(pub fn $fn(value: impl Value) -> Param { Param::attribute($name, value) })*
    };
}
elements! {
    width => "width", height => "height", depth => "depth", diffuse => "diffuse",
    head_locked => "headLocked", mute => "mute", screen_ref => "screenRef", cartesian => "cartesian",
    order => "order", degree => "degree", speaker_label => "speakerLabel",
    integrated_loudness => "integratedLoudness", max_true_peak => "maxTruePeak",
    loudness_range => "loudnessRange", max_momentary => "maxMomentary",
    max_short_term => "maxShortTerm", dialogue_loudness => "dialogueLoudness",
    normalization => "normalization", nfc_ref_dist => "nfcRefDist",
    azimuth => "azimuth", elevation => "elevation", distance => "distance",
    x => "X", y => "Y", z => "Z",
}
attributes! {
    programme_name => "audioProgrammeName", content_name => "audioContentName",
    object_name => "audioObjectName", pack_name => "audioPackFormatName",
    channel_name => "audioChannelFormatName", stream_name => "audioStreamFormatName",
    track_name => "audioTrackFormatName", programme_language => "audioProgrammeLanguage",
    content_language => "audioContentLanguage", importance => "importance",
    dialogue_id => "dialogue", label_language => "language", loudness_method => "loudnessMethod",
    azimuth_range => "azimuthRange", position_range => "positionRange",
    max_distance => "maxDistance",
}
pub fn rtime(ns: i64) -> Param {
    Param::attribute("rtime", time(ns))
}
pub fn duration(ns: i64) -> Param {
    Param::attribute("duration", time(ns))
}
pub fn start(ns: i64) -> Param {
    Param::attribute("start", time(ns))
}
pub fn end(ns: i64) -> Param {
    Param::attribute("end", time(ns))
}
/// `InterpolationLength`: seconds as a double.
pub fn interpolation_length(seconds: f64) -> Param {
    Param::attribute("interpolationLength", seconds)
}
pub fn gain_db(value: f64) -> Param {
    Param::element("gain", value).with_attr("gainUnit", "dB")
}
pub fn gain_linear(value: f64) -> Param {
    Param::element("gain", value)
}
/// `Frequency{LowPass{value}}`.
pub fn low_pass(value: f32) -> Param {
    Param::element("frequency", value).with_attr("typeDefinition", "lowPass")
}
pub fn channel_lock(values: impl IntoIterator<Item = Param>) -> Param {
    Param::compound("channelLock", values)
}
pub fn object_divergence(values: impl IntoIterator<Item = Param>) -> Param {
    Param::compound("objectDivergence", values)
}
pub fn jump_position(values: impl IntoIterator<Item = Param>) -> Param {
    Param::compound("jumpPosition", values)
}
pub fn label(values: impl IntoIterator<Item = Param>) -> Param {
    Param::compound("label", values)
}
pub fn loudness_metadata(values: impl IntoIterator<Item = Param>) -> Param {
    Param::compound("loudnessMetadata", values)
}
/// `LoudnessMetadatas`: every entry appended.
pub fn list(values: impl IntoIterator<Item = Param>) -> Param {
    Param {
        name: "#list".to_owned(),
        children: values.into_iter().collect(),
        ..Param::default()
    }
}

/// Attributes and child parameters of an element or block (`Properties`).
#[derive(Clone, Debug, Default, PartialEq)]
pub struct Properties {
    pub attributes: BTreeMap<String, String>,
    pub parameters: Vec<Param>,
}

impl Properties {
    /// `Properties::set`.
    pub fn set(&mut self, p: Param) {
        if p.attribute {
            self.attributes.insert(p.name, p.text);
            return;
        }
        if p.name == "#position" || p.name == "#offset" {
            let key = if p.name == "#position" {
                "position"
            } else {
                "positionOffset"
            };
            self.parameters.retain(|v| v.name != key);
            self.parameters.extend(p.children);
            return;
        }
        if p.name == "#list" {
            self.parameters.extend(p.children);
            return;
        }
        match self.parameters.iter_mut().find(|v| v.name == p.name) {
            Some(v) => *v = p,
            None => self.parameters.push(p),
        }
    }
    /// `Properties::add`: always appended.
    pub fn add(&mut self, p: Param) {
        self.parameters.push(p);
    }
    /// `DialogueContent::voiceover`.
    pub fn set_voiceover(&mut self) {
        self.set(Param::element("dialogue", 1).with_attr("dialogueContentKind", "2"));
    }
    pub fn with(mut self, values: impl IntoIterator<Item = Param>) -> Self {
        for v in values {
            self.set(v);
        }
        self
    }
}

/// An Objects block: a spherical origin position, then `values`.
pub fn objects_block(values: impl IntoIterator<Item = Param>) -> Properties {
    Properties::default().with([spherical([])]).with(values)
}
/// A DirectSpeakers block: a spherical speaker position, then `values`.
pub fn direct_speakers_block(values: impl IntoIterator<Item = Param>) -> Properties {
    objects_block(values)
}
/// An HOA block.
pub fn hoa_block(values: impl IntoIterator<Item = Param>) -> Properties {
    Properties::default().with(values)
}

/// `TypeDefinition`.
#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub enum Type {
    DirectSpeakers = 1,
    Matrix = 2,
    Objects = 3,
    Hoa = 4,
    Binaural = 5,
}

/// An element in a [`Document`].
#[derive(Clone, Copy, Debug, PartialEq, Eq, Hash)]
pub struct Id(usize);

#[derive(Clone, Debug)]
struct Entity {
    tag: &'static str,
    id: String,
    ty: Type,
    props: Properties,
    refs: Vec<Id>,
    owned: bool,
    objects: Vec<Properties>,
    direct: Vec<Properties>,
    hoa: Vec<Properties>,
}

/// The document: an arena of elements and the order they joined it. An element joins when it is
/// [added](Document::add), and with it every element it already references; a reference made from
/// an element that has joined adds the target at once (the C++ ownership rules).
#[derive(Clone, Debug, Default)]
pub struct Document {
    entities: Vec<Entity>,
    elements: Vec<Id>,
}

/// Which block list an [`Document::block`] call appends to.
#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub enum BlockKind {
    Objects,
    DirectSpeakers,
    Hoa,
}

impl Document {
    pub fn new() -> Self {
        Self::default()
    }
    fn create(&mut self, tag: &'static str, ty: Type, values: Vec<Param>) -> Id {
        let id = Id(self.entities.len());
        self.entities.push(Entity {
            tag,
            id: String::new(),
            ty,
            props: Properties::default(),
            refs: Vec::new(),
            owned: false,
            objects: Vec::new(),
            direct: Vec::new(),
            hoa: Vec::new(),
        });
        for v in values {
            self.set(id, v);
        }
        id
    }
    pub fn programme(&mut self, values: impl IntoIterator<Item = Param>) -> Id {
        self.create(
            "audioProgramme",
            Type::Objects,
            values.into_iter().collect(),
        )
    }
    pub fn content(&mut self, values: impl IntoIterator<Item = Param>) -> Id {
        self.create("audioContent", Type::Objects, values.into_iter().collect())
    }
    pub fn object(&mut self, values: impl IntoIterator<Item = Param>) -> Id {
        self.create("audioObject", Type::Objects, values.into_iter().collect())
    }
    pub fn pack(&mut self, ty: Type, values: impl IntoIterator<Item = Param>) -> Id {
        self.create("audioPackFormat", ty, values.into_iter().collect())
    }
    pub fn channel(&mut self, ty: Type, values: impl IntoIterator<Item = Param>) -> Id {
        self.create("audioChannelFormat", ty, values.into_iter().collect())
    }
    /// A PCM stream format.
    pub fn stream(&mut self, values: impl IntoIterator<Item = Param>) -> Id {
        let id = self.create(
            "audioStreamFormat",
            Type::Objects,
            values.into_iter().collect(),
        );
        self.set_pcm(id);
        id
    }
    /// A PCM track format.
    pub fn track(&mut self, values: impl IntoIterator<Item = Param>) -> Id {
        let id = self.create(
            "audioTrackFormat",
            Type::Objects,
            values.into_iter().collect(),
        );
        self.set_pcm(id);
        id
    }
    pub fn track_uid(&mut self) -> Id {
        self.create("audioTrackUID", Type::Objects, Vec::new())
    }
    /// `set(FormatDefinition::pcm)`.
    fn set_pcm(&mut self, id: Id) {
        let a = &mut self.entities[id.0].props.attributes;
        a.insert("formatLabel".to_owned(), "0001".to_owned());
        a.insert("formatDefinition".to_owned(), "pcm".to_owned());
    }
    /// `Entity::set` of a parameter.
    pub fn set(&mut self, id: Id, value: Param) {
        self.entities[id.0].props.set(value);
    }
    /// `Entity::add` of a parameter.
    pub fn add_param(&mut self, id: Id, value: Param) {
        self.entities[id.0].props.add(value);
    }
    /// Appends a block.
    pub fn block(&mut self, id: Id, kind: BlockKind, block: Properties) {
        let e = &mut self.entities[id.0];
        match kind {
            BlockKind::Objects => e.objects.push(block),
            BlockKind::DirectSpeakers => e.direct.push(block),
            BlockKind::Hoa => e.hoa.push(block),
        }
    }
    /// `Document::add`.
    pub fn add(&mut self, id: Id) {
        if self.elements.contains(&id) {
            return;
        }
        self.elements.push(id);
        self.entities[id.0].owned = true;
        let refs = self.entities[id.0].refs.clone();
        for r in refs {
            self.add(r);
        }
    }
    /// `Entity::add_reference` (and `set_reference`).
    pub fn reference(&mut self, from: Id, to: Id) {
        if self.entities[from.0].owned {
            self.add(to);
        }
        self.entities[from.0].refs.push(to);
    }
    /// The element's identifier (after [`reassign_ids`](Self::reassign_ids)).
    pub fn id(&self, id: Id) -> &str {
        &self.entities[id.0].id
    }

    /// `reassign_ids`: sequential identifiers per kind and type, in document order.
    pub fn reassign_ids(&mut self) {
        let mut counters: BTreeMap<String, u32> = BTreeMap::new();
        let next = |counters: &mut BTreeMap<String, u32>, key: String| {
            let c = counters.entry(key).or_insert(0);
            *c += 1;
            *c
        };
        let order = self.elements.clone();
        for &e in &order {
            let entity = &self.entities[e.0];
            let prefix = match entity.tag {
                "audioProgramme" => "APR",
                "audioContent" => "ACO",
                "audioObject" => "AO",
                _ => "",
            };
            let id = if !prefix.is_empty() {
                Some(format!(
                    "{prefix}_{}",
                    hex(0x1000 + next(&mut counters, prefix.to_owned()), 4)
                ))
            } else if entity.tag == "audioTrackUID" {
                Some(format!(
                    "ATU_{}",
                    hex(next(&mut counters, "ATU".to_owned()), 8)
                ))
            } else if entity.tag == "audioChannelFormat" || entity.tag == "audioPackFormat" {
                let prefix = if entity.tag == "audioChannelFormat" {
                    "AC"
                } else {
                    "AP"
                };
                let ty = hex(entity.ty as u32, 4);
                let n = next(&mut counters, format!("{prefix}{ty}"));
                Some(format!("{prefix}_{ty}{}", hex(0x1000 + n, 4)))
            } else {
                None
            };
            if let Some(id) = id {
                self.entities[e.0].id = id;
            }
        }
        for (tag, from, prefix, suffix) in [
            ("audioStreamFormat", "audioChannelFormat", "AS", ""),
            ("audioTrackFormat", "audioStreamFormat", "AT", "_01"),
        ] {
            for &e in &order {
                if self.entities[e.0].tag != tag {
                    continue;
                }
                let refs = self.entities[e.0].refs.clone();
                for r in refs {
                    if self.entities[r.0].tag == from {
                        self.entities[e.0].ty = self.entities[r.0].ty;
                    }
                }
                let ty = hex(self.entities[e.0].ty as u32, 4);
                let n = next(&mut counters, format!("{prefix}{ty}"));
                self.entities[e.0].id = format!("{prefix}_{ty}{}{suffix}", hex(0x1000 + n, 4));
            }
        }
    }

    /// `write_xml`.
    pub fn xml(&self) -> String {
        let mut out = String::from(
            "<?xml version=\"1.0\" encoding=\"UTF-8\"?><ebuCoreMain><coreMetadata><format><audioFormatExtended>",
        );
        for &e in &self.elements {
            let e = &self.entities[e.0];
            let id_name = if e.tag == "audioTrackUID" {
                "UID".to_owned()
            } else {
                format!("{}ID", e.tag)
            };
            let _ = write!(out, "<{} {}=\"{}\"", e.tag, id_name, e.id);
            for (key, value) in &e.props.attributes {
                let _ = write!(out, " {key}=\"{}\"", escaped(value));
            }
            if e.tag == "audioChannelFormat" || e.tag == "audioPackFormat" {
                let _ = write!(out, " typeLabel=\"{}\"", hex(e.ty as u32, 4));
            }
            out.push('>');
            properties_xml(&mut out, &e.props, &format!("{}Label", e.tag));
            let mut block_id = 0u32;
            for block in e.objects.iter().chain(&e.direct).chain(&e.hoa) {
                block_id += 1;
                let _ = write!(
                    out,
                    "<audioBlockFormat audioBlockFormatID=\"AB_{}_{}\"",
                    e.id.get(3..).unwrap_or(""),
                    hex(block_id, 8)
                );
                for (key, value) in &block.attributes {
                    let _ = write!(out, " {key}=\"{}\"", escaped(value));
                }
                out.push('>');
                properties_xml(&mut out, block, "label");
                out.push_str("</audioBlockFormat>");
            }
            for &r in &e.refs {
                let next = &self.entities[r.0];
                let tag = if next.tag == "audioTrackUID" {
                    "audioTrackUIDRef".to_owned()
                } else {
                    format!("{}IDRef", next.tag)
                };
                let _ = write!(out, "<{tag}>{}</{tag}>", next.id);
            }
            let _ = write!(out, "</{}>", e.tag);
        }
        out.push_str("</audioFormatExtended></format></coreMetadata></ebuCoreMain>");
        out
    }
}

/// Lower-case hexadecimal of at least `width` digits.
pub fn hex(value: u32, width: usize) -> String {
    format!("{value:0width$x}")
}

fn param_xml(out: &mut String, p: &Param, name: &str) {
    let _ = write!(out, "<{name}");
    for (key, value) in &p.attrs {
        let _ = write!(out, " {key}=\"{}\"", escaped(value));
    }
    let _ = write!(out, ">{}", escaped(&p.text));
    for child in &p.children {
        param_xml(out, child, &child.name);
    }
    let _ = write!(out, "</{name}>");
}

fn properties_xml(out: &mut String, p: &Properties, label_name: &str) {
    for parameter in &p.parameters {
        let name = if parameter.name == "label" {
            label_name
        } else {
            &parameter.name
        };
        param_xml(out, parameter, name);
    }
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn values_are_printed_as_percent_17g() {
        assert_eq!(30.0f32.text(), "30");
        assert_eq!((-97.5f32).text(), "-97.5");
        assert_eq!(0.05f64.text(), "0.050000000000000003");
        assert_eq!(0.1f32.text(), "0.10000000149011612");
        assert_eq!(1e20f64.text(), "1e+20");
        assert_eq!(1e-5f64.text(), "1.0000000000000001e-05");
        assert_eq!(123456789.0f64.text(), "123456789");
        assert_eq!((-0.0f64).text(), "-0");
        assert_eq!(true.text(), "1");
        assert_eq!(g(0.1, 6), "0.1");
        assert_eq!(g(1234567.0, 6), "1.23457e+06");
        assert_eq!(g(0.000012345, 6), "1.2345e-05");
        assert_eq!(g(f64::from(1.0f32 / 3.0), 6), "0.333333");
    }

    #[test]
    fn times_have_nine_fraction_digits() {
        assert_eq!(time(500_000_000), "00:00:00.500000000");
        assert_eq!(time(3_725_000_000_001), "01:02:05.000000001");
    }
}
