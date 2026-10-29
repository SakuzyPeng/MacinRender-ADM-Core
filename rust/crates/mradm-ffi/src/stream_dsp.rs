//! Private boundary for the monitor's live gain (PCM consistency 3.1); the file-stream arithmetic
//! is called inside Rust since 4.7 C.
use mradm_dsp::stream_dsp;

#[unsafe(no_mangle)]
pub extern "C" fn mradm_dsp_live_gain(db: f32) -> f32 {
    stream_dsp::live_gain(db)
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn forwards() {
        assert_eq!(mradm_dsp_live_gain(0.0), 1.0);
    }
}
