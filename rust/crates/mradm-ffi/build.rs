fn main() {
    println!("cargo::rerun-if-changed=build.rs");
    println!("cargo::rerun-if-env-changed=MR_ADM_FLAC_LINK_FILE");
    // CMake owns the FLAC provider. Cargo-linked examples and tests need the same
    // library, but must not bundle another copy into the production staticlib.
    if let Some(library) = std::env::var_os("MR_ADM_FLAC_LINK_FILE") {
        let library = std::path::Path::new(&library).display();
        println!("cargo::rerun-if-changed={library}");
        println!("cargo::rustc-link-arg={library}");
    }
}
