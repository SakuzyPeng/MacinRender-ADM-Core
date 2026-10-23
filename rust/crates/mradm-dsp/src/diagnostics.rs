//! Opt-in checkpoints for explicitly scoped kernel replay. Normal DSP has no scope.
//! Shared with mradm-ear; there is no DSP dependency or public C ABI addition.
//!
//! `process` writes production-path checkpoints without a scope, with the same contract as C++
//! `consistency::dump`: `MR_ADM_TRACE_DIR/<name>`, the first value per path in the process wins,
//! words are explicit little-endian. Code moved from C++ (PCM consistency phase 4) keeps emitting
//! the same files through it. Each checkpoint name has exactly one owner, so the C++ and Rust
//! registries never see the same path. Use the mradm-dsp copy; mradm-ear's is unused.
#[cfg(feature = "diagnostics")]
mod process {
    use std::{collections::BTreeSet, io::Write, path::PathBuf, sync::Mutex};
    static WRITTEN: Mutex<BTreeSet<PathBuf>> = Mutex::new(BTreeSet::new());
    pub fn record(name: &str, bytes: impl FnOnce() -> Vec<u8>) -> std::io::Result<()> {
        record_in(std::env::var_os("MR_ADM_TRACE_DIR"), name, bytes)
    }
    pub fn record_in(
        directory: Option<std::ffi::OsString>,
        name: &str,
        bytes: impl FnOnce() -> Vec<u8>,
    ) -> std::io::Result<()> {
        let Some(directory) = directory.filter(|d| !d.is_empty()) else {
            return Ok(());
        };
        let path = PathBuf::from(directory).join(name);
        let mut written = WRITTEN.lock().unwrap_or_else(|e| e.into_inner());
        if !written.insert(path.clone()) {
            return Ok(());
        }
        if let Some(parent) = path.parent() {
            std::fs::create_dir_all(parent)?;
        }
        let mut out = std::fs::File::create(&path)?;
        out.write_all(&bytes())?;
        out.flush()
    }
}
/// Element kinds of a process checkpoint; complex values are interleaved f32 pairs.
#[derive(Clone, Copy, Debug, PartialEq)]
pub enum Words<'a> {
    F32(&'a [f32]),
    F64(&'a [f64]),
    I32(&'a [i32]),
}
/// Whether production-path checkpoints are compiled in; lets callers skip building their names.
pub const ENABLED: bool = cfg!(feature = "diagnostics");
/// Write one production-path checkpoint (no-op without the `diagnostics` feature).
#[inline]
pub fn process(name: &str, words: Words<'_>) -> std::io::Result<()> {
    #[cfg(feature = "diagnostics")]
    return process::record(name, || match words {
        Words::F32(v) => v.iter().flat_map(|x| x.to_bits().to_le_bytes()).collect(),
        Words::F64(v) => v.iter().flat_map(|x| x.to_bits().to_le_bytes()).collect(),
        Words::I32(v) => v.iter().flat_map(|x| x.to_le_bytes()).collect(),
    });
    #[cfg(not(feature = "diagnostics"))]
    {
        let _ = (name, words);
        Ok(())
    }
}
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

#[cfg(all(test, feature = "diagnostics"))]
mod tests {
    #[test]
    fn process_checkpoints_keep_the_first_value_and_little_endian_words() {
        let directory = std::env::temp_dir().join(format!("mradm-diag-{}", std::process::id()));
        let _ = std::fs::remove_dir_all(&directory);
        let write = |value: f32| {
            super::process::record_in(Some(directory.clone().into()), "stage/a.10-x.f32", || {
                value.to_bits().to_le_bytes().to_vec()
            })
        };
        write(-0.0).unwrap();
        write(1.0).unwrap();
        let bytes = std::fs::read(directory.join("stage/a.10-x.f32")).unwrap();
        assert_eq!(bytes, (-0.0f32).to_bits().to_le_bytes());
        super::process::record_in(None, "unused", Vec::new).unwrap();
        std::fs::remove_dir_all(&directory).unwrap();
    }
}
