// EBU libear common/point_source_panner.cpp and convex_hull.cpp, Apache-2.0.
use crate::{Channel, Error, Layout, Result, data, geom::*};
use nalgebra::Matrix3;
use std::collections::BTreeSet;

enum Region {
    Triplet {
        ids: Vec<usize>,
        inverse: Matrix3<f64>,
    },
    Quad {
        ids: Vec<usize>,
        points: Vec<Vec3>,
        order: Vec<usize>,
        x: Matrix3<f64>,
        y: Matrix3<f64>,
    },
    Ngon {
        ids: Vec<usize>,
        children: Vec<Region>,
    },
}
impl Region {
    fn triplet(ids: Vec<usize>, points: &[Vec3]) -> Result<Self> {
        let inverse = Matrix3::from_columns(points)
            .try_inverse()
            .ok_or(Error::RenderFailed("Degenerate EAR triplet"))?;
        Ok(Self::Triplet { ids, inverse })
    }
    fn handle(&self, pos: Vec3, count: usize) -> Option<Vec<f64>> {
        let (ids, mut values) = match self {
            Self::Triplet { ids, inverse } => {
                let raw = inverse * pos;
                if raw.iter().any(|v| *v < -1e-11) || raw.norm() == 0.0 {
                    return None;
                }
                (
                    ids,
                    (raw / raw.norm())
                        .iter()
                        .map(|v| v.clamp(0.0, 1.0))
                        .collect::<Vec<_>>(),
                )
            }
            Self::Quad {
                ids,
                points,
                order,
                x,
                y,
            } => {
                let u = pan(*x * pos)?;
                let v = pan(*y * pos)?;
                let mut g = vec![0.0; 4];
                for (i, value) in [(1.0 - u) * (1.0 - v), u * (1.0 - v), u * v, (1.0 - u) * v]
                    .into_iter()
                    .enumerate()
                {
                    g[order[i]] = value;
                }
                if points
                    .iter()
                    .zip(&g)
                    .map(|(p, g)| p * (*g))
                    .sum::<Vec3>()
                    .dot(&pos)
                    <= 0.0
                {
                    return None;
                }
                normalize(&mut g);
                (ids, g)
            }
            Self::Ngon { ids, children } => {
                let n = ids.len();
                let mut g = children.iter().find_map(|c| c.handle(pos, n + 1))?;
                let centre = g.pop().unwrap() / (n as f64).sqrt();
                for v in &mut g {
                    *v += centre;
                }
                normalize(&mut g);
                (ids, g)
            }
        };
        let mut output = vec![0.0; count];
        for (i, value) in ids.iter().zip(values.drain(..)) {
            output[*i] = value;
        }
        Some(output)
    }
}
fn pan(poly: Vec3) -> Option<f64> {
    let (a, b, c) = (poly[0], poly[1], poly[2]);
    let roots = if c.abs() < 1e-10 {
        vec![0.0]
    } else if a.abs() < 1e-10 {
        vec![-c / b]
    } else {
        let d = b * b - 4.0 * a * c;
        if d > 1e-10 {
            vec![(-b + d.sqrt()) / (2.0 * a), (-b - d.sqrt()) / (2.0 * a)]
        } else if d > -1e-10 {
            vec![-b / (2.0 * a)]
        } else {
            vec![]
        }
    };
    roots
        .into_iter()
        .find(|r| -1e-10 < *r && *r < 1.0 + 1e-10)
        .map(|r| r.clamp(0.0, 1.0))
}
fn poly_basis(p: &[Vec3]) -> Matrix3<f64> {
    let (a, b, c, d) = (p[0], p[1], p[2], p[3]);
    Matrix3::from_rows(&[
        (b - a).cross(&(c - d)).transpose(),
        (a.cross(&(c - d)) + (b - a).cross(&d)).transpose(),
        a.cross(&d).transpose(),
    ])
}
fn hull(points: &[Vec3]) -> Result<Vec<Vec<usize>>> {
    let inside = points.iter().copied().sum::<Vec3>() / points.len() as f64;
    let tol = 1e-5;
    let mut facets: Vec<BTreeSet<usize>> = Vec::new();
    let mut normals: Vec<Vec3> = Vec::new();
    for i in 0..points.len() {
        for j in i + 1..points.len() {
            for k in j + 1..points.len() {
                let normal = (points[j] - points[i]).cross(&(points[k] - points[i]));
                if normal.norm_squared() <= tol {
                    return Err(Error::InvalidArgument("Collinear EAR hull vertices"));
                }
                let normal = normal.normalize();
                let di = normal.dot(&(inside - points[i]));
                if di.abs() < tol
                    || points.iter().any(|p| {
                        let d = normal.dot(&(p - points[i]));
                        if di > 0.0 { d <= -tol } else { d >= tol }
                    })
                {
                    continue;
                }
                if let Some(idx) = facets.iter().enumerate().position(|(f, ids)| {
                    (points[i] - points[*ids.first().unwrap()])
                        .dot(&normals[f])
                        .abs()
                        < tol
                        && normals[f].cross(&normal).norm_squared() < tol
                }) {
                    facets[idx].extend([i, j, k]);
                } else {
                    facets.push(BTreeSet::from([i, j, k]));
                    normals.push(normal);
                }
            }
        }
    }
    Ok(facets
        .into_iter()
        .map(|f| f.into_iter().collect())
        .collect())
}

