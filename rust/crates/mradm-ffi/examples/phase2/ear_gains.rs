//! EAR gain calculation on fixed metadata (PCM consistency 3.2): point-source panning, polar
//! extent and DirectSpeakers routing. Every input is a dyadic number.
use super::doubles;
use mradm_ear::{Calculator, DirectSpeakers, Object, standard_layout};
use std::path::Path;

const ELEVATIONS: [f64; 10] = [
    -90.0, -60.0, -30.0, -10.0, 0.0, 10.0, 30.0, 45.0, 60.0, 90.0,
];

fn calculator(layout: &str) -> Calculator {
    Calculator::new(standard_layout(layout).unwrap()).unwrap()
}

pub(super) fn run(out: &Path) {
    // Point sources every 3.75 degrees of azimuth on ten elevations: triplets, quads, the
    // virtual-speaker polygons below and above the layout, and the 0+2+0 downmix with its powf.
    let mut point = Vec::new();
    for layout in ["0+2+0", "0+5+0", "4+5+0", "4+7+0", "9+10+3"] {
        let mut calc = calculator(layout);
        for &elevation in &ELEVATIONS {
            for step in 0..96 {
                let object = Object {
                    position: [-180.0 + f64::from(step) * 3.75, elevation, 1.0],
                    ..Object::default()
                };
                point.extend(calc.objects(object).unwrap().0);
            }
        }
    }
    doubles(out, "ear-gains.10-point.f64", &point);

    // Polar extent: spread weights (acos/asin), extent modification by distance (atan2), depth
    // pairs and the diffuse split.
    let positions = [
        [0.0, 0.0],
        [30.0, 0.0],
        [-110.0, 15.0],
        [180.0, 45.0],
        [45.0, 90.0],
    ];
    let extents = [
        [5.0, 0.0, 0.0],
        [20.0, 10.0, 0.0],
        [45.0, 45.0, 0.0],
        [90.0, 30.0, 0.25],
        [180.0, 0.0, 0.0],
        [360.0, 360.0, 0.0],
        [30.0, 120.0, 0.5],
        [0.0, 60.0, 0.0],
    ];
    let mut extent = Vec::new();
    for layout in ["0+5+0", "4+7+0", "9+10+3"] {
        let mut calc = calculator(layout);
        for [azimuth, elevation] in positions {
            for distance in [0.5, 1.0] {
                for [width, height, depth] in extents {
                    let (direct, diffuse) = calc
                        .objects(Object {
                            position: [azimuth, elevation, distance],
                            width,
                            height,
                            depth,
                            gain: 0.75,
                            diffuse: 0.25,
                        })
                        .unwrap();
                    extent.extend(direct);
                    extent.extend(diffuse);
                }
            }
        }
    }
    doubles(out, "ear-gains.20-extent.f64", &extent);

    // DirectSpeakers without labels: exact or bounded nearest-speaker matches, and the panner
    // fallback between speakers or on ties.
    let mut direct = Vec::new();
    for layout in ["0+5+0", "4+7+0", "9+10+3"] {
        let calc = calculator(layout);
        for elevation in [-30.0, 0.0, 30.0, 60.0, 90.0] {
            for step in 0..24 {
                let azimuth = -180.0 + f64::from(step) * 15.0;
                for bounds in [
                    [None; 6],
                    [
                        Some(azimuth - 15.0),
                        Some(azimuth + 15.0),
                        Some(elevation - 10.0),
                        Some(elevation + 10.0),
                        None,
                        None,
                    ],
                ] {
                    let speakers = DirectSpeakers {
                        position: [azimuth, elevation, 1.0],
                        bounds,
                        ..DirectSpeakers::default()
                    };
                    direct.extend(calc.direct_speakers(&speakers).unwrap());
                }
            }
        }
    }
    doubles(out, "ear-gains.30-direct.f64", &direct);
}
