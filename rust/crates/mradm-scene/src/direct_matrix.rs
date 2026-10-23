//! DirectSpeakers label-routing matrix JSON, `mradm.direct-speakers-matrix.v1` (PCM consistency
//! 4.4). Moved from the C++ `direct_speakers_matrix.cpp` with the same checks, check order and
//! messages; only the text of JSON syntax errors is the project's own. Every error is
//! `invalid_argument` with the caller's source label as context.
use crate::error::{Context, Error, Result};
use mradm_json::{Map, Value};

pub const SCHEMA: &str = "mradm.direct-speakers-matrix.v1";

#[derive(Clone, Debug, PartialEq)]
pub struct Target {
    pub label: String,
    /// The authored positive ratio.
    pub weight: f32,
    /// `sqrt(weight / sum(weights))` of the route, the equal-power coefficient.
    pub gain: f32,
}

/// A source-label row: one or more targets, or muted.
#[derive(Clone, Debug, PartialEq)]
pub struct Route {
    pub source_label: String,
    pub targets: Vec<Target>,
    pub mute: bool,
}

#[derive(Clone, Debug, PartialEq)]
pub struct Matrix {
    pub output_layout: String,
    pub routes: Vec<Route>,
}

fn invalid(message: impl AsRef<str>) -> Error {
    Error::invalid(
        format!("invalid DirectSpeakers matrix: {}", message.as_ref()),
        Context::Label,
    )
}

/// The first key outside `allowed`, in key byte order as `std::map` iterated.
fn unknown_key<'a>(map: &'a Map, allowed: &[&str]) -> Option<&'a str> {
    map.keys()
        .map(String::as_str)
        .find(|key| !allowed.contains(key))
}

fn required_string(map: &Map, key: &str, path: &str) -> Result<String> {
    let value = map
        .get(key)
        .ok_or_else(|| invalid(format!("{path} is missing required field '{key}'")))?;
    let text = value
        .as_str()
        .ok_or_else(|| invalid(format!("{path}.{key} must be a string")))?;
    if text.is_empty() {
        return Err(invalid(format!("{path}.{key} must not be empty")));
    }
    Ok(text.to_owned())
}

fn parse_target(value: &Value, route: usize, index: usize) -> Result<Target> {
    let path = format!("routes[{route}].targets[{index}]");
    let map = value
        .as_object()
        .ok_or_else(|| invalid(format!("{path} must be an object")))?;
    if let Some(key) = unknown_key(map, &["label", "weight"]) {
        return Err(invalid(format!("{path} has unknown field '{key}'")));
    }
    let label = required_string(map, "label", &path)?;
    let weight = map
        .get("weight")
        .ok_or_else(|| invalid(format!("{path} is missing required field 'weight'")))?
        .as_f32()
        .ok_or_else(|| invalid(format!("{path}.weight must be numeric")))?;
    if !weight.is_finite() || weight <= 0. {
        return Err(invalid(format!("{path}.weight must be finite and > 0")));
    }
    Ok(Target {
        label,
        weight,
        gain: 0.,
    })
}

fn parse_route(value: &Value, index: usize) -> Result<Route> {
    let path = format!("routes[{index}]");
    let map = value
        .as_object()
        .ok_or_else(|| invalid(format!("{path} must be an object")))?;
    if let Some(key) = unknown_key(map, &["source_label", "targets", "mute"]) {
        return Err(invalid(format!("{path} has unknown field '{key}'")));
    }
    let source_label = required_string(map, "source_label", &path)?;
    let (targets, mute) = (map.get("targets"), map.get("mute"));
    if targets.is_some() == mute.is_some() {
        return Err(invalid(format!(
            "{path} must define exactly one of 'targets' or 'mute'"
        )));
    }
    if let Some(mute) = mute {
        if mute.as_bool() != Some(true) {
            return Err(invalid(format!("{path}.mute must be true")));
        }
        return Ok(Route {
            source_label,
            targets: Vec::new(),
            mute: true,
        });
    }
    let items = targets
        .and_then(Value::as_array)
        .filter(|items| !items.is_empty())
        .ok_or_else(|| invalid(format!("{path}.targets must be a non-empty array")))?;
    let mut targets = Vec::with_capacity(items.len());
    let mut sum = 0.0_f64;
    for (target, item) in items.iter().enumerate() {
        let target = parse_target(item, index, target)?;
        sum += f64::from(target.weight);
        if !sum.is_finite() {
            return Err(invalid(format!("{path}.targets weight sum must be finite")));
        }
        targets.push(target);
    }
    for target in &mut targets {
        target.gain = (f64::from(target.weight) / sum).sqrt() as f32;
    }
    Ok(Route {
        source_label,
        targets,
        mute: false,
    })
}

pub fn parse(text: &[u8]) -> Result<Matrix> {
    let doc = mradm_json::parse(text).map_err(|e| invalid(e.to_string()))?;
    let map = doc
        .as_object()
        .ok_or_else(|| invalid("top-level value must be an object"))?;
    if let Some(key) = unknown_key(map, &["schema", "output_layout", "routes"]) {
        return Err(invalid(format!("unknown top-level field '{key}'")));
    }
    let schema = required_string(map, "schema", "top-level value")?;
    if schema != SCHEMA {
        return Err(invalid(format!("unsupported schema '{schema}'")));
    }
    let output_layout = required_string(map, "output_layout", "top-level value")?;
    let routes = map
        .get("routes")
        .ok_or_else(|| invalid("top-level value is missing required field 'routes'"))?
        .as_array()
        .filter(|items| !items.is_empty())
        .ok_or_else(|| invalid("top-level routes must be a non-empty array"))?;
    Ok(Matrix {
        output_layout,
        routes: routes
            .iter()
            .enumerate()
            .map(|(index, route)| parse_route(route, index))
            .collect::<Result<_>>()?,
    })
}
