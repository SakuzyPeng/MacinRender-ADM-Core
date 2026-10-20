#[path = "room_support/assets.rs"]
mod assets;
#[cfg(feature = "room-gpu-experiment")]
#[path = "room_support/gpu.rs"]
mod gpu;

#[path = "room_support/room_replay.rs"]
mod implementation;
fn main() -> std::result::Result<(), Box<dyn std::error::Error>> {
    implementation::run()
}
