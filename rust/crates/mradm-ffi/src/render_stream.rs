//! Private file-stream boundary: one handle for every Rust backend's
//! stream (`mradm-engine::stream`). Validation may overlap the worker and borrows only the
//! immutable validator; serialize every other call on the worker and destroy after all return.
//! Errors carry the stream locator (`MradmRenderLocation`) like the offline driver's.
