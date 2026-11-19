fn main() {
    println!("cargo::rerun-if-changed=build.rs");
    println!("cargo::rerun-if-env-changed=MR_ADM_FLAC_LINK_FILE");
    println!("cargo::rerun-if-env-changed=MR_ADM_ISLAND_LINK_FILE");
    // CMake owns the FLAC provider. Cargo-linked examples and tests need the same
    // library, but must not bundle another copy into the production staticlib.
    if let Some(library) = std::env::var_os("MR_ADM_FLAC_LINK_FILE") {
        let library = std::path::Path::new(&library).display();
        println!("cargo::rerun-if-changed={library}");
        println!("cargo::rustc-link-arg={library}");
    }
    // The room device probes (feature `islands`) link the C++ islands: CMake writes their
    // archives and platform libraries one linker argument per line. A check (clippy) needs none.
    if std::env::var_os("CARGO_FEATURE_ISLANDS").is_some()
        && let Some(list) = std::env::var_os("MR_ADM_ISLAND_LINK_FILE")
    {
        let list = std::path::PathBuf::from(list);
        println!("cargo::rerun-if-changed={}", list.display());
        let text = std::fs::read_to_string(&list)
            .unwrap_or_else(|e| panic!("cannot read {}: {e}", list.display()));
        for line in text.lines().filter(|l| !l.is_empty()) {
            if std::path::Path::new(line).is_file() {
                println!("cargo::rerun-if-changed={line}");
            }
            println!("cargo::rustc-link-arg-examples={line}");
        }
    }
}
