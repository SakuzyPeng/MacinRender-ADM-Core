//! PCM consistency 3.5: ADM import takes its elementary functions from the libm port, so imported
//! gains and positions do not depend on the platform C library behind `f64::powf` and friends.
use std::{fs, path::Path};

#[test]
fn adm_sources_use_the_libm_port() {
    let src = Path::new(env!("CARGO_MANIFEST_DIR")).join("src");
    let mut checked = 0;
    for entry in fs::read_dir(&src).unwrap() {
        let file = entry.unwrap().path();
        if file.extension().is_none_or(|ext| ext != "rs") {
            continue;
        }
        checked += 1;
        let text = fs::read_to_string(&file).unwrap();
        for (line, code) in text.lines().enumerate() {
            let code = code.split("//").next().unwrap();
            for function in [
                "sin", "cos", "tan", "asin", "acos", "atan", "atan2", "hypot", "powf", "powi",
                "exp", "ln", "log10", "sin_cos",
            ] {
                assert!(
                    !code.contains(&format!(".{function}(")),
                    "{}:{}: platform `{function}` instead of libm",
                    file.display(),
                    line + 1
                );
            }
        }
    }
    assert!(checked >= 7);
}
