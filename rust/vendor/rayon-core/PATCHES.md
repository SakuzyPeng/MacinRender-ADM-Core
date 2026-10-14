# Local Rayon core patch

Source: crates.io Rayon core 1.13.0. `PROVENANCE.json` pins the archive and
records the normalized hashes of every unchanged file. Both upstream licenses
are retained here and in `third_party/licenses/rust-rayon-core-1.13.0/`.

`src/sleep/worker_sleep_state.rs` extracts the existing worker parking state
from `src/sleep/mod.rs`. On Apple targets its constructor performs a zero-duration
condition-variable wait using that state's own mutex. This initializes the
pthread-backed Rust synchronization storage before the registry starts workers.
Other targets retain the same initial mutex, condition variable, and false flag.
Scheduling, work stealing, sleep/wake transitions, and task execution are unchanged.

Room DSP previously tried to force workers to park using a stolen join task and
a 5 ms sleep. A joining worker can be descheduled for that entire interval, so
completion of that warmup does not prove the worker's first wait occurred.
The constructor now prepares the actual primitives independently of scheduling;
the room mailbox is prepared in the same way.

The project test `rust/crates/mradm-dsp/tests/rayon_sleep_no_alloc.rs` includes
this private implementation and checks the first timed wait and both notification
operations after construction for 1/2/3/8/32 worker states. It deterministically
fails on macOS with the upstream lazy constructor (two allocations for a cold
state), including when the mutex was used earlier (one 48-byte condition-variable
allocation with the pinned Rust 1.98.0 toolchain). The existing `room_no_alloc`
test still checks every worker from the first DSP block, including room switches.
