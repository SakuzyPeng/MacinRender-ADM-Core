//! PCM consistency 3.2: EAR geometry, extent, panning, DirectSpeakers and decorrelation take every
//! elementary function from the libm port. Inherent `f64::sin` and friends call the platform C
//! library (nalgebra's `libm-force` does not rewrite them), so they must not reappear here.
//! `hoa.rs` is converted with the HOA slice (3.3).
use std::{fs, path::Path};

const PORTABLE: [&str; 6] = [
    "decorrelate.rs",
    "direct.rs",
    "extent.rs",
    "geom.rs",
    "lib.rs",
    "panner.rs",
];
const PLATFORM: [&str; 16] = [
    "sin", "cos", "tan", "asin", "acos", "atan", "atan2", "sinh", "cosh", "tanh", "hypot", "powf",
    "exp", "ln", "log10", "sin_cos",
];

#[test]
fn ear_sources_use_the_libm_port() {
    let src = Path::new(env!("CARGO_MANIFEST_DIR")).join("src");
    for file in PORTABLE {
        let text = fs::read_to_string(src.join(file)).unwrap();
        for (line, code) in text.lines().enumerate() {
            let code = code.split("//").next().unwrap();
            for name in PLATFORM {
                let call = format!(".{name}(");
                assert!(
                    !code.contains(&call),
                    "{file}:{}: platform `{name}` instead of libm",
                    line + 1
                );
            }
        }
    }
}
