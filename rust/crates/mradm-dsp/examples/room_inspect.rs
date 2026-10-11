//! Standalone preparation probe; no audio/device dependencies.
use mradm_dsp::{
    Error, Result,
    room::{
        geometry,
        resources::{HrirData, RoomProfile},
    },
};
use std::{fs, path::Path};

fn read(root: &Path, name: &str, expected: usize) -> Result<Vec<u8>> {
    let path = root.join(name);
    if fs::metadata(&path)
        .map_err(|_| Error::Io("Missing room asset"))?
        .len()
        != expected as u64
    {
        return Err(Error::Io("Room asset length mismatch"));
    }
    fs::read(path).map_err(|_| Error::Io("Cannot read room asset"))
}

fn main() -> std::result::Result<(), Box<dyn std::error::Error>> {
    let argument = std::env::args()
        .nth(1)
        .ok_or("usage: room_inspect ROOM_JSON")?;
    let path = Path::new(&argument);
    let root = path.parent().ok_or("missing parent")?;
    let room = RoomProfile::parse(&fs::read(path)?, |n, s| read(root, n, s))?;
    let hrir = HrirData::parse(&fs::read(root.join(&room.default_hrir))?, |n, s| {
        read(root, n, s)
    })?;
    let mut active = 0;
    let position = geometry::position(&room, [0., 1., 0.], 1.5, [1., 0., 0.])?;
    for image in geometry::images(room.reflection_order) {
        if geometry::path(&room, position, image).gain != 0. {
            active += 1;
        }
    }
    println!(
        "{}",
        serde_json::json!({
            "room": room.id, "hash": room.identity, "hrir_hash": hrir.identity,
            "hrir_directions": hrir.directions.len(), "hrir_faces": hrir.faces.len(),
            "late_directions": room.late_directions.len(), "late_taps": room.late_taps,
            "eq_taps": room.eq.len(), "default_position_m": position.point,
            "active_default_paths": active,
        })
    );
    Ok(())
}
