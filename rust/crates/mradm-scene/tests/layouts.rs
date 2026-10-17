use mradm_scene::layouts::{Geometry, layouts};
use std::fmt::Write;

fn range(out: &mut String, range: Option<[f32; 2]>) {
    match range {
        Some([min, max]) => write!(out, " {:08x}:{:08x}", min.to_bits(), max.to_bits()).unwrap(),
        None => out.push_str(" -"),
    }
}

/// The tables match, bit for bit, the C++ `render_layouts` tables they replaced. The fixture was
/// written by the C++ implementation before its deletion (PCM consistency 4.3).
#[test]
fn layouts_match_the_retired_cpp_tables() {
    let mut out = String::new();
    for (name, geometry) in [("standard", Geometry::Standard), ("apple", Geometry::Apple)] {
        for layout in layouts(geometry) {
            writeln!(out, "{name} {} {}", layout.id, layout.display_name).unwrap();
            for s in &layout.speakers {
                write!(
                    out,
                    "  {} {:08x} {:08x} {}",
                    s.label,
                    s.azimuth.to_bits(),
                    s.elevation.to_bits(),
                    u8::from(s.is_lfe)
                )
                .unwrap();
                range(&mut out, s.azimuth_range);
                range(&mut out, s.elevation_range);
                out.push('\n');
            }
        }
    }
    assert_eq!(out, include_str!("layouts-v1.txt"));
}
