use std::path::Path;

fn main() {
    println!("cargo::rerun-if-changed=build.rs");
    version();
    shared_library_name();
}

/// The shared library's name as its consumers record it, as CMake set it before 6.6: SONAME
/// `libmradm_capi.so.<major>` on ELF; on macOS the install name `@rpath/libmradm_capi.<major>.dylib`
/// with the product version as current version and `<major>.0.0` as compatibility version (the
/// product version is `project(VERSION)` of the top-level CMakeLists.txt, as version_metadata.py
/// reads it). Windows names the DLL by its file name.
fn shared_library_name() {
    let manifest = std::env::var_os("CARGO_MANIFEST_DIR").expect("CARGO_MANIFEST_DIR");
    let cmake = Path::new(&manifest).join("../../../CMakeLists.txt");
    println!("cargo::rerun-if-changed={}", cmake.display());
    let text = std::fs::read_to_string(&cmake).expect("CMakeLists.txt");
    let version = text
        .split("project(")
        .nth(1)
        .and_then(|rest| rest.split(')').next())
        .and_then(|args| {
            args.split_whitespace()
                .skip_while(|w| *w != "VERSION")
                .nth(1)
        })
        .expect("project(... VERSION x.y.z) in CMakeLists.txt");
    let major = version.split('.').next().expect("major version");
    let os = std::env::var("CARGO_CFG_TARGET_OS").unwrap_or_default();
    if os == "macos" {
        println!(
            "cargo::rustc-cdylib-link-arg=-Wl,-install_name,@rpath/libmradm_capi.{major}.dylib"
        );
        println!("cargo::rustc-cdylib-link-arg=-Wl,-current_version,{version}");
        println!("cargo::rustc-cdylib-link-arg=-Wl,-compatibility_version,{major}.0.0");
    } else if os != "windows" {
        println!("cargo::rustc-cdylib-link-arg=-Wl,-soname,libmradm_capi.so.{major}");
    }
}

/// `adm_api_version_*` return the version macros of `c_api.h`, the one place they are set.
fn version() {
    // Resolve the active Cargo invocation, including when the build-script binary is cached.
    let manifest = std::env::var_os("CARGO_MANIFEST_DIR").expect("CARGO_MANIFEST_DIR");
    let header = Path::new(&manifest).join("../../../include/adm/c_api.h");
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
