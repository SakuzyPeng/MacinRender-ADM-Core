//! BS.2127-1 §7.3.3 polar-edge screen warping in the room's physical coordinates.
//! Reference: EBU EAR Python 2.0.0 (10149bb0), PolarEdges/PolarScreenScaler.
//! No loudspeaker-layout compensation or radius remapping is introduced.
use crate::{Error, Result};

#[derive(Clone, Copy, Debug, PartialEq)]
#[repr(C)]
pub struct RoomScreenGeometry {
    pub cartesian: u32,
    pub reserved: u32,
    pub aspect_ratio: f64,
    /// Polar: azimuth/elevation/depth. Cartesian: ADM x/right, y/front, z/up.
    pub centre: [f64; 3],
    pub width: f64,
}
impl Default for RoomScreenGeometry {
    fn default() -> Self {
        Self {
            cartesian: 0,
            reserved: 0,
            aspect_ratio: 1.78,
            centre: [0., 0., 1.],
            width: 58.,
        }
    }
}

#[derive(Clone, Copy, Debug, Default, PartialEq)]
#[repr(C)]
pub struct RoomScreens {
    pub enabled: u32,
    pub reserved: u32,
    pub reference: RoomScreenGeometry,
    pub reproduction: RoomScreenGeometry,
}

#[derive(Clone, Copy, Default)]
pub struct Mapping {
    reference: [f64; 4],
    reproduction: [f64; 4],
    enabled: bool,
}

fn invalid() -> Error {
    Error::InvalidArgument("Invalid screen geometry or edge ordering")
}
fn norm(v: [f64; 3]) -> f64 {
    libm::hypot(libm::hypot(v[0], v[1]), v[2])
}
fn angles(v: [f64; 3]) -> [f64; 2] {
    [
        libm::atan2(-v[0], v[1]).to_degrees(),
        libm::atan2(v[2], libm::hypot(v[0], v[1])).to_degrees(),
    ]
}

fn edges(screen: RoomScreenGeometry) -> Result<[f64; 4]> {
    if screen.cartesian > 1
        || screen.reserved != 0
        || !screen.aspect_ratio.is_finite()
        || screen.aspect_ratio <= 0.
        || !screen.width.is_finite()
        || screen.width <= 0.
        || screen.centre.iter().any(|v| !v.is_finite())
    {
        return Err(invalid());
    }
    let (centre, horizontal, vertical) = if screen.cartesian == 0 {
        let [az, el, distance] = screen.centre;
        if !(-180. ..=180.).contains(&az)
            || !(-90. ..90.).contains(&el)
            || distance <= 0.
            || screen.width >= 180.
        {
            return Err(invalid());
        }
        let (sa, ca) = libm::sincos(az.to_radians());
        let (se, ce) = libm::sincos(el.to_radians());
        let width = libm::tan((screen.width / 2.).to_radians());
        let height = width / screen.aspect_ratio;
        (
            [-sa * ce, ca * ce, se],
            [ca * width, sa * width, 0.],
            [sa * se * height, -ca * se * height, ce * height],
        )
    } else {
        let length = norm(screen.centre);
        if !length.is_finite() || length <= 1e-15 {
            return Err(invalid());
        }
        let width = screen.width / length / 2.;
        (
            screen.centre.map(|v| v / length),
            [width, 0., 0.],
            [0., 0., width / screen.aspect_ratio],
        )
    };
    let points = [
        std::array::from_fn(|i| centre[i] - horizontal[i]),
        std::array::from_fn(|i| centre[i] + horizontal[i]),
        std::array::from_fn(|i| centre[i] - vertical[i]),
        std::array::from_fn(|i| centre[i] + vertical[i]),
    ];
    if points
        .iter()
        .any(|v| v.iter().any(|x| !x.is_finite()) || norm(*v) <= 1e-15)
    {
        return Err(invalid());
    }
    let result = [
        angles(points[1])[0],
        angles(points[0])[0],
        angles(points[2])[1],
        angles(points[3])[1],
    ];
    if !(result[0] > -180.
        && result[0] < result[1]
        && result[1] < 180.
        && result[2] > -90.
        && result[2] < result[3]
        && result[3] < 90.)
    {
        return Err(invalid());
    }
    Ok(result)
}

fn warp(value: f64, reference: [f64; 4], reproduction: [f64; 4]) -> f64 {
    let segment = if value <= reference[1] {
        0
    } else if value <= reference[2] {
        1
    } else {
        2
    };
    let alpha = (value - reference[segment]) / (reference[segment + 1] - reference[segment]);
    reproduction[segment] + (reproduction[segment + 1] - reproduction[segment]) * alpha
}

