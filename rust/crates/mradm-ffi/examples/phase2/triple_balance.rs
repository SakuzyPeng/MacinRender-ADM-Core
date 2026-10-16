//! Triple Balance panning on fixed positions and sizes (PCM consistency 3.4): the standard-mode
//! point, room, size lattice and mixes shared by offline, file-streaming and Scene rendering,
//! and the D-mode gains. Every input is a dyadic number.
use super::floats;
use mradm_dsp::triple_balance::{Layout, Position, d_mode, panner};
use std::path::Path;

fn positions() -> Vec<Position> {
    let mut result = Vec::new();
    for z in [0.0, 0.25, 0.5, 0.75, 1.0] {
        for y in 0..9 {
            for x in 0..9 {
                result.push(Position {
                    x: -1.0 + x as f32 * 0.25,
                    y: -1.0 + y as f32 * 0.25,
                    z,
                });
            }
        }
    }
    result
}

fn mix(out: &mut Vec<f32>, mix: panner::Mix) {
    out.push(mix.direct);
    out.extend(mix.spread);
}

pub(super) fn run(out: &Path) {
    let grid = positions();

    let mut point = Vec::new();
    for layout in [Layout::Seven, Layout::Nine, Layout::Room222] {
        for &p in &grid {
            point.extend(panner::point(p, 0.75, layout).unwrap());
        }
    }
    floats(out, "triple-balance.10-point.f32", &point);

    // 22.2 room panning with size, and its direct / spread mix.
    let mut room = Vec::new();
    for size in [0.0, 0.125, 0.375, 0.75] {
        for &p in &grid {
            let gains = panner::room(p, size).unwrap();
            room.extend(gains);
            mix(&mut room, panner::room_mix(gains, size));
        }
    }
    floats(out, "triple-balance.20-room.f32", &room);

    // 7.1.4 / 9.1.6 size lattice on quantized internal coordinates, and its mix.
    let mut size = Vec::new();
    for s in [0.0, 0.0625, 0.25, 0.5, 1.0] {
        for &p in &grid {
            let raw = panner::raw(panner::quantize(p.internal(), s).unwrap()).unwrap();
            size.extend(raw);
            mix(&mut size, panner::size_mix(raw, s));
        }
    }
    floats(out, "triple-balance.30-size.f32", &size);

    // D mode, already on the libm port since its introduction; gated on its own from 3.4.
    let mut d = Vec::new();
    for layout in [Layout::Nine, Layout::Room222] {
        for s in [0.0, 0.25, 0.625, 1.0] {
            for &p in &grid {
                d.extend(d_mode::gains_for_layout(p, s, layout).unwrap());
            }
        }
    }
    floats(out, "triple-balance.40-d-mode.f32", &d);
}
