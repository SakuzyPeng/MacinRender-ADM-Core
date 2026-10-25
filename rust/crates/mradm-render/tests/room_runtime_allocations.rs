use mradm_dsp::room::{loading, quality::Quality};
use mradm_render::room_runtime::{Config, Fault, Prepare, Resources, Runtime};
use std::alloc::{GlobalAlloc, Layout, System};
use std::cell::Cell;
use std::sync::Arc;
use std::time::{Duration, Instant};

thread_local! { static COUNT: Cell<Option<usize>> = const { Cell::new(None) }; }
struct Counting;
unsafe impl GlobalAlloc for Counting {
    unsafe fn alloc(&self, layout: Layout) -> *mut u8 {
        let _ = COUNT.try_with(|c| {
            if let Some(n) = c.get() {
                c.set(Some(n + 1));
            }
        });
        unsafe { System.alloc(layout) }
    }
    unsafe fn dealloc(&self, p: *mut u8, layout: Layout) {
        unsafe { System.dealloc(p, layout) }
    }
    unsafe fn realloc(&self, p: *mut u8, layout: Layout, size: usize) -> *mut u8 {
        let _ = COUNT.try_with(|c| {
            if let Some(n) = c.get() {
                c.set(Some(n + 1));
            }
        });
        unsafe { System.realloc(p, layout, size) }
    }
}
#[global_allocator]
static ALLOCATOR: Counting = Counting;

fn scope(call: &mut dyn FnMut() -> mradm_dsp::Result<()>) -> Result<(), Fault> {
    call().map_err(|_| (1, "scope failure"))
}

#[test]
fn room_switch_disposal_keeps_the_worker_allocation_free() {
    let manifest = concat!(
        env!("CARGO_MANIFEST_DIR"),
        "/../../../tests/fixtures/room/phase2/room.json"
    );
    let package = Arc::new(loading::load_package(manifest.as_bytes()).unwrap());
    let set = loading::prepare(&package, b"", b"", None, Quality::Exact).unwrap();
    let resources = Arc::new(Resources { set, cuda: false });
    let config = Config {
        package,
        hrir: String::new(),
        metres: 1.,
        quality: Quality::Exact,
    };
    let prepare: Prepare = Arc::new(|_, reuse| {
        Ok(Resources {
            set: Arc::clone(&reuse.set),
            cuda: false,
        })
    });
    let mut runtime =
        Runtime::create(resources, config.clone(), true, true, scope, prepare).unwrap();
    runtime.configure(&[]).unwrap();
    let control = runtime.control();
    // Only process(), on this thread, is counted. Candidate construction and destruction
    // run on the preparation thread; the count excludes status, request, and test polling.
    let mut allocations = 0;
    for _ in 0..3 {
        let revision = control.request(&config).unwrap();
        let deadline = Instant::now() + Duration::from_secs(10);
        while control.status().applied_revision != revision {
            assert!(Instant::now() < deadline);
            COUNT.with(|c| c.set(Some(0)));
            let result = runtime.process(0, std::iter::empty(), &[], &[], &mut []);
            allocations += COUNT.with(|c| c.replace(None).unwrap());
            result.unwrap();
            std::thread::sleep(Duration::from_millis(1));
        }
    }
    assert_eq!(
        allocations, 0,
        "parking displaced room instances allocated on the render worker"
    );
}
