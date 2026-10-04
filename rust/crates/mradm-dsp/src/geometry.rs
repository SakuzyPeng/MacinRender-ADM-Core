//! Spherical geometry for the project-owned VBAP, HRTF and spreader paths.
//! Incremental convex hull with explicit interior orientation and deterministic
//! tie-breaking; small matrices use f64 cofactors, without a BLAS dependency.
use crate::{Error, Result, rng::Pcg32};
use std::collections::HashMap;

pub type Vec3 = [f64; 3];
pub fn dot(a: Vec3, b: Vec3) -> f64 {
    a[0] * b[0] + a[1] * b[1] + a[2] * b[2]
}
pub fn sub(a: Vec3, b: Vec3) -> Vec3 {
    std::array::from_fn(|i| a[i] - b[i])
}
pub fn add(a: Vec3, b: Vec3) -> Vec3 {
    std::array::from_fn(|i| a[i] + b[i])
}
pub fn scale(a: Vec3, k: f64) -> Vec3 {
    a.map(|x| x * k)
}
pub fn cross(a: Vec3, b: Vec3) -> Vec3 {
    [
        a[1] * b[2] - a[2] * b[1],
        a[2] * b[0] - a[0] * b[2],
        a[0] * b[1] - a[1] * b[0],
    ]
}
pub fn norm(a: Vec3) -> f64 {
    dot(a, a).sqrt()
}
pub fn unit(a: Vec3) -> Vec3 {
    scale(a, 1.0 / norm(a).max(f64::MIN_POSITIVE))
}
pub fn direction(azimuth: f32, elevation: f32) -> Vec3 {
    let (sa, ca) = (azimuth as f64).to_radians().sin_cos();
    let (se, ce) = (elevation as f64).to_radians().sin_cos();
    [ca * ce, sa * ce, se]
}
pub fn directions(input: &[f32]) -> Result<Vec<Vec3>> {
    if !input.len().is_multiple_of(2) || input.len() < 4 || input.iter().any(|v| !v.is_finite()) {
        return Err(Error::InvalidArgument("Invalid azimuth/elevation array"));
    }
    Ok(input
        .as_chunks::<2>()
        .0
        .iter()
        .map(|d| direction(d[0], d[1]))
        .collect())
}

#[derive(Clone)]
struct Face {
    vertices: [usize; 3],
    normal: Vec3,
    offset: f64,
}
impl Face {
    fn new(mut indices: [usize; 3], points: &[Vec3], interior: Vec3) -> Option<Self> {
        let a = points[indices[0]];
        let mut normal = cross(sub(points[indices[1]], a), sub(points[indices[2]], a));
        if norm(normal) < 1e-18 {
            return None;
        }
        normal = unit(normal);
        if dot(normal, sub(interior, a)) > 0.0 {
            indices.swap(1, 2);
            normal = scale(normal, -1.0);
        }
        Some(Self {
            vertices: indices,
            normal,
            offset: -dot(normal, a),
        })
    }
}

/// Returns outward-facing triangles. Original vertex IDs remain unchanged.
pub fn hull(vertices: &[Vec3]) -> Result<Vec<[usize; 3]>> {
    if vertices.len() < 4
        || vertices.len() > 100_000
        || vertices.iter().flatten().any(|x| !x.is_finite())
    {
        return Err(Error::InvalidArgument(
            "A 3D hull needs at least four finite vertices",
        ));
    }
    let mut rng = Pcg32::new(0x4d5241444d, 1);
    let points: Vec<Vec3> = vertices
        .iter()
        .map(|v| v.map(|x| x + (rng.unit() as f64 - 0.5) * 1e-9))
        .collect();
    let count = points.len();
    let a = (0..count)
        .min_by(|&a, &b| points[a][0].total_cmp(&points[b][0]))
        .unwrap();
    let b = (0..count)
        .max_by(|&b, &c| {
            norm(sub(points[b], points[a])).total_cmp(&norm(sub(points[c], points[a])))
        })
        .unwrap();
    let axis = sub(points[b], points[a]);
    let c = (0..count)
        .max_by(|&b, &c| {
            norm(cross(axis, sub(points[b], points[a])))
                .total_cmp(&norm(cross(axis, sub(points[c], points[a]))))
        })
        .unwrap();
    let normal = cross(axis, sub(points[c], points[a]));
    let d = (0..count)
        .max_by(|&b, &c| {
            dot(normal, sub(points[b], points[a]))
                .abs()
                .total_cmp(&dot(normal, sub(points[c], points[a])).abs())
        })
        .unwrap();
    if dot(normal, sub(points[d], points[a])).abs() < 1e-10 {
        return Err(Error::InvalidArgument("Degenerate 3D speaker geometry"));
    }
    let initial = [a, b, c, d];
    let interior = scale(
        add(add(points[a], points[b]), add(points[c], points[d])),
        0.25,
    );
    let mut faces: Vec<Face> = [[a, b, c], [a, d, b], [a, c, d], [b, d, c]]
        .into_iter()
        .map(|f| Face::new(f, &points, interior).unwrap())
        .collect();
    let mut visible = Vec::new();
    let mut horizon = Vec::new();
    let mut edge_counts = HashMap::new();
    for next in 0..count {
        if initial.contains(&next) {
            continue;
        }
        visible.clear();
        for (index, face) in faces.iter().enumerate() {
            if dot(face.normal, points[next]) + face.offset > 1e-13 {
                visible.push(index);
            }
        }
        if visible.is_empty() {
            continue;
        }
        horizon.clear();
        edge_counts.clear();
        for &id in &visible {
            let f = faces[id].vertices;
            for (a, b) in [(f[0], f[1]), (f[1], f[2]), (f[2], f[0])] {
                let key = (a.min(b), a.max(b));
                *edge_counts.entry(key).or_insert(0usize) += 1;
                horizon.push((a, b));
            }
        }
        // Iterate the insertion-ordered edge list, not HashMap iteration order.
        let mut index = 0;
        let mut deleted = 0;
        faces.retain(|_| {
            let keep = visible.get(deleted) != Some(&index);
            if !keep {
                deleted += 1;
            }
            index += 1;
            keep
        });
        for &(a, b) in &horizon {
            if edge_counts[&(a.min(b), a.max(b))] == 1
                && let Some(face) = Face::new([a, b, next], &points, interior)
            {
                faces.push(face);
            }
        }
        if faces.len() > 2 * count + 8 {
            return Err(Error::RenderFailed("Non-manifold convex hull"));
        }
    }
    Ok(faces.into_iter().map(|f| f.vertices).collect())
}

