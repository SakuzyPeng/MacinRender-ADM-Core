use std::path::Path;

/// `mradm --version` prints the project version, set once in the top-level CMakeLists.txt.
fn main() {
    let cmake = Path::new(env!("CARGO_MANIFEST_DIR")).join("../../../CMakeLists.txt");
    println!("cargo::rerun-if-changed={}", cmake.display());
    let text = std::fs::read_to_string(&cmake).expect("CMakeLists.txt");
    let project = text
        .find("project(")
        .expect("CMakeLists.txt declares no project");
    let version = text[project..]
        .split_whitespace()
        .skip_while(|word| *word != "VERSION")
        .nth(1)
        .expect("the project declares no VERSION");
    println!("cargo::rustc-env=MRADM_VERSION={version}");
}
