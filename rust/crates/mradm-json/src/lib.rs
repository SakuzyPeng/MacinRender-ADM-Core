//! JSON compatible with the project's former nlohmann/json 3.12.0 handling (PCM consistency 4.4).
//!
//! Parsing follows nlohmann's acceptance: an optional UTF-8 BOM, RFC 8259 grammar with strict
//! UTF-8 strings, no nesting limit, later duplicate keys replacing earlier ones, and nlohmann's
//! number classes (a non-negative integer is unsigned, a negative one signed, `-0` the signed
//! integer 0, integers outside 64 bits become doubles, and a double that overflows is the
//! `out_of_range.406` error). Objects keep their keys in byte order, as `std::map` did.
//!
//! [`dump`] reproduces `basic_json::dump(indent)` byte for byte: the same separators, lower-case
//! `\u00xx` escapes for control characters only, `null` for non-finite numbers and nlohmann's
//! Grisu2 double formatting (`dtoa`). Store `f32` values widened to `f64`, as nlohmann did.
//!
//! Only the text of syntax errors is the project's own; every other error message reproduces the
//! nlohmann `what()` text that the C++ callers embedded.
#![forbid(unsafe_code)]

mod dtoa;

use std::collections::BTreeMap;
use std::fmt;

pub type Map = BTreeMap<String, Value>;

#[derive(Clone, Debug, Default, PartialEq)]
pub enum Value {
    #[default]
    Null,
    Bool(bool),
    Unsigned(u64),
    Integer(i64),
    Float(f64),
    String(String),
    Array(Vec<Value>),
    Object(Map),
}

impl Drop for Value {
    /// Tears nested containers down without recursion; nlohmann places no depth limit on input.
    fn drop(&mut self) {
        let mut stack = match self {
            Value::Array(items) if !items.is_empty() => std::mem::take(items),
            Value::Object(map) if !map.is_empty() => std::mem::take(map).into_values().collect(),
            _ => return,
        };
        while let Some(mut value) = stack.pop() {
            match &mut value {
                Value::Array(items) => stack.append(items),
                Value::Object(map) => stack.extend(std::mem::take(map).into_values()),
                _ => {}
            }
        }
    }
}

impl Value {
    /// nlohmann's `type_name()`, as used in its type-error messages.
    pub fn type_name(&self) -> &'static str {
        match self {
            Value::Null => "null",
            Value::Bool(_) => "boolean",
            Value::Unsigned(_) | Value::Integer(_) | Value::Float(_) => "number",
            Value::String(_) => "string",
            Value::Array(_) => "array",
            Value::Object(_) => "object",
        }
    }

    pub fn is_number(&self) -> bool {
        matches!(
            self,
            Value::Unsigned(_) | Value::Integer(_) | Value::Float(_)
        )
    }

    pub fn as_object(&self) -> Option<&Map> {
        match self {
            Value::Object(map) => Some(map),
            _ => None,
        }
    }

    pub fn as_array(&self) -> Option<&[Value]> {
        match self {
            Value::Array(items) => Some(items),
            _ => None,
        }
    }

    pub fn as_str(&self) -> Option<&str> {
        match self {
            Value::String(s) => Some(s),
            _ => None,
        }
    }

    pub fn as_bool(&self) -> Option<bool> {
        match self {
            Value::Bool(b) => Some(*b),
            _ => None,
        }
    }

    /// `get<double>()` of a number.
    pub fn as_f64(&self) -> Option<f64> {
        match *self {
            Value::Unsigned(u) => Some(u as f64),
            Value::Integer(i) => Some(i as f64),
            Value::Float(f) => Some(f),
            _ => None,
        }
    }

    /// `get<float>()` of a number: integers convert directly, without rounding through double.
    pub fn as_f32(&self) -> Option<f32> {
        match *self {
            Value::Unsigned(u) => Some(u as f32),
            Value::Integer(i) => Some(i as f32),
            Value::Float(f) => Some(f as f32),
            _ => None,
        }
    }

    /// `get<int>()` of a number. Floats truncate; values outside `int` were undefined in C++.
    pub fn as_i32(&self) -> Option<i32> {
        match *self {
            Value::Unsigned(u) => Some(u as i32),
            Value::Integer(i) => Some(i as i32),
            Value::Float(f) => Some(f as i32),
            _ => None,
        }
    }

    /// `get<uint32_t>()` of an unsigned number (`is_number_unsigned()`), truncating as C++ did.
    pub fn as_unsigned_u32(&self) -> Option<u32> {
        match *self {
            Value::Unsigned(u) => Some(u as u32),
            _ => None,
        }
    }

    /// nlohmann's `operator==` for two numbers of any class.
    pub fn number_eq(&self, other: &Value) -> bool {
        use Value::{Float, Integer, Unsigned};
        match (self, other) {
            (Unsigned(a), Unsigned(b)) => a == b,
            (Integer(a), Integer(b)) => a == b,
            (Float(a), Float(b)) => a == b,
            (Unsigned(a), Integer(b)) | (Integer(b), Unsigned(a)) => *a as i64 == *b,
            (Unsigned(a), Float(b)) | (Float(b), Unsigned(a)) => *a as f64 == *b,
            (Integer(a), Float(b)) | (Float(b), Integer(a)) => *a as f64 == *b,
            _ => false,
        }
    }
}

