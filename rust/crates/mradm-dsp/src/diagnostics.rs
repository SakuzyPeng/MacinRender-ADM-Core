//! Opt-in checkpoints for explicitly scoped kernel replay. Normal DSP has no scope.
//! Shared with mradm-ear; there is no DSP dependency or public C ABI addition.
#[cfg(feature = "diagnostics")]
mod enabled {
    use std::{cell::RefCell, collections::HashSet, io::Write};
    thread_local! {
        static SCOPE: RefCell<(String, HashSet<String>)> = RefCell::new((String::new(), HashSet::new()));
    }
    pub fn scope(name: &str) {
        SCOPE.with_borrow_mut(|state| {
            state.0 = name.to_owned();
            state.1.clear();
        });
    }
    pub fn record(name: &str, bytes: impl FnOnce() -> Vec<u8>) {
        SCOPE.with_borrow_mut(|state| {
            if state.0.is_empty() {
                return;
            }
            let Some(directory) = std::env::var_os("MR_ADM_TRACE_DIR") else {
                return;
            };
            if !state.1.insert(name.to_owned()) {
                return;
            }
            let path = std::path::PathBuf::from(directory)
                .join(&state.0)
                .join(name);
            std::fs::create_dir_all(path.parent().unwrap()).expect("checkpoint directory");
            let mut out = std::fs::File::create(path).expect("checkpoint file");
            out.write_all(&bytes()).expect("checkpoint write");
            out.flush().expect("checkpoint flush");
        });
    }
}
#[inline]
pub fn scope(name: &str) {
    #[cfg(feature = "diagnostics")]
    enabled::scope(name);
    #[cfg(not(feature = "diagnostics"))]
    let _ = name;
}
#[inline]
pub fn f32(name: &str, values: &[f32]) {
    #[cfg(feature = "diagnostics")]
    enabled::record(name, || {
        values
            .iter()
            .flat_map(|v| v.to_bits().to_le_bytes())
            .collect()
    });
    #[cfg(not(feature = "diagnostics"))]
    let _ = (name, values);
}
#[inline]
pub fn f64(name: &str, values: &[f64]) {
    #[cfg(feature = "diagnostics")]
    enabled::record(name, || {
        values
            .iter()
            .flat_map(|v| v.to_bits().to_le_bytes())
            .collect()
    });
    #[cfg(not(feature = "diagnostics"))]
    let _ = (name, values);
}
