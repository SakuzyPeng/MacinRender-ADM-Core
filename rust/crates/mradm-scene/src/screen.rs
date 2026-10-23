//! Screen configuration JSON, `mradm.screen-config` version 1 (PCM consistency 4.4). Moved from
//! the C++ `screen.cpp`: the same 1 MiB limit, lookups in the same order and nlohmann's exception
//! texts after `无效的屏幕配置: `; only the text of JSON syntax errors is the project's own.
//! Unknown keys are accepted. Geometry validation (`room::screen::Mapping`) stays with the caller.
use crate::error::{Context, Error, Result};
use mradm_dsp::room::screen::RoomScreenGeometry;
use mradm_json::{Value, at, type_must_be};

pub const SCHEMA: &str = "mradm.screen-config";
pub const MAX_BYTES: usize = 1024 * 1024;

#[derive(Clone, Copy, Debug, Default, PartialEq)]
pub struct Screens {
    pub reference: Option<RoomScreenGeometry>,
    pub reproduction: Option<RoomScreenGeometry>,
}

fn invalid(message: impl AsRef<str>) -> Error {
    Error::invalid(
        format!("无效的屏幕配置: {}", message.as_ref()),
        Context::None,
    )
}

fn number(value: &Value) -> Result<f64> {
    value
        .as_f64()
        .ok_or_else(|| invalid(type_must_be("number", value)))
}

fn field<'a>(value: &'a Value, key: &str) -> Result<&'a Value> {
    at(value, key).map_err(invalid)
}

fn geometry(value: &Value) -> Result<RoomScreenGeometry> {
    let coordinates = field(value, "coordinates")?;
    let cartesian = match coordinates.as_str() {
        Some("polar") => false,
        Some("cartesian") => true,
        Some(_) => return Err(invalid("屏幕 coordinates 必须是 polar 或 cartesian")),
        None => return Err(invalid(type_must_be("string", coordinates))),
    };
    let centre = match field(value, "centre")?.as_array() {
        Some([x, y, z]) => [number(x)?, number(y)?, number(z)?],
        _ => return Err(invalid("屏幕 centre 必须有三个坐标")),
    };
    let width = number(field(value, "width")?)?;
    let aspect_ratio = number(field(value, "aspect_ratio")?)?;
    Ok(RoomScreenGeometry {
        cartesian: u32::from(cartesian),
        reserved: 0,
        aspect_ratio,
        centre,
        width,
    })
}

pub fn parse(text: &[u8]) -> Result<Screens> {
    if text.len() > MAX_BYTES {
        return Err(Error::invalid("屏幕配置超过 1 MiB", Context::None));
    }
    let doc = mradm_json::parse(text).map_err(|e| invalid(e.to_string()))?;
    let supported = field(&doc, "schema")?.as_str() == Some(SCHEMA)
        && field(&doc, "version")?.number_eq(&Value::Integer(1));
    if !supported {
        return Err(Error::unsupported("不支持的屏幕配置版本", Context::None));
    }
    let optional = |key: &str| match doc.as_object().and_then(|map| map.get(key)) {
        None | Some(Value::Null) => Ok(None),
        Some(value) => geometry(value).map(Some),
    };
    Ok(Screens {
        reference: optional("reference")?,
        reproduction: optional("reproduction")?,
    })
}
