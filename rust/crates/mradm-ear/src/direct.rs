// EBU libear DirectSpeakers routing (Apache-2.0); project overrides precede this layer.
use crate::{Error, Layout, Result, data, geom::*, panner::Panner};
#[derive(Clone, Debug)]
pub struct DirectSpeakers {
    pub labels: Vec<String>,
    pub pack: Option<String>,
    pub position: [f64; 3],
    /// Azimuth min/max, elevation min/max, distance min/max.
    pub bounds: [Option<f64>; 6],
    pub low_pass: Option<f64>,
    pub high_pass: Option<f64>,
}
impl Default for DirectSpeakers {
    fn default() -> Self {
        Self {
            labels: vec![],
            pack: None,
            position: [0.0, 0.0, 1.0],
            bounds: [None; 6],
            low_pass: None,
            high_pass: None,
        }
    }
}
fn label(s: &str) -> &str {
    match s {
        "LFE" | "LFEL" => "LFE1",
        "LFER" => "LFE2",
        _ => s
            .strip_prefix("urn:itu:bs:2051:")
            .and_then(|v| v.split_once(":speaker:"))
            .filter(|(version, _)| {
                !version.is_empty() && version.bytes().all(|b| b.is_ascii_digit())
            })
            .map(|(_, name)| name)
            .unwrap_or(s),
    }
}
pub fn calculate(layout: &Layout, panner: &Panner, m: &DirectSpeakers) -> Result<Vec<f64>> {
    if m.position
        .iter()
        .chain(m.bounds.iter().flatten())
        .chain(m.low_pass.iter())
        .chain(m.high_pass.iter())
        .any(|v| !v.is_finite())
        || (m.pack.is_some() && m.labels.is_empty())
    {
        return Err(Error::InvalidArgument(
            "Invalid EAR DirectSpeakers metadata",
        ));
    }
    let labels: Vec<_> = m.labels.iter().map(|s| label(s)).collect();
    let lfe = (m.low_pass.is_some_and(|v| v <= 200.0) && m.high_pass.is_none())
        || labels.iter().any(|s| *s == "LFE1" || *s == "LFE2");
    let mut gains = vec![0.0; layout.channels.len()];
    if let Some(pack) = &m.pack
        && let Some((_, name)) = data::PACKS.iter().find(|(p, _)| p == pack)
    {
        for rule in data::RULES {
            if rule.label == labels[0]
                && (rule.inputs.is_empty() || rule.inputs.contains(name))
                && (rule.outputs.is_empty() || rule.outputs.contains(&layout.name.as_str()))
                && rule
                    .gains
                    .iter()
                    .all(|(n, _)| layout.channels.iter().any(|c| c.name == *n))
            {
                for (name, gain) in rule.gains {
                    gains[layout
                        .channels
                        .iter()
                        .position(|c| c.name == *name)
                        .unwrap()] = *gain;
                }
                return Ok(gains);
            }
        }
    }
    for name in labels {
        if let Some(i) = layout
            .channels
            .iter()
            .position(|c| c.name == name && c.lfe == lfe)
        {
            gains[i] = 1.0;
            return Ok(gains);
        }
    }
    let bounds = std::array::from_fn::<_, 6, _>(|i| m.bounds[i].unwrap_or(m.position[i / 2]));
    let position = cart(m.position);
    let tol = 1e-5;
    let mut candidates: Vec<_> = layout
        .channels
        .iter()
        .enumerate()
        .filter(|(_, c)| {
            let p = c.nominal;
            c.lfe == lfe
                && (inside_angle(p[0], bounds[0], bounds[1], tol) || p[1].abs() >= 90.0 - tol)
                && p[1] > bounds[2] - tol
                && p[1] < bounds[3] + tol
                && p[2] > bounds[4] - tol
                && p[2] < bounds[5] + tol
        })
        .map(|(i, c)| (i, (cart(c.real) - position).norm()))
        .collect();
    candidates.sort_by(|a, b| a.1.total_cmp(&b.1));
    if !candidates.is_empty()
        && (candidates.len() == 1 || (candidates[0].1 - candidates[1].1).abs() > tol)
    {
        gains[candidates[0].0] = 1.0;
    } else if lfe {
        if let Some(i) = layout.channels.iter().position(|c| c.name == "LFE1") {
            gains[i] = 1.0;
        }
    } else {
        let mut values = panner.gains(position)?.into_iter();
        for (g, c) in gains.iter_mut().zip(&layout.channels) {
            if !c.lfe {
                *g = values.next().unwrap();
            }
        }
    }
    Ok(gains)
}
