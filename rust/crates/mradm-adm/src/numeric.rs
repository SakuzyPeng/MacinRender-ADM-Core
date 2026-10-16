//! Numeric projections of ADM values into the scene (PCM consistency 3.5, audit N02). Elementary
//! functions come from the libm port, so imported gains and positions do not depend on the
//! platform C library.

/// Linear gain of an ADM `gain` element with `gainUnit="dB"`, rounded once to binary32.
pub fn gain_from_db(decibels: f64) -> f32 {
    libm::pow(10.0, decibels / 20.0) as f32
}

/// Azimuth (degrees, positive to the left), elevation (degrees) and distance of a Cartesian
/// DirectSpeakers position in ADM coordinates (x right, y front, z up).
pub fn cartesian_to_polar(x: f32, y: f32, z: f32) -> [f32; 3] {
    let (x, y, z) = (f64::from(x), f64::from(y), f64::from(z));
    let degrees = 180.0 / std::f64::consts::PI;
    [
        (libm::atan2(-x, y) * degrees) as f32,
        (libm::atan2(z, (x * x + y * y).sqrt()) * degrees) as f32,
        (x * x + y * y + z * z).sqrt() as f32,
    ]
}
