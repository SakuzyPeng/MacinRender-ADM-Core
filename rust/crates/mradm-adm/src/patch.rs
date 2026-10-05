pub use crate::records::Patch;
use crate::{Document, Error, Result, document::COMMON, xml::escape};
use std::{collections::BTreeMap, ops::Range};

#[derive(Default)]
struct ElementEdit {
    text: Option<String>,
    attributes: BTreeMap<String, Option<String>>,
}
#[derive(Default)]
struct Edits {
    existing: BTreeMap<u32, ElementEdit>,
    missing: BTreeMap<(u32, String), ElementEdit>,
}

impl Document {
    pub fn apply_patches(&self, patches: &[Patch], rate: u32) -> Result<String> {
        if rate == 0 {
            return Err(Error::invalid("采样率必须大于零"));
        }
        let mut groups = BTreeMap::<(u32, u32), Vec<&Patch>>::new();
        for patch in patches {
            if patch.present > 1
                || patch.original_present > 1
                || !patch.value.is_finite()
                || !patch.original.is_finite()
            {
                return Err(Error::invalid("无效 ADM 修改值"));
            }
            if patch.node & COMMON == 0 && patch.node as usize >= self.xml.nodes.len()
                || patch.node & COMMON != 0
                    && (patch.node & !COMMON) as usize >= self.common.nodes.len()
            {
                return Err(Error::invalid("无效 ADM 修改节点"));
            }
            if !(1..=14).contains(&patch.field) {
                return Err(Error::invalid("无效 ADM 修改字段"));
            }
            groups
                .entry((patch.node, patch.field))
                .or_default()
                .push(patch);
        }
        let mut edits = Edits::default();
        for ((node, field), group) in groups {
            if !group.iter().any(|p| p.changed()) {
                continue;
            }
            let patch = group[0];
            if group.iter().any(|p| !p.same_result(patch)) {
                return Err(Error::unsupported("共享 ADM 节点收到相互矛盾的修改"));
            }
            if node & COMMON != 0 {
                return Err(Error::unsupported(
                    "修改 common definitions 需要重建引用与 CHNA，当前回写不支持",
                ));
            }
            let kind = self.node(node).local.as_str();
            if kind != "audioObject" && kind != "audioBlockFormat"
                || kind == "audioObject" && field != 1 && field != 2
                || kind == "audioBlockFormat" && field == 2
            {
                return Err(Error::invalid("ADM 修改字段不适用于目标节点"));
            }
            let (element, attribute) = match field {
                1 => ("gain", None),
                2 => ("mute", None),
                3 => ("headLocked", None),
                4 => ("diffuse", None),
                5 => ("width", None),
                6 => ("height", None),
                7 => ("depth", None),
                8 => ("objectDivergence", None),
                9 => ("objectDivergence", Some("azimuthRange")),
                10 => ("objectDivergence", Some("positionRange")),
                11 => ("channelLock", None),
                12 => ("channelLock", Some("maxDistance")),
                13 => ("jumpPosition", None),
                14 => ("jumpPosition", Some("interpolationLength")),
                _ => unreachable!(),
            };
            if [2, 3, 11, 13].contains(&field) && patch.value != 0.0 && patch.value != 1.0 {
                return Err(Error::invalid("ADM 布尔修改必须为 0 或 1"));
            }
            if [1, 4, 5, 6, 7, 8, 9, 10, 12].contains(&field)
                && patch.present != 0
                && patch.value < 0.0
                || [4, 8].contains(&field) && patch.value > 1.0
            {
                return Err(Error::invalid("ADM 修改数值超出范围"));
            }
            let value = if field == 14 {
                // Preserve the previous samples -> truncated nanoseconds conversion.
                let ns = (u128::from(patch.samples) * 1_000_000_000 / u128::from(rate))
                    .min(i64::MAX as u128);
                format!("{}.{:09}", ns / 1_000_000_000, ns % 1_000_000_000)
            } else {
                patch.value.to_string()
            };
            let edit = if let Some(child) = self.child(node, element) {
                edits.existing.entry(child).or_default()
            } else {
                edits.missing.entry((node, element.to_owned())).or_default()
            };
            if let Some(attribute) = attribute {
                edit.attributes
                    .insert(attribute.to_owned(), (patch.present != 0).then_some(value));
            } else {
                edit.text = Some(value);
            }
            if field == 1 {
                edit.attributes
                    .insert("gainUnit".to_owned(), Some("linear".to_owned()));
            }
        }
        if edits.existing.is_empty() && edits.missing.is_empty() {
            return Ok(self.xml.source.clone());
        }
        let mut replacements = Vec::<(Range<usize>, String)>::new();
        let mut insertions = BTreeMap::<usize, String>::new();
        for (key, edit) in edits.existing {
            let node = self.node(key);
            let index = key as usize;
            for (name, value) in edit.attributes {
                if let Some(range) = self.xml.attribute_range(index, &name) {
                    if let Some(value) = value {
                        replacements.push((range, escape(&value)));
                    } else {
                        let begin = self.xml.source[node.start..range.start]
                            .rfind(&name)
                            .ok_or_else(|| Error::xml("无法定位 XML 属性"))?
                            + node.start;
                        replacements.push((begin..range.end + 1, String::new()));
                    }
                } else if let Some(value) = value {
                    insertions
                        .entry(node.open_end - if node.empty { 2 } else { 1 })
                        .or_default()
                        .push_str(&format!(" {name}=\"{}\"", escape(&value)));
                }
            }
            if let Some(value) = edit.text {
                if !node.children.is_empty() {
                    return Err(Error::unsupported("无法修改包含嵌套扩展的 ADM 标量"));
                }
                if node.empty {
                    replacements.push((
                        node.open_end - 2..node.open_end,
                        format!(">{}</{}>", escape(&value), node.name),
                    ));
                } else {
                    replacements.push((node.open_end..node.close_start, escape(&value)));
                }
            }
        }
        let mut empty_parents = BTreeMap::<u32, String>::new();
        for ((parent, name), edit) in edits.missing {
            if edit.text.is_none() && edit.attributes.values().all(Option::is_none) {
                continue;
            }
            let node = self.node(parent);
            let qualified = node.qualified(&name);
            let mut value = format!("<{qualified}");
            for (name, attribute) in edit.attributes {
                if let Some(attribute) = attribute {
                    value.push_str(&format!(" {name}=\"{}\"", escape(&attribute)));
                }
            }
            value.push('>');
            value.push_str(&escape(edit.text.as_deref().unwrap_or(if name == "gain" {
                "1"
            } else {
                "0"
            })));
            value.push_str(&format!("</{qualified}>"));
            if node.empty {
                empty_parents.entry(parent).or_default().push_str(&value);
            } else {
                insertions
                    .entry(node.close_start)
                    .or_default()
                    .push_str(&value);
            }
        }
        for (parent, children) in empty_parents {
            let node = self.node(parent);
            replacements.push((
                node.open_end - 2..node.open_end,
                format!(">{children}</{}>", node.name),
            ));
        }
        replacements.extend(insertions.into_iter().map(|(p, s)| (p..p, s)));
        replacements.sort_by_key(|(r, _)| (r.start, r.end));
        let mut output = String::new();
        let mut cursor = 0;
        for (range, text) in replacements {
            if range.start < cursor {
                return Err(Error::unsupported("ADM 修改范围重叠"));
            }
            output.push_str(&self.xml.source[cursor..range.start]);
            output.push_str(&text);
            cursor = range.end;
        }
        output.push_str(&self.xml.source[cursor..]);
        // A malformed generated document must never reach the container writer.
        Document::parse(&output)?;
        Ok(output)
    }
}
impl Patch {
    fn changed(&self) -> bool {
        self.present != self.original_present
            || self.present != 0
                && if self.field == 14 {
                    self.samples != self.original_samples
                } else {
                    self.value != self.original
                }
    }
    fn same_result(&self, other: &Self) -> bool {
        self.present == other.present
            && (self.present == 0
                || if self.field == 14 {
                    self.samples == other.samples
                } else {
                    self.value == other.value
                })
    }
}
