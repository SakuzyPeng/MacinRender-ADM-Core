//! Native clocks and bounded, preallocated call records for the diagnostic probe.
use mradm_dsp::room::profile;
use serde_json::{Value, json};
use std::time::Instant;

type Counters = [(u64, u64); profile::STAGE_COUNT];

#[cfg(all(target_os = "linux", target_pointer_width = "64"))]
fn thread_cpu_ns() -> Option<u64> {
    use std::ffi::{c_int, c_long};
    #[repr(C)]
    struct Timespec {
        seconds: c_long,
        nanos: c_long,
    }
    unsafe extern "C" {
        fn clock_gettime(clock: c_int, value: *mut Timespec) -> c_int;
    }
    let mut value = Timespec {
        seconds: 0,
        nanos: 0,
    };
    // Linux 64-bit time_t and long have the same layout. The pointer is valid
    // for this synchronous call; CLOCK_THREAD_CPUTIME_ID is 3 on Linux.
    if unsafe { clock_gettime(3, &mut value) } != 0 {
        return None;
    }
    let seconds = u64::try_from(value.seconds).ok()?;
    let nanos = u64::try_from(value.nanos).ok()?;
    (nanos < 1_000_000_000)
        .then_some(())
        .and_then(|()| seconds.checked_mul(1_000_000_000)?.checked_add(nanos))
}

#[cfg(target_os = "macos")]
fn thread_cpu_ns() -> Option<u64> {
    unsafe extern "C" {
        fn clock_gettime_nsec_np(clock: std::ffi::c_uint) -> u64;
    }
    // The macOS SDK declares CLOCK_THREAD_CPUTIME_ID as 16. This clock has no
    // user-provided pointers and reads only the current thread's CPU time.
    Some(unsafe { clock_gettime_nsec_np(16) })
}

#[cfg(not(any(
    target_os = "macos",
    all(target_os = "linux", target_pointer_width = "64")
)))]
fn thread_cpu_ns() -> Option<u64> {
    None
}

pub fn enable_cpu_clock() -> Option<f64> {
    thread_cpu_ns()?;
    if !profile::set_thread_cpu_clock(thread_cpu_ns) {
        return None;
    }
    let start = Instant::now();
    for _ in 0..1000 {
        let _ = std::hint::black_box(thread_cpu_ns());
    }
    Some(start.elapsed().as_nanos() as f64 / 1000.)
}

#[derive(Clone, Copy, Default)]
pub struct Start {
    wall: Counters,
    cpu: Counters,
    phase: u32,
}

pub fn start(phase: u32) -> Start {
    Start {
        wall: profile::snapshot(),
        cpu: profile::cpu_snapshot(),
        phase,
    }
}

#[derive(Clone, Copy, Default)]
struct Call {
    block: usize,
    phase_before: u32,
    phase_after: u32,
    seconds: f64,
    wall: Counters,
    cpu: Counters,
}

pub struct Trace {
    calls: Vec<Call>,
    clock_read_ns: Option<f64>,
}

fn delta(before: Counters, after: Counters) -> Counters {
    std::array::from_fn(|i| (after[i].0 - before[i].0, after[i].1 - before[i].1))
}

fn stages(counters: Counters) -> Value {
    Value::Object(
        profile::NAMES
            .iter()
            .zip(counters)
            .map(|(name, (calls, nanos))| {
                (
                    (*name).to_owned(),
                    json!({"calls": calls, "summed_ms": nanos as f64 / 1e6}),
                )
            })
            .collect(),
    )
}

impl Trace {
    pub fn new(blocks: usize, clock_read_ns: Option<f64>) -> Self {
        let mut calls = vec![Call::default(); blocks];
        // Commit record pages before measurement; capture itself never allocates.
        for call in calls.iter_mut().step_by((4096 / size_of::<Call>()).max(1)) {
            let copy = *call;
            *std::hint::black_box(call) = std::hint::black_box(copy);
        }
        Self {
            calls,
            clock_read_ns,
        }
    }

    pub fn finish(&mut self, block: usize, before: Start, phase_after: u32, seconds: f64) {
        self.calls[block] = Call {
            block,
            phase_before: before.phase,
            phase_after,
            seconds,
            wall: delta(before.wall, profile::snapshot()),
            cpu: delta(before.cpu, profile::cpu_snapshot()),
        };
    }

    pub fn report(mut self, deadline: f64) -> Value {
        self.calls.sort_by(|a, b| b.seconds.total_cmp(&a.seconds));
        let misses = self.calls.iter().filter(|c| c.seconds > deadline).count();
        let slow_calls: Vec<_> = self
            .calls
            .iter()
            .take(32)
            .map(|call| {
                json!({
                    "measured_block": call.block, "phase_before": call.phase_before,
                    "phase_after": call.phase_after, "wall_ms": call.seconds * 1000.,
                    "wall_stages": stages(call.wall), "thread_cpu_stages": stages(call.cpu),
                })
            })
            .collect();
        json!({
            "thread_cpu_available": self.clock_read_ns.is_some(),
            "inner_stage_walls": !cfg!(feature = "room-trace"),
            "clock_read_mean_ns": self.clock_read_ns,
            "captured_calls": self.calls.len(), "deadline_misses": misses,
            "retained_slowest_calls": slow_calls,
            "stage_cpu_totals": stages(profile::cpu_snapshot()),
            "scope": "Instrumented diagnostic only. Stage wall/CPU totals sum across threads and overlap when nested. CPU excludes blocking/descheduling but not clock overhead. CPU clocks cover session/block/worker/voice and late stages. room-trace disables inner arithmetic-stage clocks entirely; room-profile alone retains inner stage wall times. Waiting for other workers is not evidence of OS preemption. Capacity requires a separate unprofiled run."
        })
    }
}
