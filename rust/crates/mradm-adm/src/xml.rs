use crate::{Error, Result};
use quick_xml::{NsReader, events::Event, name::ResolveResult};
use std::ops::Range;

#[derive(Debug)]
pub(crate) struct Attribute {
    pub name: String,
    pub value: String,
}

#[derive(Debug)]
pub(crate) struct Node {
    pub name: String,
    pub local: String,
    pub namespace: String,
    pub attributes: Vec<Attribute>,
    pub text: String,
    pub children: Vec<usize>,
    pub start: usize,
    pub open_end: usize,
    pub close_start: usize,
    pub end: usize,
    pub empty: bool,
}

impl Node {
    pub fn attr(&self, name: &str) -> Option<&str> {
        self.attributes
            .iter()
            .find(|a| a.name == name)
            .map(|a| a.value.as_str())
    }
    pub fn qualified(&self, local: &str) -> String {
        self.name.rsplit_once(':').map_or_else(
            || local.to_owned(),
            |(prefix, _)| format!("{prefix}:{local}"),
        )
    }
}

#[derive(Debug)]
pub(crate) struct Tree {
    pub source: String,
    pub nodes: Vec<Node>,
    pub root: usize,
}

fn decoded(bytes: &[u8]) -> Result<String> {
    std::str::from_utf8(bytes)
        .map(str::to_owned)
        .map_err(|_| Error::xml("XML 必须使用 UTF-8"))
}

impl Tree {
    pub fn parse(source: &str) -> Result<Self> {
        let source_for_parser = source.trim_end_matches('\0');
        if source_for_parser.contains('\0') {
            return Err(Error::xml("XML 包含 NUL 字符"));
        }
        let mut reader = NsReader::from_str(source_for_parser);
        reader.config_mut().check_comments = true;
        let mut tree = Self {
            source: source.to_owned(),
            nodes: Vec::new(),
            root: 0,
        };
        let mut stack: Vec<usize> = Vec::new();
        let mut root = None;
        loop {
            let start = reader.buffer_position() as usize;
            let (namespace, event) = reader
                .read_resolved_event()
                .map_err(|e| Error::xml(format!("XML: {e}")))?;
            let ns = match namespace {
                ResolveResult::Bound(ns) => decoded(ns.as_ref())?,
                ResolveResult::Unbound => String::new(),
                ResolveResult::Unknown(_) => return Err(Error::xml("XML 命名空间前缀未定义")),
            };
            let end = reader.buffer_position() as usize;
            match event {
                Event::Start(ref element) | Event::Empty(ref element) => {
                    if stack.len() >= 1024 || tree.nodes.len() >= 0x7fff_ffff {
                        return Err(Error::unsupported("XML 结构过大或嵌套过深"));
                    }
                    let empty = matches!(event, Event::Empty(_));
                    let mut attributes = Vec::new();
                    for attr in element.attributes() {
                        let attr = attr.map_err(|e| Error::xml(format!("XML attribute: {e}")))?;
                        attributes.push(Attribute {
                            name: decoded(attr.key.as_ref())?,
                            value: attr
                                .decoded_and_normalized_value(
                                    quick_xml::XmlVersion::Implicit1_0,
                                    reader.decoder(),
                                )
                                .map_err(|e| Error::xml(e.to_string()))?
                                .into_owned(),
                        });
                    }
                    let index = tree.nodes.len();
                    if let Some(parent) = stack.last() {
                        tree.nodes[*parent].children.push(index);
                    } else if root.replace(index).is_some() {
                        return Err(Error::xml("XML 包含多个根元素"));
                    }
                    tree.nodes.push(Node {
                        name: decoded(element.name().as_ref())?,
                        local: decoded(element.local_name().as_ref())?,
                        namespace: ns,
                        attributes,
                        text: String::new(),
                        children: Vec::new(),
                        start,
                        open_end: end,
                        close_start: end,
                        end,
                        empty,
                    });
                    if !empty {
                        stack.push(index);
                    }
                }
                Event::End(_) => {
                    let index = stack
                        .pop()
                        .ok_or_else(|| Error::xml("XML 结束标签不匹配"))?;
                    tree.nodes[index].close_start = start;
                    tree.nodes[index].end = end;
                }
                Event::Text(text) => {
                    let value = text.decode().map_err(|e| Error::xml(e.to_string()))?;
                    let value = value.replace("\r\n", "\n").replace('\r', "\n");
                    if let Some(index) = stack.last() {
                        tree.nodes[*index].text.push_str(&value);
                    } else if !value.trim().is_empty() {
                        return Err(Error::xml("XML 根元素之外存在文本"));
                    }
                }
                Event::CData(text) => {
                    let index = stack
                        .last()
                        .ok_or_else(|| Error::xml("XML 根元素之外存在 CDATA"))?;
                    tree.nodes[*index]
                        .text
                        .push_str(&text.decode().map_err(|e| Error::xml(e.to_string()))?);
                }
                Event::GeneralRef(reference) => {
                    let index = stack
                        .last()
                        .ok_or_else(|| Error::xml("XML 根元素之外存在实体"))?;
                    if let Some(ch) = reference
                        .resolve_char_ref()
                        .map_err(|e| Error::xml(e.to_string()))?
                    {
                        tree.nodes[*index].text.push(ch);
                    } else {
                        let name = reference.decode().map_err(|e| Error::xml(e.to_string()))?;
                        let value = quick_xml::escape::resolve_predefined_entity(&name)
                            .ok_or_else(|| Error::xml(format!("XML 实体未定义: {name}")))?;
                        tree.nodes[*index].text.push_str(value);
                    }
                }
                Event::DocType(_) => {
                    return Err(Error::unsupported("ADM XML 不支持 DTD 或外部实体"));
                }
                Event::Decl(decl) => {
                    if root.is_some() {
                        return Err(Error::xml("XML 声明位置无效"));
                    }
                    if let Some(encoding) = decl.encoding() {
                        let encoding = encoding.map_err(|e| Error::xml(e.to_string()))?;
                        if !encoding.eq_ignore_ascii_case(b"utf-8") {
                            return Err(Error::unsupported("ADM XML 必须使用 UTF-8"));
                        }
                    }
                }
                Event::Eof => break,
                Event::Comment(_) | Event::PI(_) => {}
            }
        }
        if !stack.is_empty() {
            return Err(Error::xml("XML 文档被截断"));
        }
        tree.root = root.ok_or_else(|| Error::xml("XML 文档为空"))?;
        Ok(tree)
    }

