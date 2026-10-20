#[path = "../../mradm-dsp/examples/room_support/assets.rs"]
mod assets;
#[path = "../../mradm-dsp/examples/room_support/audio_workers.rs"]
mod audio_workers;
#[path = "support/options.rs"]
mod gpu;
#[path = "../../mradm-dsp/examples/room_support/room_bench.rs"]
mod implementation;
#[cfg(feature = "room-profile")]
#[path = "../../mradm-dsp/examples/room_bench/trace.rs"]
mod trace;
fn main() -> std::result::Result<(), Box<dyn std::error::Error>> {
    implementation::run()
}
