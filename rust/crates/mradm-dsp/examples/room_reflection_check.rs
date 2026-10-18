//! Inspect native image-path geometry and distance selection without audio I/O.
use mradm_dsp::{geometry::portable_direction, room::geometry};
use std::path::Path;
#[path = "room_support/assets.rs"]
mod assets;

fn main() -> Result<(), Box<dyn std::error::Error>> {
    let args: Vec<_> = std::env::args().collect();
    if args.len() != 3 {
        return Err("usage: room_reflection_check ROOM_JSON COLLECTION_JSON".into());
    }
    let profile = assets::profile(Path::new(&args[1]))?;
    let data = assets::hrir_at_rate(Path::new(&args[2]), profile.sample_rate)?;
    let collection = data
        .collection
        .as_ref()
        .ok_or("Distance collection required")?;
    let images = geometry::images(profile.reflection_order);
    let mut rows = Vec::new();
    let mut pairs = std::collections::BTreeSet::new();
    let mut zero_gain_paths = 0;
    let mut clamped_paths = 0;
    for azimuth in [-60., 60.] {
        let direction = portable_direction(azimuth, 0.);
        for step in 0..=28 {
            let radius = 0.2 + step as f64 * 0.05;
            let source = geometry::position(
                &profile,
                [
                    -(direction[1] * radius) as f32,
                    (direction[0] * radius) as f32,
                    0.,
                ],
                1.,
                [1., 0., 0.],
            )?;
            for (index, image) in images.iter().enumerate().filter(|(_, i)| i.order > 0) {
                let path = geometry::path(&profile, source, *image);
                if path.gain == 0. {
                    zero_gain_paths += 1;
                    continue;
                }
                let (lo, hi, alpha) = collection.bracket(path.distance);
                pairs.insert((lo, hi));
                let clamped = path.distance > collection.layers.last().unwrap().radius
                    || path.distance < collection.layers[0].radius;
                clamped_paths += usize::from(clamped);
                rows.push(serde_json::json!({"source_azimuth_deg":azimuth,"source_distance_m":source.distance,
                    "image_index":index,"reflection_order":image.order,"walls":image.walls,
                    "path_distance_m":path.distance,"arrival_samples":path.samples,"propagation_gain":path.gain,
                    "direction":path.direction,"lower_radius_m":collection.layers[lo].radius,
                    "upper_radius_m":collection.layers[hi].radius,"upper_weight":alpha,"clamped":clamped}));
            }
        }
    }
    let pairs: Vec<_> = pairs
        .into_iter()
        .map(|(l, h)| [collection.layers[l].radius, collection.layers[h].radius])
        .collect();
    println!(
        "{}",
        serde_json::json!({"schema":"mradm.reflection-distance-paths","version":1,"room_hash":profile.identity,
        "collection_hash":data.identity,"collection":collection.summary(),"active_paths":rows.len(),"zero_gain_paths":zero_gain_paths,
        "clamped_paths":clamped_paths,"selected_pairs_m":pairs,"paths":rows})
    );
    Ok(())
}
