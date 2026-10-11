use mradm_dsp::room::profile::{self, Guard, Stage};

#[cfg(feature = "room-profile")]
#[test]
fn diagnostic_cpu_clock_is_optional_and_excludes_inner_hrir_queries() {
    use std::sync::atomic::{AtomicU64, Ordering};
    static READS: AtomicU64 = AtomicU64::new(0);
    fn clock() -> Option<u64> {
        Some(READS.fetch_add(1, Ordering::Relaxed) * 100)
    }
    profile::reset();
    drop(Guard::new(Stage::EmitterBegin));
    assert_eq!(profile::cpu_snapshot(), [(0, 0); profile::STAGE_COUNT]);
    assert!(profile::set_thread_cpu_clock(clock));
    assert!(!profile::set_thread_cpu_clock(clock));
    profile::reset();
    {
        let _outer = Guard::new(Stage::VoiceRender);
        drop(Guard::new(Stage::HrirQuery));
    }
    assert_eq!(READS.load(Ordering::Relaxed), 2);
    assert_eq!(
        profile::cpu_snapshot()[Stage::VoiceRender as usize],
        (1, 100)
    );
    assert_eq!(profile::cpu_snapshot()[Stage::HrirQuery as usize], (0, 0));
    assert_eq!(
        profile::snapshot()[Stage::HrirQuery as usize].0,
        u64::from(!cfg!(feature = "room-trace"))
    );
    profile::reset();
    assert_eq!(profile::snapshot(), [(0, 0); profile::STAGE_COUNT]);
    assert_eq!(profile::cpu_snapshot(), [(0, 0); profile::STAGE_COUNT]);
    let pool = rayon::ThreadPoolBuilder::new()
        .num_threads(4)
        .build()
        .unwrap();
    pool.broadcast(|_| {
        for _ in 0..64 {
            drop(Guard::new(Stage::VoiceRender));
        }
    });
    assert_eq!(profile::snapshot()[Stage::VoiceRender as usize].0, 256);
    assert_eq!(profile::cpu_snapshot()[Stage::VoiceRender as usize].0, 256);
}

#[cfg(not(feature = "room-profile"))]
#[test]
fn production_guards_never_invoke_the_native_clock() {
    fn forbidden_clock() -> Option<u64> {
        panic!("Production profiling must not read a clock")
    }
    assert!(!profile::set_thread_cpu_clock(forbidden_clock));
    drop(Guard::new(Stage::SourceResponse));
    assert_eq!(profile::snapshot(), [(0, 0); profile::STAGE_COUNT]);
    assert_eq!(profile::cpu_snapshot(), [(0, 0); profile::STAGE_COUNT]);
}
