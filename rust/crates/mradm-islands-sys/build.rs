//! Links the islands' archives and platform libraries: from the link manifest of a tree that built
//! them (`MRADM_NATIVE_LINK_FILE`, the top-level CMake test tree), or from a build of
//! `src/CMakeLists.txt` through the `cmake` crate. The manifest has one `kind:value` line per library,
//! in link order. `file:`, `framework:` and `system:` identify paths and platform libraries.
//! `link:` selects a codec target, and `target:Name=FILE;LINKS` records its evaluated CMake link
//! interface and those of its dependencies. `msvc-runtime:` records the selected CRT.
use std::collections::HashMap;
use std::path::{Path, PathBuf};

type LinkTargets<'a> = HashMap<&'a str, (&'a str, &'a str)>;

fn main() {
    println!("cargo::rerun-if-changed=build.rs");
    println!("cargo::rerun-if-env-changed=MRADM_NATIVE_LINK_FILE");
    for name in [
        "MR_ADM_FLAC_PROVIDER",
        "MR_ADM_OPUS_PROVIDER",
        "MR_ADM_IAMF_AOM_ROOT",
        "MRADM_NATIVE_CMAKE_ARGS",
        "MACOSX_DEPLOYMENT_TARGET",
        "SDKROOT",
    ] {
        println!("cargo::rerun-if-env-changed={name}");
    }
    let runtime = msvc_runtime();
    let manifest = match std::env::var_os("MRADM_NATIVE_LINK_FILE") {
        Some(path) => PathBuf::from(path),
        None => build(runtime),
    };
    println!("cargo::rerun-if-changed={}", manifest.display());
    println!("cargo::metadata=manifest={}", manifest.display());
    let text = std::fs::read_to_string(&manifest)
        .unwrap_or_else(|e| panic!("cannot read {}: {e}", manifest.display()));
    let targets: LinkTargets<'_> = text
        .lines()
        .filter_map(|line| line.strip_prefix("target:"))
        .map(|value| {
            let (name, spec) = value.split_once('=').expect("target:Name=FILE;LINKS");
            (name, spec.split_once(';').expect("target:Name=FILE;LINKS"))
        })
        .collect();
    for line in text.lines().filter(|l| !l.trim().is_empty()) {
        let (kind, value) = line
            .split_once(':')
            .unwrap_or_else(|| panic!("{}: not a kind:value line: {line}", manifest.display()));
        match kind {
            "file" => file(Path::new(value)),
            "framework" => println!("cargo::rustc-link-lib=framework={value}"),
            "system" => system_library(value),
            "link" => link_item(value, &targets, &mut Vec::new()),
            "msvc-runtime" => assert!(
                Some(value) == runtime,
                "{} uses {value}, but this Rust target requires {}. Match CMAKE_MSVC_RUNTIME_LIBRARY to Rust's crt-static setting",
                manifest.display(),
                runtime.unwrap_or("a non-MSVC runtime")
            ),
            "target" => {}
            _ => panic!("{}: unknown kind {kind}", manifest.display()),
        }
    }
}

fn link_item<'a>(item: &'a str, targets: &LinkTargets<'a>, parents: &mut Vec<&'a str>) {
    if item.is_empty() || item.starts_with("::@") {
        // CMake can annotate cross-directory target references with lookup-scope markers.
        return;
    }
    if let Some((library, links)) = targets.get(item) {
        assert!(
            !parents.contains(&item),
            "cyclic codec link dependency: {item}"
        );
        if !library.is_empty() {
            file(Path::new(library));
        }
        parents.push(item);
        for dependency in links.split(';') {
            link_item(dependency, targets, parents);
        }
        parents.pop();
    } else if Path::new(item).is_absolute() {
        file(Path::new(item));
    } else if let Some(framework) = item.strip_prefix("-framework ") {
        println!("cargo::rustc-link-lib=framework={framework}");
    } else if item == "-pthread" {
        system_library("pthread");
    } else {
        let name = item.strip_prefix("-l").unwrap_or(item);
        assert!(
            !name.is_empty() && !name.starts_with('-') && !name.contains("::"),
            "unsupported CMake codec link item: {item}"
        );
        system_library(name.strip_suffix(".lib").unwrap_or(name));
    }
}

fn system_library(name: &str) {
    println!("cargo::rustc-link-lib=dylib={name}");
}

/// One library file: an archive (or a Windows import library) links statically, without bundling
/// it into the rlib, so that a staticlib of a dependent crate leaves it to the C++ link; a shared
/// library links dynamically.
fn file(path: &Path) {
    println!("cargo::rerun-if-changed={}", path.display());
    let dir = path.parent().expect("library directory");
    let file = path
        .file_name()
        .expect("library name")
        .to_str()
        .expect("UTF-8 library name");
    let windows = std::env::var("CARGO_CFG_TARGET_OS").as_deref() == Ok("windows");
    #[cfg(unix)]
    if !windows && file.ends_with(".a") {
        link_archive(path);
        return;
    }
    println!("cargo::rustc-link-search=native={}", dir.display());
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

#[cfg(unix)]
fn link_archive(path: &Path) {
    use std::hash::{DefaultHasher, Hash, Hasher};

    // With -bundle disabled Rust passes -l to the system linker. Give each archive a distinct
    // name so the linker cannot pick a shared library with the same stem (e.g. system FLAC).
    let dir = PathBuf::from(std::env::var_os("OUT_DIR").expect("OUT_DIR")).join("native-archives");
    std::fs::create_dir_all(&dir).expect("native archive directory");
    let mut hash = DefaultHasher::new();
    path.hash(&mut hash);
    let name = format!("mradm_native_{:016x}", hash.finish());
    let link = dir.join(format!("lib{name}.a"));
    if let Err(error) = std::os::unix::fs::symlink(path, &link)
        && std::fs::read_link(&link).ok().as_deref() != Some(path)
    {
        panic!(
            "cannot link {} to {}: {error}",
            link.display(),
            path.display()
        );
    }
    println!("cargo::rustc-link-search=native={}", dir.display());
    println!("cargo::rustc-link-lib=static:-bundle={name}");
}

/// Cargo describes the target's CRT even when the build script runs on a different host.
fn msvc_runtime() -> Option<&'static str> {
    if std::env::var("CARGO_CFG_TARGET_ENV").as_deref() != Ok("msvc") {
        return None;
    }
    let features = std::env::var("CARGO_CFG_TARGET_FEATURE").expect("Cargo target features");
    let static_crt = features.split(',').any(|feature| feature == "crt-static");
    Some(if static_crt {
        "MultiThreaded"
    } else {
        "MultiThreadedDLL"
    })
}

/// Builds the islands' project in OUT_DIR, optimized and with the target's C runtime.
fn build(runtime: Option<&str>) -> PathBuf {
    // A shared Cargo target directory can reuse the compiled build script in another checkout.
    let manifest_dir =
        PathBuf::from(std::env::var_os("CARGO_MANIFEST_DIR").expect("CARGO_MANIFEST_DIR"));
    for input in ["src", "include", "cmake", "tests/manual/capture_tap.c"] {
        println!("cargo::rerun-if-changed=../../../{input}");
    }
    let root = manifest_dir.join("../../../src");
    let mut config = cmake::Config::new(&root);
    config
        .profile("Release")
        .build_target("mradm_native")
        .define(
            "MR_ADM_CONSISTENCY_DIAGNOSTICS",
            on(cfg!(feature = "diagnostics")),
        );
    if let Some(runtime) = runtime {
        config.define("CMAKE_MSVC_RUNTIME_LIBRARY", runtime);
    }
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