impl From<bool> for Value {
    fn from(v: bool) -> Self {
        Value::Bool(v)
    }
}
impl From<f32> for Value {
    /// Widened to double, as nlohmann stores every float.
    fn from(v: f32) -> Self {
        Value::Float(f64::from(v))
    }
}
impl From<f64> for Value {
    fn from(v: f64) -> Self {
        Value::Float(v)
    }
}
impl From<u64> for Value {
    fn from(v: u64) -> Self {
        Value::Unsigned(v)
    }
}
impl From<u32> for Value {
    fn from(v: u32) -> Self {
        Value::Unsigned(u64::from(v))
    }
}
impl From<usize> for Value {
    fn from(v: usize) -> Self {
        Value::Unsigned(v as u64)
    }
}
impl From<i64> for Value {
    fn from(v: i64) -> Self {
        Value::Integer(v)
    }
}
impl From<i32> for Value {
    fn from(v: i32) -> Self {
        Value::Integer(i64::from(v))
    }
}
impl From<&str> for Value {
    fn from(v: &str) -> Self {
        Value::String(v.to_owned())
    }
}
impl From<String> for Value {
    fn from(v: String) -> Self {
        Value::String(v)
    }
}
impl<T: Into<Value>> From<Option<T>> for Value {
    fn from(v: Option<T>) -> Self {
        v.map_or(Value::Null, Into::into)
    }
}
impl From<Vec<Value>> for Value {
    fn from(v: Vec<Value>) -> Self {
        Value::Array(v)
    }
}
impl From<Map> for Value {
    fn from(v: Map) -> Self {
        Value::Object(v)
    }
}

/// `[json.exception.type_error.302] type must be <expected>, but is <type>`.
pub fn type_must_be(expected: &str, actual: &Value) -> String {
    format!(
        "[json.exception.type_error.302] type must be {expected}, but is {}",
        actual.type_name()
    )
}

/// `[json.exception.type_error.304] cannot use at() with <type>`.
pub fn cannot_use_at(actual: &Value) -> String {
    format!(
        "[json.exception.type_error.304] cannot use at() with {}",
        actual.type_name()
    )
}

/// `[json.exception.out_of_range.403] key '<key>' not found`.
pub fn key_not_found(key: &str) -> String {
    format!("[json.exception.out_of_range.403] key '{key}' not found")
}

/// nlohmann's `at(key)`: the value or its exception text.
pub fn at<'a>(value: &'a Value, key: &str) -> Result<&'a Value, String> {
    match value {
        Value::Object(map) => map.get(key).ok_or_else(|| key_not_found(key)),
        other => Err(cannot_use_at(other)),
    }
}

#[derive(Clone, Debug, PartialEq, Eq)]
pub enum ParseError {
    /// Not JSON; the description is the project's own (nlohmann's text is not reproduced).
    Syntax {
        line: usize,
        column: usize,
        reason: &'static str,
    },
    /// A number whose double value overflows.
    NumberOverflow(String),
}

impl fmt::Display for ParseError {
    fn fmt(&self, f: &mut fmt::Formatter<'_>) -> fmt::Result {
        match self {
            ParseError::Syntax {
                line,
                column,
                reason,
            } => write!(f, "JSON 语法错误（第 {line} 行第 {column} 列）：{reason}"),
            ParseError::NumberOverflow(token) => write!(
                f,
                "[json.exception.out_of_range.406] number overflow parsing '{token}'"
            ),
        }
    }
}