impl Mapping {
    pub fn new(screens: RoomScreens) -> Result<Self> {
        if screens.enabled > 1 || screens.reserved != 0 {
            return Err(invalid());
        }
        if screens.enabled == 0 {
            return Ok(Self::default());
        }
        let reference = edges(screens.reference)?;
        let reproduction = edges(screens.reproduction)?;
        Ok(Self {
            reference,
            reproduction,
            enabled: reference != reproduction,
        })
    }
    pub fn position(&self, position: [f32; 3]) -> [f32; 3] {
        if !self.enabled {
            return position;
        }
        let vector = position.map(f64::from);
        let radius = norm(vector);
        if radius <= 1e-15 {
            return position;
        }
        let [az, el] = angles(vector);
        let az = warp(
            az,
            [-180., self.reference[0], self.reference[1], 180.],
            [-180., self.reproduction[0], self.reproduction[1], 180.],
        );
        let el = warp(
            el,
            [-90., self.reference[2], self.reference[3], 90.],
            [-90., self.reproduction[2], self.reproduction[3], 90.],
        );
        let (sa, ca) = libm::sincos(az.to_radians());
        let (se, ce) = libm::sincos(el.to_radians());
        // Huge finite Scene positions already lie beyond every valid room wall.
        // Keep their direction representable until the subsequent room constraint.
        let radius = radius.min(f64::from(f32::MAX));
        [
            (-sa * ce * radius) as f32,
            (ca * ce * radius) as f32,
            (se * radius) as f32,
        ]
    }
}

#[cfg(test)]
mod tests {
    use super::*;
    #[test]
    fn matches_pinned_ear_python_screen_vectors() {
        let data: serde_json::Value = serde_json::from_str(include_str!(
            "../../../../../tests/fixtures/room_screen_ear_2_0_0.json"
        ))
        .unwrap();
        let geometry = |value: &serde_json::Value| RoomScreenGeometry {
            cartesian: u32::from(value["cartesian"].as_bool().unwrap()),
            reserved: 0,
            aspect_ratio: value["aspect_ratio"].as_f64().unwrap(),
            centre: std::array::from_fn(|i| value["centre"][i].as_f64().unwrap()),
            width: value["width"].as_f64().unwrap(),
        };
        for case in data["cases"].as_array().unwrap() {
            let mapping = Mapping::new(RoomScreens {
                enabled: 1,
                reserved: 0,
                reference: geometry(&case["reference"]),
                reproduction: geometry(&case["reproduction"]),
            })
            .unwrap();
            let point = std::array::from_fn(|i| case["position"][i].as_f64().unwrap() as f32);
            let actual = mapping.position(point);
            for (axis, value) in actual.into_iter().enumerate() {
                assert!(
                    (f64::from(value) - case["expected"][axis].as_f64().unwrap()).abs()
                        <= data["absolute_tolerance"].as_f64().unwrap(),
                    "EAR vector {case}, actual {actual:?}"
                );
            }
        }
    }
    #[test]
    fn edges_outside_screen_and_distance_are_preserved() {
        let screens = RoomScreens {
            enabled: 1,
            reproduction: RoomScreenGeometry {
                width: 90.,
                ..RoomScreenGeometry::default()
            },
            ..RoomScreens::default()
        };
        let mapping = Mapping::new(screens).unwrap();
        for (az, expected) in [
            (0f64, 0f64),
            (29., 45.),
            (-29., -45.),
            (180., 180.),
            (104.5, 112.5),
        ] {
            let position = [
                (-libm::sin(az.to_radians()) * 2.) as f32,
                (libm::cos(az.to_radians()) * 2.) as f32,
                0.,
            ];
            let value = mapping.position(position).map(f64::from);
            assert!((angles(value)[0] - expected).abs() < 1e-5);
            assert!((norm(value) - 2.).abs() < 1e-6);
        }
    }
    #[test]
    fn cartesian_edges_defaults_and_invalid_geometry() {
        let screen = RoomScreenGeometry {
            cartesian: 1,
            centre: [0., 1., 0.],
            width: 2.,
            aspect_ratio: 1.,
            reserved: 0,
        };
        let edges = edges(screen).unwrap();
        assert_eq!(edges, [-45., 45., -45., 45.]);
        let position = [0.25, 1., -0.5];
        assert_eq!(
            Mapping::new(RoomScreens::default())
                .unwrap()
                .position(position),
            position
        );
        assert_eq!(
            Mapping::new(RoomScreens {
                enabled: 1,
                ..RoomScreens::default()
            })
            .unwrap()
            .position(position),
            position
        );
        assert!(
            Mapping::new(RoomScreens {
                enabled: 1,
                reproduction: RoomScreenGeometry {
                    width: 0.,
                    ..screen
                },
                ..RoomScreens::default()
            })
            .is_err()
        );
        assert!(
            Mapping::new(RoomScreens {
                enabled: 1,
                reproduction: RoomScreenGeometry {
                    centre: [0.; 3],
                    ..screen
                },
                ..RoomScreens::default()
            })
            .is_err()
        );
    }
}
