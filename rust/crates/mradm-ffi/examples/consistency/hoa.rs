//! HOA encoding coefficients and EAR HOA decoders on fixed metadata (PCM consistency 3.3).
//! Every input is a dyadic number.
use super::{doubles, floats};
use mradm_dsp::hoa::{Source, Trace, coefficients::compile};
use mradm_ear::{Calculator, Normalization, standard_layout};
use std::path::Path;

fn coefficients(out: &mut Vec<f32>, kind: u32, sources: &[Source], object_gain: f32) {
    let result = compile(kind, sources, object_gain, &mut Trace::default()).unwrap();
    out.extend(result.direct);
    if kind == 0 {
        out.extend(result.diffuse.iter().flatten());
    }
}

fn polar(azimuth: f32, elevation: f32, distance: f32) -> Source {
    Source {
        position: [azimuth, elevation, distance],
        gain: 1.0,
        ..Source::default()
    }
}

pub(super) fn run(out: &Path) {
    // Point objects (extent path without spread) every 7.5 degrees on nine elevations, and the
    // same directions as DirectSpeakers channels (polar encode).
    let mut point = Vec::new();
    let mut direct = Vec::new();
    for elevation in [-90.0, -60.0, -30.0, -7.5, 0.0, 7.5, 30.0, 60.0, 90.0] {
        for step in 0..48 {
            let source = polar(-180.0 + step as f32 * 7.5, elevation, 1.0);
            coefficients(&mut point, 0, &[source], 1.0);
            coefficients(&mut direct, 1, &[source], 0.75);
        }
    }
    floats(out, "hoa-encode.10-point.f32", &point);
    floats(out, "hoa-encode.30-direct.f32", &direct);

    // Extent sampling (tan of the disk offsets), distance scaling, Cartesian positions, the
    // diffuse split and the three-source divergence slots.
    let mut extent = Vec::new();
    let positions = [
        (false, [0.0, 0.0, 1.0]),
        (false, [30.0, 15.0, 0.5]),
        (false, [-110.0, -20.0, 2.0]),
        (false, [45.0, 90.0, 1.0]),
        (true, [0.25, 0.75, 0.5]),
        (true, [-0.5, -0.25, 0.125]),
    ];
    let extents = [
        [0.0, 0.0, 0.0],
        [0.125, 0.0, 0.0],
        [0.25, 0.5, 0.0],
        [0.5, 0.25, 0.25],
        [1.0, 1.0, 0.5],
        [0.0, 0.75, 0.0],
    ];
    for (cartesian, position) in positions {
        for [width, height, depth] in extents {
            let source = Source {
                position,
                cartesian: u32::from(cartesian),
                width,
                height,
                depth,
                gain: 0.5,
                diffuse: 0.25,
            };
            coefficients(&mut extent, 0, &[source], 0.75);
        }
    }
    for spread in [0.125, 0.5] {
        let sources = [-30.0f32, 0.0, 30.0].map(|azimuth| Source {
            width: spread,
            diffuse: 0.5,
            gain: 0.25,
            ..polar(azimuth, 10.0, 1.0)
        });
        coefficients(&mut extent, 0, &sources, 1.0);
    }
    floats(out, "hoa-encode.20-extent.f32", &extent);

    // EAR AllRAD decoders: full orders 1-3 in every normalization, and a sparse mixed-order set.
    let mut decode = Vec::new();
    let full = |order: i32| -> (Vec<i32>, Vec<i32>) {
        (0..=order)
            .flat_map(|n| (-n..=n).map(move |m| (n, m)))
            .unzip()
    };
    for layout in ["0+5+0", "4+5+0", "4+7+0", "9+10+3"] {
        let mut calc = Calculator::new(standard_layout(layout).unwrap()).unwrap();
        for order in 1..=3 {
            let (orders, degrees) = full(order);
            for norm in [Normalization::Sn3d, Normalization::N3d, Normalization::Fuma] {
                decode.extend(calc.hoa(&orders, &degrees, norm).unwrap());
            }
        }
        decode.extend(
            calc.hoa(
                &[0, 1, 1, 1, 3, 3],
                &[0, -1, 0, 1, -3, 3],
                Normalization::N3d,
            )
            .unwrap(),
        );
    }
    doubles(out, "ear-hoa.10-decode.f64", &decode);
}