impl std::error::Error for ParseError {}

enum Frame {
    Array(Vec<Value>),
    Object(Map, String),
}

struct Parser<'a> {
    bytes: &'a [u8],
    pos: usize,
}

impl<'a> Parser<'a> {
    fn error(&self, reason: &'static str) -> ParseError {
        let consumed = &self.bytes[..self.pos.min(self.bytes.len())];
        let line = consumed.iter().filter(|&&b| b == b'\n').count() + 1;
        let start = consumed
            .iter()
            .rposition(|&b| b == b'\n')
            .map_or(0, |i| i + 1);
        ParseError::Syntax {
            line,
            column: self.pos - start + 1,
            reason,
        }
    }

    fn peek(&self) -> Option<u8> {
        self.bytes.get(self.pos).copied()
    }

    fn skip_whitespace(&mut self) {
        while matches!(self.peek(), Some(b' ' | b'\t' | b'\n' | b'\r')) {
            self.pos += 1;
        }
    }

    fn literal(&mut self, text: &[u8], value: Value) -> Result<Value, ParseError> {
        if self.bytes[self.pos..].starts_with(text) {
            self.pos += text.len();
            Ok(value)
        } else {
            Err(self.error("无效的字面量"))
        }
    }

    fn hex4(&mut self) -> Result<u32, ParseError> {
        let mut code = 0;
        for _ in 0..4 {
            let digit = self
                .peek()
                .and_then(|b| (b as char).to_digit(16))
                .ok_or_else(|| self.error("\\u 后必须是四个十六进制数字"))?;
            code = code * 16 + digit;
            self.pos += 1;
        }
        Ok(code)
    }

    /// Positioned after the opening quote.
    fn string(&mut self) -> Result<String, ParseError> {
        let mut out: Vec<u8> = Vec::new();
        loop {
            let run = self.pos;
            while let Some(b) = self.peek() {
                if b == b'"' || b == b'\\' || b < 0x20 {
                    break;
                }
                self.pos += 1;
            }
            let raw = &self.bytes[run..self.pos];
            if std::str::from_utf8(raw).is_err() {
                let valid = std::str::from_utf8(raw)
                    .err()
                    .map_or(0, |e| e.valid_up_to());
                self.pos = run + valid;
                return Err(self.error("字符串不是有效的 UTF-8"));
            }
            out.extend_from_slice(raw);
            match self.peek() {
                None => return Err(self.error("字符串缺少结束引号")),
                Some(b'"') => {
                    self.pos += 1;
                    return Ok(String::from_utf8(out).expect("validated UTF-8"));
                }
                Some(b'\\') => {
                    self.pos += 1;
                    let escaped = self
                        .peek()
                        .ok_or_else(|| self.error("字符串缺少结束引号"))?;
                    self.pos += 1;
                    let c = match escaped {
                        b'"' => '"',
                        b'\\' => '\\',
                        b'/' => '/',
                        b'b' => '\u{8}',
                        b'f' => '\u{c}',
                        b'n' => '\n',
                        b'r' => '\r',
                        b't' => '\t',
                        b'u' => {
                            let high = self.hex4()?;
                            let code = if (0xD800..=0xDBFF).contains(&high) {
                                if !self.bytes[self.pos..].starts_with(b"\\u") {
                                    return Err(self.error("高位代理后必须跟低位代理"));
                                }
                                self.pos += 2;
                                let low = self.hex4()?;
                                if !(0xDC00..=0xDFFF).contains(&low) {
                                    return Err(self.error("高位代理后必须跟低位代理"));
                                }
                                0x10000 + ((high - 0xD800) << 10) + (low - 0xDC00)
                            } else if (0xDC00..=0xDFFF).contains(&high) {
                                return Err(self.error("低位代理之前必须有高位代理"));
                            } else {
                                high
                            };
                            char::from_u32(code).expect("valid scalar value")
                        }
                        _ => {
                            self.pos -= 1;
                            return Err(self.error("无效的转义字符"));
                        }
                    };
                    let mut utf8 = [0; 4];
                    out.extend_from_slice(c.encode_utf8(&mut utf8).as_bytes());
                }
                Some(_) => return Err(self.error("控制字符必须转义")),
            }
        }
    }

