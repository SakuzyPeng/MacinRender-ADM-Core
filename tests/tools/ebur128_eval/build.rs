use std::{env, path::Path};

fn main() {
    println!("cargo:rerun-if-env-changed=EBUR128_REFERENCE_LIB_DIR");
    let directory = env::var("EBUR128_REFERENCE_LIB_DIR")
        .expect("Set EBUR128_REFERENCE_LIB_DIR to the existing Release libebur128 1.2.6 build");
    assert!(
        Path::new(&directory).is_dir(),
        "Reference directory missing"
    );
    println!("cargo:rustc-link-search=native={directory}");
    println!("cargo:rustc-link-lib=static=ebur128");
    if env::var("CARGO_CFG_TARGET_FAMILY").as_deref() == Ok("unix") {
        println!("cargo:rustc-link-lib=m");
    }
}
