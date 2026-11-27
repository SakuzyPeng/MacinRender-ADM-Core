//! Wall treatments in room metres. Polygon membership uses integer micrometres.
use super::geometry::{Image, Position};
use crate::{Error, Result, geometry::Vec3};
use serde_json::Value;

const UV: [[usize; 2]; 3] = [[1, 2], [0, 2], [0, 1]];
pub const NO_MATERIAL: u8 = u8::MAX;
type Point = [i64; 2];

fn invalid() -> Error {
    Error::InvalidArgument("Invalid v2 room surface or treatment polygon")
}
fn grid(x: f64) -> i64 {
    (x * 1_000_000. + 0.5).floor() as i64
}
fn cross(a: Point, b: Point, p: Point) -> i64 {
    (b[0] - a[0]) * (p[1] - a[1]) - (b[1] - a[1]) * (p[0] - a[0])
}
fn intersects(a: Point, b: Point, c: Point, d: Point) -> bool {
    (0..2).all(|i| a[i].min(b[i]).max(c[i].min(d[i])) <= a[i].max(b[i]).min(c[i].max(d[i])))
        && cross(a, b, c).signum() * cross(a, b, d).signum() <= 0
        && cross(c, d, a).signum() * cross(c, d, b).signum() <= 0
}

#[derive(Clone)]
struct Region {
    material: u8,
    points: Vec<Point>,
    lo: Point,
    hi: Point,
}
impl Region {
    fn parse(value: &Value, limits: [f64; 2], material: u8) -> Result<Self> {
        let vs = value
            .as_array()
            .filter(|p| (3..=128).contains(&p.len()))
            .ok_or_else(invalid)?;
        let mut points = Vec::with_capacity(vs.len());
        for v in vs {
            let a = v.as_array().filter(|p| p.len() == 2).ok_or_else(invalid)?;
            let mut point = [0; 2];
            for i in 0..2 {
                let x = a[i]
                    .as_f64()
                    .filter(|x| (0. ..=limits[i]).contains(x))
                    .ok_or_else(invalid)?;
                point[i] = grid(x);
            }
            if points.contains(&point) {
                return Err(invalid());
            }
            points.push(point);
        }
        let n = points.len();
        let mut area = 0;
        for i in 0..n {
            let (a, b, c) = (points[i], points[(i + 1) % n], points[(i + 2) % n]);
            area += a[0] * b[1] - a[1] * b[0];
            if cross(a, b, c) == 0
                && (b[0] - a[0]) * (c[0] - b[0]) + (b[1] - a[1]) * (c[1] - b[1]) < 0
            {
                return Err(invalid());
            }
            for j in i + 2..n {
                if (i, j) != (0, n - 1) && intersects(a, b, points[j], points[(j + 1) % n]) {
                    return Err(invalid());
                }
            }
        }
        if area == 0 {
            return Err(invalid());
        }
        let lo = std::array::from_fn(|i| points.iter().map(|p| p[i]).min().unwrap());
        let hi = std::array::from_fn(|i| points.iter().map(|p| p[i]).max().unwrap());
        Ok(Self {
            material,
            points,
            lo,
            hi,
        })
    }
    fn contains(&self, p: Point) -> bool {
        if (0..2).any(|i| p[i] < self.lo[i] || p[i] > self.hi[i]) {
            return false;
        }
        let mut inside = false;
        let mut a = *self.points.last().unwrap();
        for &b in &self.points {
            let side = cross(a, b, p);
            if side == 0 && (0..2).all(|i| (a[i].min(b[i])..=a[i].max(b[i])).contains(&p[i])) {
                return true;
            }
            if (a[1] > p[1]) != (b[1] > p[1]) && (side > 0) == (b[1] > a[1]) {
                inside = !inside;
            }
            a = b;
        }
        inside
    }
}
#[derive(Clone)]
struct Face {
    base: u8,
    regions: Vec<Region>,
}
#[derive(Clone)]
pub struct SurfaceModel {
    names: Vec<String>,
    reflection: Vec<[f32; 8]>,
    faces: Vec<Face>,
}