    pub fn children<'a>(
        &'a self,
        index: usize,
        local: &'a str,
    ) -> impl Iterator<Item = usize> + 'a {
        let parent = &self.nodes[index];
        parent.children.iter().copied().filter(move |&child| {
            self.nodes[child].local == local && self.nodes[child].namespace == parent.namespace
        })
    }
    pub fn child(&self, index: usize, local: &str) -> Option<usize> {
        self.children(index, local).next()
    }

    /// Find the literal value range without reformatting unrelated attributes.
    pub fn attribute_range(&self, index: usize, name: &str) -> Option<Range<usize>> {
        let node = &self.nodes[index];
        let bytes = self.source.as_bytes();
        let mut p = node.start + 1 + node.name.len();
        while p < node.open_end {
            while p < node.open_end && bytes[p].is_ascii_whitespace() {
                p += 1;
            }
            let begin = p;
            while p < node.open_end
                && bytes[p] != b'='
                && !bytes[p].is_ascii_whitespace()
                && bytes[p] != b'/'
                && bytes[p] != b'>'
            {
                p += 1;
            }
            let end = p;
            while p < node.open_end && bytes[p].is_ascii_whitespace() {
                p += 1;
            }
            if p >= node.open_end || bytes[p] != b'=' {
                break;
            }
            p += 1;
            while p < node.open_end && bytes[p].is_ascii_whitespace() {
                p += 1;
            }
            let quote = bytes[p];
            p += 1;
            let value_start = p;
            while p < node.open_end && bytes[p] != quote {
                p += 1;
            }
            if &self.source[begin..end] == name {
                return Some(value_start..p);
            }
            p += 1;
        }
        None
    }
}

pub(crate) fn escape(text: &str) -> String {
    quick_xml::escape::escape(text).into_owned()
}
