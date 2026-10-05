use crate::{
    Error, Result,
    xml::{Node, Tree},
};
use std::{collections::HashMap, sync::OnceLock};

pub(crate) const COMMON: u32 = 1 << 31;
static COMMON_DEFINITIONS: OnceLock<Result<Tree>> = OnceLock::new();

#[derive(Debug)]
pub struct Document {
    pub(crate) xml: Tree,
    pub(crate) common: &'static Tree,
    pub(crate) ids: HashMap<String, u32>,
    pub(crate) elements: Vec<u32>,
}

pub(crate) fn canonical_id(value: &str) -> String {
    let value = value.trim_end_matches([' ', '\0']).trim();
    value.split_once('_').map_or_else(
        || value.to_owned(),
        |(prefix, suffix)| {
            format!(
                "{}_{}",
                prefix.to_ascii_uppercase(),
                suffix.to_ascii_lowercase()
            )
        },
    )
}

fn id_attribute(kind: &str) -> Option<(&'static str, &'static str)> {
    Some(match kind {
        "audioProgramme" => ("audioProgrammeID", "APR"),
        "audioContent" => ("audioContentID", "ACO"),
        "audioObject" => ("audioObjectID", "AO"),
        "audioPackFormat" => ("audioPackFormatID", "AP"),
        "audioChannelFormat" => ("audioChannelFormatID", "AC"),
        "audioStreamFormat" => ("audioStreamFormatID", "AS"),
        "audioTrackFormat" => ("audioTrackFormatID", "AT"),
        "audioTrackUID" => ("UID", "ATU"),
        _ => return None,
    })
}

fn validate_id(value: &str, prefix: &str) -> Result<()> {
    let parts: Vec<_> = value.split('_').collect();
    let widths: &[usize] = match prefix {
        "APR" | "ACO" | "AO" => &[4],
        "AT" => &[8, 2],
        _ => &[8],
    };
    if parts.first().copied() != Some(prefix)
        || parts.len() != widths.len() + 1
        || !parts[1..].iter().zip(widths).all(|(part, width)| {
            part.len() == *width && part.bytes().all(|c| c.is_ascii_hexdigit())
        })
    {
        return Err(Error::xml(format!("无效 ADM ID: {value}")));
    }
    Ok(())
}

fn afe(tree: &Tree, common: bool) -> Result<usize> {
    let root = &tree.nodes[tree.root];
    if root.local != "ebuCoreMain" && !(common && root.local == "ituADM") {
        return Err(Error::xml(
            "缺少 ebuCoreMain/coreMetadata/format/audioFormatExtended",
        ));
    }
    let mut index = tree.root;
    for name in ["coreMetadata", "format", "audioFormatExtended"] {
        let children: Vec<_> = tree.children(index, name).collect();
        if children.len() != 1 {
            return Err(Error::xml(format!("需要唯一的 {name} 元素")));
        }
        index = children[0];
    }
    Ok(index)
}