#[derive(Clone, Copy, Default)]
pub struct Hit {
    pub t: f64,
    pub wall: usize,
    pub material: u8,
    pub uv: [f64; 2],
}

impl SurfaceModel {
    pub fn parse(model: &Value, dimensions: Vec3) -> Result<Self> {
        let ms = model["materials"]
            .as_array()
            .filter(|m| (1..=128).contains(&m.len()))
            .ok_or_else(invalid)?;
        let mut names = Vec::new();
        let mut reflection = Vec::new();
        for m in ms {
            let name = m["id"]
                .as_str()
                .filter(|n| !n.is_empty() && !names.iter().any(|x| x == n))
                .ok_or_else(invalid)?;
            if !matches!(
                m.get("law").map(Value::as_str),
                None | Some(Some("lambert" | "fan"))
            ) {
                return Err(invalid());
            }
            let band = |k: &str| -> Result<[f64; 8]> {
                let a = m[k]
                    .as_array()
                    .filter(|a| a.len() == 8)
                    .ok_or_else(invalid)?;
                let mut b = [0.; 8];
                for (out, x) in b.iter_mut().zip(a) {
                    *out = x
                        .as_f64()
                        .filter(|v| (0. ..=1.).contains(v))
                        .ok_or_else(invalid)?;
                }
                Ok(b)
            };
            let a = band("absorption")?;
            let s = band("scattering")?;
            reflection.push(std::array::from_fn(|b| {
                ((1. - a[b]) * (1. - s[b])).sqrt() as f32
            }));
            names.push(name.to_owned());
        }
        let binding = |v: &Value| -> Result<u8> {
            if let Some(a) = v.get("angle_deg")
                && !a.as_f64().is_some_and(|x| (-360. ..=360.).contains(&x))
            {
                return Err(invalid());
            }
            let name = v["material"].as_str().ok_or_else(invalid)?;
            names
                .iter()
                .position(|n| n == name)
                .map(|i| i as u8)
                .ok_or_else(invalid)
        };
        let list = model["faces"]
            .as_array()
            .filter(|f| f.len() == 6)
            .ok_or_else(invalid)?;
        let mut faces = Vec::new();
        for (wall, v) in list.iter().enumerate() {
            let mut face = Face {
                base: binding(v)?,
                regions: Vec::new(),
            };
            if let Some(rs) = v.get("regions") {
                for item in rs
                    .as_array()
                    .filter(|r| r.len() <= 64)
                    .ok_or_else(invalid)?
                {
                    face.regions.push(Region::parse(
                        &item["polygon"],
                        UV[wall / 2].map(|i| dimensions[i]),
                        binding(item)?,
                    )?);
                }
            }
            faces.push(face);
        }
        Ok(Self {
            names,
            reflection,
            faces,
        })
    }
    pub fn name(&self, id: u8) -> &str {
        &self.names[id as usize]
    }
    pub fn material_at(&self, wall: usize, uv: [f64; 2]) -> u8 {
        let face = &self.faces[wall];
        let p = uv.map(grid);
        face.regions
            .iter()
            .rev()
            .find(|r| r.contains(p))
            .map_or(face.base, |r| r.material)
    }
    /// Intersect the unfolded listener-to-image segment with lattice planes.
    /// Fold the other two coordinates back into the physical wall's UV system.
    pub fn hits(
        &self,
        dimensions: Vec3,
        listener: Vec3,
        source: Position,
        image: Image,
    ) -> [Hit; 8] {
        let delta: Vec3 = std::array::from_fn(|i| {
            f64::from(image.parity[i]) * source.point[i]
                + 2. * f64::from(image.lattice[i]) * dimensions[i]
                - listener[i]
        });
        let mut hits = [Hit::default(); 8];
        let mut used = 0;
        for axis in 0..3 {
            let crossings = image.walls[axis * 2] + image.walls[axis * 2 + 1];
            for j in 0..crossings {
                let plane = if delta[axis] > 0. {
                    j as i32 + 1
                } else {
                    -(j as i32)
                };
                let t = (f64::from(plane) * dimensions[axis] - listener[axis]) / delta[axis];
                let wall = axis * 2 + plane.rem_euclid(2) as usize;
                let uv = UV[axis].map(|i| {
                    let period = 2. * dimensions[i];
                    let value = (listener[i] + t * delta[i]).rem_euclid(period);
                    value.min(period - value)
                });
                hits[used] = Hit {
                    t,
                    wall,
                    uv,
                    material: self.material_at(wall, uv),
                };
                used += 1;
            }
        }
        hits[..used].sort_unstable_by(|a, b| a.t.total_cmp(&b.t).then(a.wall.cmp(&b.wall)));
        hits
    }
    pub fn key(&self, dimensions: Vec3, listener: Vec3, source: Position, image: Image) -> [u8; 8] {
        let hits = self.hits(dimensions, listener, source, image);
        let mut key = [NO_MATERIAL; 8];
        for (id, h) in key.iter_mut().zip(&hits[..image.order]) {
            *id = h.material;
        }
        key[..image.order].sort_unstable();
        key
    }
    pub fn gains(&self, key: &[u8; 8]) -> [f32; 8] {
        let mut gains = [1.; 8];
        for &id in key.iter().take_while(|&&id| id != NO_MATERIAL) {
            for (g, k) in gains.iter_mut().zip(self.reflection[id as usize]) {
                *g *= k;
            }
        }
        gains
    }
}