pub fn inverse_columns(a: Vec3, b: Vec3, c: Vec3) -> Option<[Vec3; 3]> {
    let row0 = cross(b, c);
    let determinant = dot(a, row0);
    if determinant.abs() < 1e-12 {
        return None;
    }
    Some([
        scale(row0, 1.0 / determinant),
        scale(cross(c, a), 1.0 / determinant),
        scale(cross(a, b), 1.0 / determinant),
    ])
}

#[derive(Clone)]
pub struct Triplet {
    pub vertices: [usize; 3],
    pub inverse: [Vec3; 3],
}

pub fn triplets(vertices: &[Vec3], omit_large: bool) -> Result<Vec<Triplet>> {
    let faces = hull(vertices)?;
    let mut result = Vec::with_capacity(faces.len());
    for face in faces {
        let [a, b, c] = face.map(|i| vertices[i]);
        let normal = cross(sub(b, a), sub(c, a));
        if dot(normal, add(add(a, b), c)) <= 1e-12 {
            continue;
        }
        if omit_large
            && [dot(a, b), dot(b, c), dot(c, a)]
                .iter()
                .any(|&d| d <= -1.0 + 1e-12)
        {
            continue;
        }
        if let Some(inverse) = inverse_columns(a, b, c) {
            result.push(Triplet {
                vertices: face,
                inverse,
            });
        }
    }
    if result.is_empty() {
        return Err(Error::InvalidArgument(
            "Speaker geometry has no usable triangles",
        ));
    }
    Ok(result)
}

/// Spherical Voronoi cells are dual to hull triangles. Triangulate each ordered
/// dual polygon around its measurement direction, using spherical excess.
pub fn voronoi_weights(vertices: &[Vec3]) -> Result<Vec<f32>> {
    let faces = hull(vertices)?;
    let mut cells = vec![Vec::new(); vertices.len()];
    for face in faces {
        let [a, b, c] = face.map(|i| vertices[i]);
        let mut center = unit(cross(sub(b, a), sub(c, a)));
        if dot(center, add(add(a, b), c)) < 0.0 {
            center = scale(center, -1.0);
        }
        for id in face {
            cells[id].push(center);
        }
    }
    let mut weights = Vec::with_capacity(vertices.len());
    for (id, cell) in cells.iter_mut().enumerate() {
        let center = unit(vertices[id]);
        let reference = if center[2].abs() < 0.9 {
            [0.0, 0.0, 1.0]
        } else {
            [1.0, 0.0, 0.0]
        };
        let x = unit(cross(center, reference));
        let y = cross(center, x);
        cell.sort_by(|&a, &b| {
            dot(a, y)
                .atan2(dot(a, x))
                .total_cmp(&dot(b, y).atan2(dot(b, x)))
        });
        let mut area = 0.0;
        if cell.len() >= 3 {
            for k in 0..cell.len() {
                let a = cell[k];
                let b = cell[(k + 1) % cell.len()];
                area += 2.0
                    * dot(center, cross(a, b))
                        .abs()
                        .atan2(1.0 + dot(center, a) + dot(a, b) + dot(b, center));
            }
        }
        weights.push(area as f32);
    }
    let total: f32 = weights.iter().sum();
    if !total.is_finite() || total <= 0.0 {
        return Err(Error::RenderFailed("Invalid spherical Voronoi cells"));
    }
    for value in &mut weights {
        *value /= total;
    }
    Ok(weights)
}

#[cfg(test)]
mod tests {
    use super::*;
    fn octahedron() -> Vec<Vec3> {
        vec![
            [1., 0., 0.],
            [-1., 0., 0.],
            [0., 1., 0.],
            [0., -1., 0.],
            [0., 0., 1.],
            [0., 0., -1.],
        ]
    }
    #[test]
    fn closed_hull_and_equal_voronoi_cells() {
        let vertices = octahedron();
        let faces = hull(&vertices).unwrap();
        assert_eq!(faces.len(), 8);
        assert_eq!(faces, hull(&vertices).unwrap());
        let weights = voronoi_weights(&vertices).unwrap();
        for value in weights {
            assert!((value - 1.0 / 6.0).abs() < 1e-6);
        }
    }
    #[test]
    fn inverse_reconstructs_direction() {
        let vertices = directions(&[30., 0., -30., 0., 0., 60.]).unwrap();
        let inverse = inverse_columns(vertices[0], vertices[1], vertices[2]).unwrap();
        let query = direction(5., 20.);
        let gains = inverse.map(|row| dot(row, query));
        let output = (0..3).fold([0.; 3], |sum, i| add(sum, scale(vertices[i], gains[i])));
        assert!(norm(sub(output, query)) < 1e-12);
    }
}
