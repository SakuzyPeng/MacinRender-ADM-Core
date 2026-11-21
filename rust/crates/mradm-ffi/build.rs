//! `cfg(mradm_iamf)` when the islands' build linked the official AOM IAMF bridge (`feature:iamf` in
//! its link manifest, passed on by mradm-islands-sys): `iamf` then calls it.
fn main() {
    println!("cargo::rustc-check-cfg=cfg(mradm_iamf)");
    println!("cargo::rerun-if-env-changed=DEP_MRADM_ISLANDS_FEATURE_IAMF");
    if std::env::var_os("DEP_MRADM_ISLANDS_FEATURE_IAMF").is_some() {
        println!("cargo::rustc-cfg=mradm_iamf");
    }
}
