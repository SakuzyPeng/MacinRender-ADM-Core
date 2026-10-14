# Local RealFFT patch

Source: crates.io RealFFT 3.5.0 (upstream commit `d0d4eee0525fd27c96c8a046d6d107acd5ed84a6`). Archive SHA-256:
`f821338fddb99d089116342c46e9f1fbf3828dba077674613e734e01d6ea8677`.
`PROVENANCE.json` records every unchanged file after LF/trailing-whitespace normalization. Publication metadata, the upstream
lockfile and GitHub configuration are omitted. The package declares MIT without a separate license file; the license text is
kept in `third_party/licenses/rust-realfft-3.5.0/`.

The only change is `compute_twiddle` in `src/lib.rs`: the real-to-complex post-processing twiddles take their f64 cosine and
sine from the pure-Rust `libm` 0.2.16 port instead of the platform C library, then convert to the FFT precision exactly as
before (ADR 0018). Both manifests add the `libm` dependency. The angle expression, the conversion and every FFT algorithm
are unchanged. RustFFT's `compute_twiddle` carries the same patch, so every twiddle either crate builds is identical on all
platforms by construction rather than by observation.

The phase-two kernel probe computes the same twiddles with `libm` and gates both the f64 values and the f32 table.
