//! Links the islands' archives and platform libraries: from the link manifest of a tree that built
//! them (`MRADM_NATIVE_LINK_FILE`, the top-level CMake test tree), or from a build of
//! `src/CMakeLists.txt` through the `cmake` crate. The manifest has one `kind:value` line per library,
//! in link order: `file:` an archive (or a shared or import library), `framework:` an Apple
//! framework, `system:` a platform library.
use std::path::{Path, PathBuf};

fn main() {
    println!("cargo::rerun-if-changed=build.rs");
    println!("cargo::rerun-if-env-changed=MRADM_NATIVE_LINK_FILE");
    for name in [
        "MR_ADM_FLAC_PROVIDER",
        "MR_ADM_OPUS_PROVIDER",
        "MR_ADM_IAMF_AOM_ROOT",
        "MRADM_NATIVE_CMAKE_ARGS",
    ] {
        println!("cargo::rerun-if-env-changed={name}");
    }
    let manifest = match std::env::var_os("MRADM_NATIVE_LINK_FILE") {
        Some(path) => PathBuf::from(path),
        None => build(),
    };
    println!("cargo::rerun-if-changed={}", manifest.display());
    println!("cargo::metadata=manifest={}", manifest.display());
    let text = std::fs::read_to_string(&manifest)
        .unwrap_or_else(|e| panic!("cannot read {}: {e}", manifest.display()));
    for line in text.lines().filter(|l| !l.trim().is_empty()) {
        let (kind, value) = line
            .split_once(':')
            .unwrap_or_else(|| panic!("{}: not a kind:value line: {line}", manifest.display()));
        match kind {
            "file" => file(Path::new(value)),
            "framework" => println!("cargo::rustc-link-lib=framework={value}"),
            "system" => println!("cargo::rustc-link-lib=dylib={value}"),
            _ => panic!("{}: unknown kind {kind}", manifest.display()),
        }
    }
}

/// One library file: an archive (or a Windows import library) links statically, without bundling
/// it into the rlib, so that a staticlib of a dependent crate leaves it to the C++ link; a shared
/// library links dynamically. The library name is the file name without the platform's prefix and
/// suffix (`libFLAC.a`, `FLAC.lib`: `FLAC`), which every linker resolves on the search path.
fn file(path: &Path) {
    println!("cargo::rerun-if-changed={}", path.display());
    let dir = path.parent().expect("library directory");
    let file = path
        .file_name()
        .expect("library name")
        .to_str()
        .expect("UTF-8 library name");
    println!("cargo::rustc-link-search=native={}", dir.display());
    let windows = std::env::var("CARGO_CFG_TARGET_OS").as_deref() == Ok("windows");
    let (kind, name) = if windows {
        ("static:-bundle", file.strip_suffix(".lib"))
    } else if let Some(stem) = file.strip_suffix(".a") {
        ("static:-bundle", stem.strip_prefix("lib"))
    } else {
        let stem = file.split(".so").next().filter(|s| s.len() < file.len());
        let stem = stem
            .or_else(|| file.strip_suffix(".dylib"))
            .or_else(|| file.strip_suffix(".tbd"));
        ("dylib", stem.and_then(|s| s.strip_prefix("lib")))
    };
    let name = name.unwrap_or_else(|| panic!("not a library file name: {}", path.display()));
    println!("cargo::rustc-link-lib={kind}={name}");
}

/// Builds the islands' project in OUT_DIR, always optimized (the C++ islands do no numerical work
/// that a debug build checks), with the dynamic C runtime on Windows like Rust.
fn build() -> PathBuf {
    let repository = Path::new(env!("CARGO_MANIFEST_DIR")).join("../../..");
    for input in ["src", "include", "cmake", "tests/manual/capture_tap.c"] {
        println!(
            "cargo::rerun-if-changed={}",
            repository.join(input).display()
        );
    }
    let root = repository.join("src");
    let mut config = cmake::Config::new(&root);
    config
        .profile("Release")
        .build_target("mradm_native")
        .define("CMAKE_MSVC_RUNTIME_LIBRARY", "MultiThreadedDLL")
        .define(
            "MR_ADM_CONSISTENCY_DIAGNOSTICS",
            on(cfg!(feature = "diagnostics")),
        );
    for name in ["MR_ADM_FLAC_PROVIDER", "MR_ADM_OPUS_PROVIDER"] {
        if let Ok(value) = std::env::var(name) {
            config.define(name, value);
        }
    }
    // IAMF encoding needs the prebuilt AOM iamf-tools bridge; without it `.iamf` is unsupported.
    let iamf = std::env::var("MR_ADM_IAMF_AOM_ROOT").unwrap_or_default();
    config.define("MR_ADM_ENABLE_IAMF", on(!iamf.is_empty()));
    if !iamf.is_empty() {
        config.define("MR_ADM_IAMF_AOM_ROOT", iamf);
    }
    // Further cache entries, `;`-separated NAME=VALUE (e.g. FETCHCONTENT_BASE_DIR of a shared cache).
    if let Ok(args) = std::env::var("MRADM_NATIVE_CMAKE_ARGS") {
        for entry in args.split(';').filter(|e| !e.is_empty()) {
            let (name, value) = entry
                .split_once('=')
                .expect("MRADM_NATIVE_CMAKE_ARGS: NAME=VALUE");
            config.define(name, value);
        }
    }
    let out = config.build();
    out.join("build").join("mradm_native_link-Release.txt")
}

fn on(enabled: bool) -> &'static str {
    if enabled { "ON" } else { "OFF" }
}