#[cfg(test)]
mod tests {
    use super::*;
    use crate::room::geometry::images;
    fn fixture() -> Value {
        serde_json::from_str(include_str!(
            "../../../../../tests/fixtures/room/variant-manifests/zoned-room.json"
        ))
        .unwrap()
    }
    #[test]
    fn shared_concave_boundary_and_overlap_cases() {
        let f = fixture();
        let m = SurfaceModel::parse(&f["surface_model"], [7., 6., 3.2]).unwrap();
        for c in f["cases"].as_array().unwrap() {
            let uv = std::array::from_fn(|i| c[1][i].as_f64().unwrap());
            assert_eq!(
                m.name(m.material_at(c[0].as_u64().unwrap() as usize, uv)),
                c[2].as_str().unwrap()
            );
        }
        let mut invalid = f["surface_model"].clone();
        invalid["faces"][0]["regions"][0]["polygon"] =
            serde_json::json!([[0, 0], [3, 2], [0, 2], [2, 0]]);
        assert!(SurfaceModel::parse(&invalid, [7., 6., 3.2]).is_err());
    }
    #[test]
    fn unfolded_hits_match_the_image_counts_including_coincident_edges() {
        let dims = [4., 4., 4.];
        let l = [2., 2., 2.];
        let m = SurfaceModel::parse(&fixture()["surface_model"], dims).unwrap();
        let s = Position {
            point: [1., 1., 1.],
            direction: [-1., 0., 0.],
            distance: 1.,
            constrained: false,
        };
        for image in images(8) {
            let hs = m.hits(dims, l, s, image);
            let mut counts = [0; 6];
            let mut prev = 0.;
            for h in &hs[..image.order] {
                assert!(h.t >= prev && h.t > 0. && h.t < 1.);
                prev = h.t;
                assert!(h.uv.iter().all(|x| (0. ..=4.).contains(x)));
                counts[h.wall] += 1;
            }
            assert_eq!(counts, image.walls);
            if image.walls == [1, 0, 0, 0, 0, 0] {
                assert!((hs[0].uv[0] - 4. / 3.).abs() < 1e-14);
            }
        }
    }
}
