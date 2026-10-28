//! File-stream graph replacement must reuse convolution storage and preserve state on errors.
use super::*;
use crate::render::test_common::{ObjectSpec, TrackSpec, object_block, scene, sine};
use crate::test_alloc::COUNT;
use mradm_dsp::dataset::Dataset;

fn plan() -> RenderPrepared {
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
    RenderPrepared {
        texts: Vec::new(),
        lines: Vec::new(),
        error: bad(""),
        kernel: None,
        bindings: Arc::from([]),
        settings: (1, 2, 4096, 48000),
        triple_balance: None,
        room: None,
        binaural: Some(Arc::new(
            binaural::prepare(&scene, hrtf, Spread::Cloud, 0, &mut |_, _| {}).unwrap(),
        )),
    }
}

struct Handle {
    bank: *mut BinauralBank,
    keys: *const RenderBinauralSourceKey,
    len: usize,
}

impl Handle {
    fn new(plan: &RenderPrepared) -> Self {
        let mut handle = Self {
            bank: ptr::null_mut(),
            keys: ptr::null(),
            len: 0,
        };
        assert_eq!(handle.rebuild(plan, ptr::null(), 0), 0);
        handle
    }

    fn rebuild(
        &mut self,
        plan: &RenderPrepared,
        scales: *const RenderBinauralScales,
        len: usize,
    ) -> i32 {
        unsafe {
            mradm_render_binaural_bank_create(
                plan,
                scales,
                len,
                &mut self.bank,
                &mut self.keys,
                &mut self.len,
                ptr::null_mut(),
                ptr::null_mut(),
                ptr::null_mut(),
            )
        }
    }

    fn render(&mut self, start: u64) -> Vec<u32> {
        let h = unsafe { &mut *self.bank };
        let samples = sine(1024, 0.5);
        h.bank
            .render(
                &h.sources,
                &h.prepared.hrtf.filters,
                &samples,
                1,
                start,
                1024,
                0,
                None,
                &|_| None,
            )
            .unwrap();
        h.bank
            .states
            .iter()
            .flat_map(|s| s.left.iter().chain(&s.right).map(|v| v.to_bits()))
            .collect()
    }
}

impl Drop for Handle {
    fn drop(&mut self) {
        unsafe { mradm_render_binaural_bank_destroy(self.bank) };
    }
}

#[test]
fn failed_binaural_rebuild_preserves_views_and_audio_history() {
    let plan = plan();
    let mut actual = Handle::new(&plan);
    let mut reference = Handle::new(&plan);
    assert_eq!(actual.render(0), reference.render(0));
    let before = (actual.bank, actual.keys, actual.len);
    assert_ne!(actual.rebuild(&plan, ptr::null(), 1), 0);
    assert_eq!((actual.bank, actual.keys, actual.len), before);
    let invalid = RenderBinauralScales {
        object_id: Text {
            data: ptr::null(),
            len: 1,
        },
        scales: [1.; 5],
    };
    assert_ne!(actual.rebuild(&plan, &invalid, 1), 0);
    assert_eq!((actual.bank, actual.keys, actual.len), before);
    assert_eq!(actual.render(1024), reference.render(1024));

    let old = unsafe { &*actual.bank }.bank.states[0].left.as_ptr();
    assert_eq!(actual.rebuild(&plan, ptr::null(), 0), 0);
    assert_eq!(actual.bank, before.0);
    assert_eq!(unsafe { &*actual.bank }.bank.states[0].left.as_ptr(), old);
    let mut fresh = Handle::new(&plan);
    assert_eq!(actual.render(0), fresh.render(0));
}

#[test]
fn equal_or_smaller_binaural_graphs_do_not_allocate_workspaces() {
    let plan = plan();
    let p = plan.binaural.as_ref().unwrap();
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
