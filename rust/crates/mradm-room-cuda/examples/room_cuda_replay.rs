#[path = "../../mradm-dsp/examples/room_support/assets.rs"]
mod assets;
#[path = "support/options.rs"]
mod gpu;
#[path = "../../mradm-dsp/examples/room_support/room_replay.rs"]
mod implementation;
fn main() -> std::result::Result<(), Box<dyn std::error::Error>> {
    implementation::run()
}
