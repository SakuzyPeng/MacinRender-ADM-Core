//! The consistency fixtures are the gate's inputs, so their bytes are pinned: the digests are
//! those of the C++ generator this crate replaced (`tests/tools/make_fixture.cpp`, R3 A), whose
//! files the Rust generator reproduced byte for byte. A change here changes every PCM gate input.
use mradm_testkit::fixtures::{KINDS, fixture};

fn fnv1a64(bytes: &[u8]) -> u64 {
    bytes.iter().fold(0xcbf2_9ce4_8422_2325, |h, &b| {
        (h ^ u64::from(b)).wrapping_mul(0x0000_0100_0000_01b3)
    })
}

const GOLDEN: [(&str, usize, u64); 13] = [
    ("objects-point", 145836, 0x4e333f76d22653ee),
    ("objects-extent", 145896, 0x38723293176bdc5c),
    ("objects-extent-multi", 436522, 0x0c2ffb35a9c6b9b1),
    ("objects-cartesian", 145956, 0x0ee622e38d822c77),
    ("objects-room", 146988, 0x539e13245c74f465),
    ("objects-room-96k", 290988, 0x4e20b2cb56b09e9f),
    ("params", 303062, 0x6632a756937b7f3b),
    ("directspeakers", 871694, 0x8d3cc1b06daee8c2),
    ("directspeakers-lfe", 871754, 0xb1be623bbfc9b13e),
    ("hoa", 580816, 0xd24c6aa7118cd380),
    ("hoa3", 2321236, 0x7bd17e042afdd165),
    ("policy-objects", 584166, 0xecaf90d0533baf4f),
    // Not a gate input (phase 7 encoder records); pinned so the records stay comparable.
    ("objects-long", 1729836, 0x6b3bd88d0c62c77b),
];

#[test]
fn fixtures_keep_their_bytes() {
    assert_eq!(GOLDEN.map(|g| g.0), KINDS);
    let dir = std::env::temp_dir().join(format!("mradm-testkit-{}", std::process::id()));
    std::fs::create_dir_all(&dir).unwrap();
    for (kind, len, digest) in GOLDEN {
        let f = fixture(kind).unwrap();
        let path = dir.join(format!("{kind}.wav"));
        f.built.write(&path, f.seed, f.rate, f.frames).unwrap();
        let bytes = std::fs::read(&path).unwrap();
        assert_eq!((bytes.len(), fnv1a64(&bytes)), (len, digest), "{kind}");
    }
    std::fs::remove_dir_all(&dir).unwrap();
}