pub struct Panner {
    regions: Vec<Region>,
    remap: Vec<usize>,
    count: usize,
    stereo: Option<Box<Panner>>,
    stereo_ids: [usize; 2],
}
impl Panner {
    pub fn count(&self) -> usize {
        self.count
    }
    pub fn new(layout: &Layout) -> Result<Self> {
        let mut channels: Vec<Channel> =
            layout.channels.iter().filter(|c| !c.lfe).cloned().collect();
        let count = channels.len();
        if count < 2 {
            return Err(Error::Unsupported(
                "EAR needs at least two non-LFE speakers",
            ));
        }
        for c in &channels {
            if (c.name == "M+SC" || c.name == "M-SC") && !(5.0..25.0).contains(&c.real[0].abs()) {
                return Err(Error::Unsupported(
                    "Unsupported EAR screen speaker position",
                ));
            }
        }
        if layout.name == "0+2+0" {
            let find = |label| {
                channels
                    .iter()
                    .position(|c| c.name == label)
                    .ok_or(Error::InvalidArgument("Missing stereo speaker"))
            };
            return Ok(Self {
                regions: vec![],
                remap: vec![],
                count,
                stereo: Some(Box::new(Self::new(&data::layout("0+5+0")?)?)),
                stereo_ids: [find("M+030")?, find("M-030")?],
            });
        }
        let original = channels.clone();
        let mut remap: Vec<_> = (0..count).collect();
        for (nominal, low, high) in [(-30.0, -70.0, -10.0), (30.0, 10.0, 70.0)] {
            let layer: Vec<_> = original
                .iter()
                .filter(|c| (low..=high).contains(&c.nominal[1]))
                .collect();
            let (limit, real) = if layer.is_empty() {
                (0.0, nominal)
            } else {
                (
                    layer
                        .iter()
                        .map(|c| c.nominal[0].abs())
                        .fold(f64::MIN_POSITIVE, f64::max)
                        + 40.0,
                    layer.iter().map(|c| c.real[1]).sum::<f64>() / layer.len() as f64,
                )
            };
            for (i, c) in original
                .iter()
                .enumerate()
                .filter(|(_, c)| (-10.0..=10.0).contains(&c.nominal[1]))
            {
                if c.real[0].abs() >= limit - 1e-5 {
                    let mut extra = c.clone();
                    extra.name = "extra".into();
                    extra.real = [c.real[0], real, 1.0];
                    extra.nominal = [c.nominal[0], nominal, 1.0];
                    channels.push(extra);
                    remap.push(i);
                }
            }
        }
        let mut real: Vec<_> = channels
            .iter()
            .map(|c| cart([c.real[0], c.real[1], 1.0]))
            .collect();
        let mut nominal: Vec<_> = channels
            .iter()
            .map(|c| cart([c.nominal[0], c.nominal[1], 1.0]))
            .collect();
        let mut virtual_ids = vec![real.len()];
        real.push(Vec3::new(0.0, 0.0, -1.0));
        nominal.push(Vec3::new(0.0, 0.0, -1.0));
        if !original
            .iter()
            .any(|c| c.name == "T+000" || c.name == "UH+180")
        {
            virtual_ids.push(real.len());
            real.push(Vec3::new(0.0, 0.0, 1.0));
            nominal.push(Vec3::new(0.0, 0.0, 1.0));
        }
        let facets = match data::facets(&layout.name) {
            Some(f) => f,
            None => hull(&nominal)?,
        };
        if facets.iter().flatten().any(|i| *i >= real.len()) {
            return Err(Error::InvalidArgument("EAR layout topology mismatch"));
        }
        let mut regions = Vec::new();
        for v in &virtual_ids {
            let mut adjacent = BTreeSet::new();
            for f in facets.iter().filter(|f| f.contains(v)) {
                adjacent.extend(f);
            }
            adjacent.remove(v);
            let ids: Vec<usize> = adjacent.into_iter().copied().collect();
            if ids.len() < 3 || ids.iter().any(|i| virtual_ids.contains(i)) {
                return Err(Error::RenderFailed("Invalid EAR virtual polygon"));
            }
            let points: Vec<_> = ids.iter().map(|i| real[*i]).collect();
            let order = vertex_order(&points);
            let mut children = Vec::new();
            for i in 0..ids.len() {
                let a = order[i];
                let b = order[(i + 1) % ids.len()];
                children.push(Region::triplet(
                    vec![a, b, ids.len()],
                    &[points[a], points[b], real[*v]],
                )?);
            }
            regions.push(Region::Ngon { ids, children });
        }
        for ids in facets
            .into_iter()
            .filter(|f| !f.iter().any(|i| virtual_ids.contains(i)))
        {
            let points: Vec<_> = ids.iter().map(|i| real[*i]).collect();
            match ids.len() {
                3 => regions.push(Region::triplet(ids, &points)?),
                4 => {
                    let order = vertex_order(&points);
                    let p: Vec<_> = order.iter().map(|i| points[*i]).collect();
                    let x = poly_basis(&p);
                    let y = poly_basis(&[p[1], p[2], p[3], p[0]]);
                    regions.push(Region::Quad {
                        ids,
                        points,
                        order,
                        x,
                        y,
                    });
                }
                _ => return Err(Error::Unsupported("EAR facet has more than four vertices")),
            }
        }
        Ok(Self {
            regions,
            remap,
            count,
            stereo: None,
            stereo_ids: [0, 1],
        })
    }
    pub fn gains(&self, pos: Vec3) -> Result<Vec<f64>> {
        if let Some(panner) = &self.stereo {
            let p = panner.gains(pos)?;
            let mut g = vec![
                p[0] + p[2] * 3.0f64.sqrt() / 3.0 + p[3] * 0.5f64.sqrt(),
                p[1] + p[2] * 3.0f64.sqrt() / 3.0 + p[4] * 0.5f64.sqrt(),
            ];
            normalize(&mut g);
            let front = p[..3].iter().copied().fold(0.0, f64::max);
            let back = p[3..].iter().copied().fold(0.0, f64::max);
            let scale = 0.5f64.powf(0.5 * back / (front + back));
            let mut out = vec![0.0; self.count];
            for i in 0..2 {
                out[self.stereo_ids[i]] = g[i] * scale;
            }
            return Ok(out);
        }
        let raw = self
            .regions
            .iter()
            .find_map(|r| r.handle(pos, self.remap.len()))
            .ok_or(Error::RenderFailed(
                "EAR position is outside panning regions",
            ))?;
        let mut out = vec![0.0; self.count];
        for (i, g) in raw.iter().enumerate() {
            out[self.remap[i]] += g;
        }
        normalize(&mut out);
        Ok(out)
    }
}
