#[path = "room_support/assets.rs"]
mod assets;
#[path = "room_support/audio_workers.rs"]
mod audio_workers;
#[cfg(feature = "room-gpu-experiment")]
#[path = "room_support/gpu.rs"]
mod gpu;

#[cfg(feature = "room-profile")]
#[path = "room_bench/trace.rs"]
mod trace;

#[path = "room_support/room_bench.rs"]
mod implementation;
fn main() -> std::result::Result<(), Box<dyn std::error::Error>> {
    implementation::run()
}
