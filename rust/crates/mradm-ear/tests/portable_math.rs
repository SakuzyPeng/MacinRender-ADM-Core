//! PCM consistency 3.2 and 3.3: every EAR source takes its elementary functions from the libm
//! port. Inherent `f64::sin` and friends call the platform C library (nalgebra's `libm-force` does
//! not rewrite them), so they must not reappear. Dynamically sized nalgebra matrices are excluded
//! too: their products call matrixmultiply, whose kernels (AVX/FMA, NEON) are chosen at run time.
use std::{fs, path::Path};

const PLATFORM: [&str; 16] = [
    "sin", "cos", "tan", "asin", "acos", "atan", "atan2", "sinh", "cosh", "tanh", "hypot", "powf",
    "exp", "ln", "log10", "sin_cos",
];
const DYNAMIC: [&str; 4] = ["DMatrix", "DVector", "OMatrix", "Dyn"];

#[test]
fn ear_sources_use_the_libm_port_and_fixed_order_products() {
    let src = Path::new(env!("CARGO_MANIFEST_DIR")).join("src");
    let mut files: Vec<_> = fs::read_dir(&src)
        .unwrap()
        .map(|entry| entry.unwrap().path())
        .filter(|path| path.extension().is_some_and(|ext| ext == "rs"))
        .collect();
    files.sort();
    assert!(files.len() >= 8);
    for file in files {
        let name = file.file_name().unwrap().to_string_lossy().into_owned();
        let text = fs::read_to_string(&file).unwrap();
        for (line, code) in text.lines().enumerate() {
            let code = code.split("//").next().unwrap();
            for function in PLATFORM {
                assert!(
                    !code.contains(&format!(".{function}(")),
                    "{name}:{}: platform `{function}` instead of libm",
                    line + 1
                );
            }
            for ty in DYNAMIC {
                assert!(
                    !code
                        .split(|c: char| !c.is_alphanumeric() && c != '_')
                        .any(|word| word == ty),
                    "{name}:{}: dynamically sized nalgebra `{ty}`",
                    line + 1
                );
            }
        }
    }
}
