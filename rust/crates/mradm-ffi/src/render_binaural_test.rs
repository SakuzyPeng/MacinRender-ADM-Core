//! File-stream graph replacement must reuse convolution storage (the stream itself is tested in
//! `mradm-render`; the allocation counter lives in this crate).
use super::*;
use crate::render::test_common::{ObjectSpec, TrackSpec, object_block, scene};
use crate::test_alloc::COUNT;
use mradm_dsp::dataset::Dataset;
use mradm_render::binaural::Spread;
use mradm_render::binaural_render as binaural;
use mradm_render::binaural_render::Bank;

fn plan() -> binaural::Prepared {
    let mut block = object_block(0, 4096, 30.);
    block.width = 60.;
    block.diffuse = 0.3;
    let scene = scene(
        vec![ObjectSpec {
            id: "object",
            gain: 1.,
            end: 4096,
            tracks: vec![TrackSpec {
                blocks: vec![block],
                direct: Vec::new(),
            }],
        }],
        4096,
    );
    let hrtf = Arc::new(binaural_hrtf::build(Dataset::kemar(), 1024, false).unwrap());
    binaural::prepare(&scene, hrtf, Spread::Cloud, 0, &mut |_, _| {}).unwrap()
}

#[test]
fn equal_or_smaller_binaural_graphs_do_not_allocate_workspaces() {
    let p = plan();
    let mut bank = Bank::new(&p.hrtf, 4, 1024, 2).unwrap();
    for count in [4, 3, 1, 0] {
        COUNT.with(|counter| counter.set(Some(0)));
        let result = bank.rebuild(count);
        let allocations = COUNT.with(|counter| counter.replace(None).unwrap());
        result.unwrap();
        assert_eq!(allocations, 0, "rebuilding {count} convolution workspaces");
        assert_eq!(bank.states.len(), count);
    }
}
