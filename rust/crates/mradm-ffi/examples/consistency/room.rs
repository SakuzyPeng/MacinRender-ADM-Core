//! The pinned synthetic room pack through the exact-tier preparation (PCM consistency 4.2):
//! time-domain HRIR queries of the prepared decoder and direct/early source responses.
use super::floats;
use mradm_dsp::{
    Error, Result,
    room::{
        geometry::{self as room_geometry, Rotation},
        hrir::HrirBank,
        prepared::{PreparedRoom, ResponseWork},
        quality::Quality,
        resources::{HrirData, RoomProfile},
    },
};
use std::{fs, path::Path, sync::Arc};

fn asset(root: &Path, name: &str, size: usize) -> Result<Vec<u8>> {
    let bytes = fs::read(root.join(name)).map_err(|_| Error::Io("room asset"))?;
    if bytes.len() != size {
        return Err(Error::Io("room asset size"));
    }
    Ok(bytes)
}

pub(super) fn run(out: &Path, fixtures: &Path) {
    let root = fixtures.join("room");
    let profile = Arc::new(
        RoomProfile::parse(&fs::read(root.join("room.json")).unwrap(), |n, s| {
            asset(&root, n, s)
        })
        .unwrap(),
    );
    let data = Arc::new(
        HrirData::parse(
            &fs::read(root.join(&profile.default_hrir)).unwrap(),
            |n, s| asset(&root, n, s),
        )
        .unwrap(),
    );
    let quality = Quality::Exact;
    let (fft, _, _) = PreparedRoom::required_fft(&profile, &data).unwrap();
    let bank = Arc::new(HrirBank::with_policy(data.clone(), fft, quality.fft()).unwrap());
    let room = PreparedRoom::with_quality(profile.clone(), bank, quality).unwrap();
    let mut work = ResponseWork::new(&room).unwrap();
    // Directions on the octahedron's vertices, edges and faces, and next to the poles.
    let directions = [
        [1., 0., 0.],
        [0., 1., 0.],
        [-0.6, 0.8, 0.],
        [0.48, -0.6, 0.64],
        [0., 0., 1.],
        [0.001, 0., -0.9999995],
    ];
    let mut face = usize::MAX;
    let mut query = vec![0.; data.output_taps * 2];
    let mut hrirs = Vec::new();
    for direction in directions {
        work.hrir(&room, direction, &mut face, &mut query).unwrap();
        hrirs.extend_from_slice(&query);
    }
    floats(out, "room.10-hrir.f32", &hrirs);
    // Scene positions in room units (1.5 m) and head rotations, including a source next to the
    // minimum distance and one close to a wall.
    let mut impulses = Vec::new();
    let mut previous = [1., 0., 0.];
    for (scene, pose) in [
        ([0., 1.4, 0.], [0., 0., 0.]),
        ([-0.75, 0.5, 0.25], [37., 12., -9.]),
        ([0.125, 0.0625, 0.], [-90., 0., 0.]),
        ([1.5, -1.25, 0.5], [12.5, -30., 5.]),
    ] {
        let position = room_geometry::position(&room.profile, scene, 1.5, previous).unwrap();
        previous = position.direction;
        work.build(&room, position, &Rotation::new(pose).unwrap())
            .unwrap();
        impulses.extend_from_slice(&work.impulse);
    }
    floats(out, "room.20-impulse.f32", &impulses);
}