impl Document {
    pub fn parse(source: &str) -> Result<Self> {
        let common = COMMON_DEFINITIONS
            .get_or_init(|| Tree::parse(include_str!("../assets/common_definitions.xml")))
            .as_ref()
            .map_err(Clone::clone)?;
        let xml = Tree::parse(source)?;
        let local_afe = afe(&xml, false)?;
        let common_afe = afe(common, true)?;
        let mut doc = Self {
            xml,
            common,
            ids: HashMap::new(),
            elements: Vec::new(),
        };
        for (tree, parent, flag) in [(common, common_afe, COMMON), (&doc.xml, local_afe, 0)] {
            for &index in &tree.nodes[parent].children {
                let node = &tree.nodes[index];
                if node.namespace != tree.nodes[parent].namespace {
                    continue;
                }
                if let Some((attr, prefix)) = id_attribute(&node.local) {
                    let raw_id = node
                        .attr(attr)
                        .ok_or_else(|| Error::xml(format!("{} 缺少 {attr}", node.local)))?;
                    let id = canonical_id(raw_id);
                    validate_id(&id, prefix)?;
                    let key = index as u32 | flag;
                    if doc.ids.insert(id.clone(), key).is_some() {
                        return Err(Error::xml(format!("重复 ADM ID: {id}")));
                    }
                    if flag == 0 {
                        doc.elements.push(key);
                    }
                }
            }
        }
        // Resolve every modeled relationship, including those not used by a renderer.
        for &key in doc.ids.values() {
            let node = doc.node(key);
            for child in doc.all_children(key) {
                let reference = doc.node(child);
                let prefix = match reference.local.as_str() {
                    "audioContentIDRef" => "ACO",
                    "audioObjectIDRef" => "AO",
                    "audioPackFormatIDRef" => "AP",
                    "audioChannelFormatIDRef" => "AC",
                    "audioStreamFormatIDRef" => "AS",
                    "audioTrackFormatIDRef" => "AT",
                    "audioTrackUIDRef" => "ATU",
                    _ => continue,
                };
                let id = canonical_id(&reference.text);
                validate_id(&id, prefix)?;
                if id != "ATU_00000000" && !doc.ids.contains_key(&id) {
                    return Err(Error::xml(format!("{} 引用了不存在的 {id}", node.local)));
                }
            }
            if matches!(
                node.local.as_str(),
                "audioPackFormat" | "audioChannelFormat"
            ) {
                doc.type_of(key)?;
            }
        }
        doc.check_cycles("audioObject", "audioObjectIDRef")?;
        doc.check_cycles("audioPackFormat", "audioPackFormatIDRef")?;
        Ok(doc)
    }
    pub(crate) fn tree(&self, key: u32) -> &Tree {
        if key & COMMON == 0 {
            &self.xml
        } else {
            self.common
        }
    }
    pub(crate) fn node(&self, key: u32) -> &Node {
        &self.tree(key).nodes[(key & !COMMON) as usize]
    }
    pub(crate) fn all_children(&self, key: u32) -> Vec<u32> {
        let node = self.node(key);
        let tree = self.tree(key);
        node.children
            .iter()
            .copied()
            .filter(|&n| tree.nodes[n].namespace == node.namespace)
            .map(|n| n as u32 | (key & COMMON))
            .collect()
    }
    pub(crate) fn children(&self, key: u32, name: &str) -> Vec<u32> {
        self.tree(key)
            .children((key & !COMMON) as usize, name)
            .map(|n| n as u32 | (key & COMMON))
            .collect()
    }
    pub(crate) fn child(&self, key: u32, name: &str) -> Option<u32> {
        self.tree(key)
            .child((key & !COMMON) as usize, name)
            .map(|n| n as u32 | (key & COMMON))
    }
    pub(crate) fn text(&self, key: u32, name: &str) -> Option<&str> {
        self.child(key, name).map(|k| self.node(k).text.as_str())
    }
    pub(crate) fn refs(&self, key: u32, name: &str) -> Vec<u32> {
        self.children(key, name)
            .iter()
            .filter_map(|k| self.ids.get(&canonical_id(&self.node(*k).text)).copied())
            .collect()
    }
    pub(crate) fn reference(&self, key: u32, name: &str) -> Option<u32> {
        self.refs(key, name).first().copied()
    }
    pub(crate) fn id(&self, key: u32) -> String {
        let n = self.node(key);
        canonical_id(
            n.attr(id_attribute(&n.local).map_or("", |(a, _)| a))
                .unwrap_or(""),
        )
    }
    pub(crate) fn of_type(&self, name: &str) -> Vec<u32> {
        self.elements
            .iter()
            .copied()
            .filter(|k| self.node(*k).local == name)
            .collect()
    }
    pub(crate) fn type_of(&self, key: u32) -> Result<u32> {
        let node = self.node(key);
        let label = node
            .attr("typeLabel")
            .map(|v| u32::from_str_radix(v, 16).map_err(|_| Error::xml("无效 typeLabel")))
            .transpose()?;
        let definition = node
            .attr("typeDefinition")
            .map(|v| match v {
                "DirectSpeakers" => Ok(1),
                "Matrix" => Ok(2),
                "Objects" => Ok(3),
                "HOA" => Ok(4),
                "Binaural" => Ok(5),
                _ => Err(Error::xml("无效 typeDefinition")),
            })
            .transpose()?;
        if label.is_some() && definition.is_some() && label != definition {
            return Err(Error::xml("typeLabel 与 typeDefinition 不一致"));
        }
        label
            .or(definition)
            .filter(|v| (1..=5).contains(v))
            .ok_or_else(|| Error::xml("ADM 元素缺少有效类型"))
    }
    /// Deliberately retains the existing TrackFormat -> StreamFormat chain and pack fallback.
    pub(crate) fn channel(&self, uid: u32) -> Option<u32> {
        let tf = self.reference(uid, "audioTrackFormatIDRef")?;
        let sf = self.reference(tf, "audioStreamFormatIDRef")?;
        self.reference(sf, "audioChannelFormatIDRef")
    }
    fn check_cycles(&self, kind: &str, edge: &str) -> Result<()> {
        let mut marks = HashMap::<u32, u8>::new();
        for root in self.of_type(kind) {
            let mut work = vec![(root, false)];
            while let Some((key, exiting)) = work.pop() {
                if exiting {
                    marks.insert(key, 2);
                    continue;
                }
                match marks.get(&key) {
                    Some(1) => return Err(Error::xml(format!("{kind} 引用包含循环"))),
                    Some(2) => continue,
                    _ => {}
                }
                marks.insert(key, 1);
                work.push((key, true));
                for next in self.refs(key, edge) {
                    work.push((next, false));
                }
            }
        }
        Ok(())
    }
}