    fn digits(&mut self) -> usize {
        let start = self.pos;
        while matches!(self.peek(), Some(b'0'..=b'9')) {
            self.pos += 1;
        }
        self.pos - start
    }

    fn number(&mut self) -> Result<Value, ParseError> {
        let start = self.pos;
        let negative = self.peek() == Some(b'-');
        if negative {
            self.pos += 1;
        }
        match self.peek() {
            Some(b'0') => self.pos += 1,
            Some(b'1'..=b'9') => {
                self.digits();
            }
            _ => return Err(self.error("无效的数字")),
        }
        let mut integer = true;
        if self.peek() == Some(b'.') {
            integer = false;
            self.pos += 1;
            if self.digits() == 0 {
                return Err(self.error("小数点后必须有数字"));
            }
        }
        if matches!(self.peek(), Some(b'e' | b'E')) {
            integer = false;
            self.pos += 1;
            if matches!(self.peek(), Some(b'+' | b'-')) {
                self.pos += 1;
            }
            if self.digits() == 0 {
                return Err(self.error("指数后必须有数字"));
            }
        }
        let token = std::str::from_utf8(&self.bytes[start..self.pos]).expect("ASCII number");
        if integer {
            // strtoull / strtoll; on ERANGE nlohmann falls back to strtod.
            if negative {
                if let Ok(v) = token.parse::<i64>() {
                    return Ok(Value::Integer(v));
                }
            } else if let Ok(v) = token.parse::<u64>() {
                return Ok(Value::Unsigned(v));
            }
        }
        let value: f64 = token.parse().expect("grammar-checked number");
        if !value.is_finite() {
            return Err(ParseError::NumberOverflow(token.to_owned()));
        }
        Ok(Value::Float(value))
    }

    fn parse(mut self) -> Result<Value, ParseError> {
        if self.bytes.first() == Some(&0xEF) {
            if !self.bytes.starts_with(&[0xEF, 0xBB, 0xBF]) {
                return Err(self.error("无效的 BOM"));
            }
            self.pos = 3;
        }
        let mut stack: Vec<Frame> = Vec::new();
        loop {
            // Expect a value.
            self.skip_whitespace();
            let mut value = match self.peek() {
                None => return Err(self.error("意外的输入结尾")),
                Some(b'{') => {
                    self.pos += 1;
                    self.skip_whitespace();
                    if self.peek() == Some(b'}') {
                        self.pos += 1;
                        Value::Object(Map::new())
                    } else {
                        let key = self.object_key()?;
                        stack.push(Frame::Object(Map::new(), key));
                        continue;
                    }
                }
                Some(b'[') => {
                    self.pos += 1;
                    self.skip_whitespace();
                    if self.peek() == Some(b']') {
                        self.pos += 1;
                        Value::Array(Vec::new())
                    } else {
                        stack.push(Frame::Array(Vec::new()));
                        continue;
                    }
                }
                Some(b'"') => {
                    self.pos += 1;
                    Value::String(self.string()?)
                }
                Some(b't') => self.literal(b"true", Value::Bool(true))?,
                Some(b'f') => self.literal(b"false", Value::Bool(false))?,
                Some(b'n') => self.literal(b"null", Value::Null)?,
                Some(b'-' | b'0'..=b'9') => self.number()?,
                Some(_) => return Err(self.error("意外的字符")),
            };
            // Attach the value and close finished containers.
            loop {
                self.skip_whitespace();
                match stack.last_mut() {
                    None => {
                        if self.pos != self.bytes.len() {
                            return Err(self.error("值之后还有多余内容"));
                        }
                        return Ok(value);
                    }
                    Some(Frame::Array(items)) => {
                        items.push(value);
                        match self.peek() {
                            Some(b',') => {
                                self.pos += 1;
                                break;
                            }
                            Some(b']') => {
                                self.pos += 1;
                                let Some(Frame::Array(items)) = stack.pop() else {
                                    unreachable!()
                                };
                                value = Value::Array(items);
                            }
                            _ => return Err(self.error("数组中缺少 ',' 或 ']'")),
                        }
                    }
                    Some(Frame::Object(map, key)) => {
                        map.insert(std::mem::take(key), value);
                        match self.peek() {
                            Some(b',') => {
                                self.pos += 1;
                                self.skip_whitespace();
                                *key = self.object_key()?;
                                break;
                            }
                            Some(b'}') => {
                                self.pos += 1;
                                let Some(Frame::Object(map, _)) = stack.pop() else {
                                    unreachable!()
                                };
                                value = Value::Object(map);
                            }
                            _ => return Err(self.error("对象中缺少 ',' 或 '}'")),
                        }
                    }
                }
            }
        }
    }

