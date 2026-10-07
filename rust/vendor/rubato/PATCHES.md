# Local rubato patch

Source: crates.io rubato 5.0.1. Archive SHA-256: cc1e951b9f5432ec1422f4dcfa5733fc501265902612edf5ef8bd8ecdd960930.
The crate's own `.cargo_vcs_info.json`, `.github/`, `.gitignore` and `Cargo.lock` are not kept.

The patch makes the asynchronous sinc resampler produce bit-identical output on macOS arm64,
Linux x64 and Windows x64 (ADR 0016). Algorithms, parameters, delays and the public API are
unchanged; only how the existing computations are carried out differs.

- `src/asynchro_sinc.rs` `make_interpolator`: always return the scalar interpolator. Upstream
  picks AVX+FMA, SSE3 or NEON at run time; each reduces the sinc dot product in a different
  order and AVX/NEON fuse multiply-add. The SIMD modules remain in the source but are unused,
  so `src/lib.rs` allows `dead_code` and `unused_imports`.
- `src/sample.rs` `Sample::sin`/`cos`: use `mradm_math` (musl-derived, IEEE basic operations
  only) instead of the platform libm. For f32 the f64 result is rounded once. This is the sine
  of the windowed-sinc table (`src/sinc.rs`).
- `src/windows.rs`: the Blackman, Blackman-Harris and Hann windows (and their squared variants)
  are evaluated locally with the periodic cosine-sum formula of `windowfunctions` 0.1.1, in the
  same f64 operation order, with `mradm_math::cos`. The `windowfunctions` dependency is removed.
- `Cargo.toml`: replace the `windowfunctions` dependency with
  `mradm-math = { path = "../../crates/mradm-math" }`.

Tests, benches and examples are kept unmodified and are not built by the workspace.
Drop this patch once upstream offers a scalar-only switch and injectable math functions.