pub(crate) fn number(text: &str) -> Result<f64> {
    let value = text
        .trim()
        .parse::<f64>()
        .map_err(|_| Error::xml(format!("无效数值: {text}")))?;
    if !value.is_finite() {
        return Err(Error::xml("ADM 数值必须有限"));
    }
    Ok(value)
}
pub(crate) fn boolean(text: &str) -> Result<u32> {
    match text.trim() {
        "0" | "false" => Ok(0),
        "1" | "true" => Ok(1),
        _ => Err(Error::xml(format!("无效布尔值: {text}"))),
    }
}
pub(crate) fn samples(time: &str, rate: u32) -> Result<u64> {
    let (clock, fraction) = time
        .split_once('.')
        .ok_or_else(|| Error::xml("无效 ADM 时间"))?;
    let parts = clock
        .split(':')
        .map(|p| p.parse::<u128>().map_err(|_| Error::xml("无效 ADM 时间")))
        .collect::<Result<Vec<_>>>()?;
    if parts.len() != 3 {
        return Err(Error::xml("无效 ADM 时间"));
    }
    let whole = parts[0]
        .checked_mul(3600)
        .and_then(|n| parts[1].checked_mul(60).and_then(|m| n.checked_add(m)))
        .and_then(|n| n.checked_add(parts[2]))
        .ok_or_else(|| Error::xml("ADM 时间溢出"))?;
    let ns = if let Some((n, d)) = fraction.split_once('S') {
        let numerator = n
            .parse::<u128>()
            .map_err(|_| Error::xml("无效 ADM 分数时间"))?;
        let denominator = d
            .parse::<u128>()
            .map_err(|_| Error::xml("无效 ADM 分数时间"))?;
        if denominator == 0 {
            return Err(Error::xml("ADM 时间分母为零"));
        }
        numerator
            .checked_mul(1_000_000_000)
            .ok_or_else(|| Error::xml("ADM 时间溢出"))?
            / denominator
    } else {
        if fraction.is_empty()
            || fraction.len() > 9
            || !fraction.bytes().all(|b| b.is_ascii_digit())
        {
            return Err(Error::xml("无效 ADM 小数时间"));
        }
        fraction
            .parse::<u128>()
            .map_err(|_| Error::xml("无效 ADM 时间"))?
            * 10u128.pow(9 - fraction.len() as u32)
    };
    let ns = whole
        .checked_mul(1_000_000_000)
        .and_then(|v| v.checked_add(ns))
        .ok_or_else(|| Error::xml("ADM 时间溢出"))?;
    let value = ns
        .checked_mul(u128::from(rate))
        .and_then(|v| v.checked_add(500_000_000))
        .ok_or_else(|| Error::xml("ADM 时间溢出"))?
        / 1_000_000_000;
    Ok(value.min(u128::from(u64::MAX)) as u64)
}