    /// A key and its colon, positioned at the opening quote.
    fn object_key(&mut self) -> Result<String, ParseError> {
        if self.peek() != Some(b'"') {
            return Err(self.error("对象键必须是字符串"));
        }
        self.pos += 1;
        let key = self.string()?;
        self.skip_whitespace();
        if self.peek() != Some(b':') {
            return Err(self.error("对象键之后缺少 ':'"));
        }
        self.pos += 1;
        Ok(key)
    }
}

/// `basic_json::parse(text)` with default options.
pub fn parse(text: &[u8]) -> Result<Value, ParseError> {
    Parser {
        bytes: text,
        pos: 0,
    }
    .parse()
}

fn escape_into(out: &mut String, s: &str) {
    for c in s.chars() {
        match c {
            '\u{8}' => out.push_str("\\b"),
            '\t' => out.push_str("\\t"),
            '\n' => out.push_str("\\n"),
            '\u{c}' => out.push_str("\\f"),
            '\r' => out.push_str("\\r"),
            '"' => out.push_str("\\\""),
            '\\' => out.push_str("\\\\"),
            c if u32::from(c) <= 0x1F => out.push_str(&format!("\\u{:04x}", u32::from(c))),
            c => out.push(c),
        }
    }
}

fn write_scalar(out: &mut String, value: &Value) {
    match value {
        Value::Null => out.push_str("null"),
        Value::Bool(true) => out.push_str("true"),
        Value::Bool(false) => out.push_str("false"),
        Value::Unsigned(u) => out.push_str(&u.to_string()),
        Value::Integer(i) => out.push_str(&i.to_string()),
        Value::Float(f) if f.is_finite() => dtoa::write_f64(out, *f),
        Value::Float(_) => out.push_str("null"),
        Value::String(s) => {
            out.push('"');
            escape_into(out, s);
            out.push('"');
        }
        Value::Array(_) | Value::Object(_) => unreachable!("containers are written by dump"),
    }
}

fn write_value(out: &mut String, value: &Value, indent: Option<usize>, level: usize) {
    let newline = |out: &mut String, level: usize| {
        if let Some(step) = indent {
            out.push('\n');
            out.extend(std::iter::repeat_n(' ', step * level));
        }
    };
    match value {
        Value::Array(items) if !items.is_empty() => {
            out.push('[');
            for (i, item) in items.iter().enumerate() {
                if i > 0 {
                    out.push(',');
                }
                newline(out, level + 1);
                write_value(out, item, indent, level + 1);
            }
            newline(out, level);
            out.push(']');
        }
        Value::Array(_) => out.push_str("[]"),
        Value::Object(map) if !map.is_empty() => {
            out.push('{');
            for (i, (key, item)) in map.iter().enumerate() {
                if i > 0 {
                    out.push(',');
                }
                newline(out, level + 1);
                out.push('"');
                escape_into(out, key);
                out.push_str(if indent.is_some() { "\": " } else { "\":" });
                write_value(out, item, indent, level + 1);
            }
            newline(out, level);
            out.push('}');
        }
        Value::Object(_) => out.push_str("{}"),
        scalar => write_scalar(out, scalar),
    }
}

/// `basic_json::dump(indent)`: `None` is the compact form (`dump()`), `Some(n)` pretty-prints
/// with n spaces per level (`dump(n)`).
pub fn dump(value: &Value, indent: Option<usize>) -> String {
    let mut out = String::new();
    write_value(&mut out, value, indent, 0);
    out
}

/// nlohmann's text for one double (`dump()` of a float value).
pub fn format_f64(value: f64) -> String {
    let mut out = String::new();
    write_scalar(&mut out, &Value::Float(value));
    out
}
