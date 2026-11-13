use std::path::Path;

fn main() {
    println!("cargo::rerun-if-changed=build.rs");
    println!("cargo::rerun-if-env-changed=MR_ADM_FLAC_LINK_FILE");
    // CMake owns the FLAC provider. Cargo-linked examples and tests need the same
    // library, but must not bundle another copy into the production staticlib.
    if let Some(library) = std::env::var_os("MR_ADM_FLAC_LINK_FILE") {
        let library = Path::new(&library).display();
        println!("cargo::rerun-if-changed={library}");
        println!("cargo::rustc-link-arg={library}");
    }
    version();
}

/// `adm_api_version_*` return the version macros of `c_api.h`, the one place they are set.
fn version() {
    let header = Path::new(env!("CARGO_MANIFEST_DIR")).join("../../../include/adm/c_api.h");
    println!("cargo::rerun-if-changed={}", header.display());
    let text = std::fs::read_to_string(&header).expect("include/adm/c_api.h");
    let macro_value = |name: &str| -> i32 {
        let prefix = format!("#define {name} ");
        let line = text
            .lines()
            .find_map(|line| line.trim().strip_prefix(&prefix))
            .unwrap_or_else(|| panic!("c_api.h defines no {name}"));
        line.trim()
            .parse()
            .unwrap_or_else(|_| panic!("{name} is not an integer"))
    };
    let out = Path::new(&std::env::var_os("OUT_DIR").expect("OUT_DIR")).join("version.rs");
    let code = format!(
        "pub(crate) const MAJOR: i32 = {};\npub(crate) const MINOR: i32 = {};\npub(crate) const PATCH: i32 = {};\n",
        macro_value("ADM_API_VERSION_MAJOR"),
        macro_value("ADM_API_VERSION_MINOR"),
        macro_value("ADM_API_VERSION_PATCH"),
    );
    std::fs::write(out, code).expect("version.rs");
}
