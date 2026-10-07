# Local RustFFT patch

Source: crates.io RustFFT 6.4.1. Archive SHA-256:
`21db5f9893e91f41798c88680037dba611ca6674703c1a18601b01a72c8adb89`.
`PROVENANCE.json` records every unchanged file after LF/trailing-whitespace normalization. Publication metadata, the upstream
lockfile and GitHub configuration are omitted; both upstream licenses are retained.

The workspace still disables all RustFFT/RealFFT SIMD features. Planner recipes,
base butterflies, twiddle generation, f32/f64 precision and scratch requirements
are unchanged. There is no new runtime dispatch or fast-math setting.

`src/algorithm/radix4.rs` prepares each layer's coefficients in groups of four
independent columns. `radix4_columns.rs` computes these columns with separate real
and imaginary arrays, allowing the compiler to vectorize ordinary Rust arithmetic.
Each complex multiply and radix-4 butterfly retains the upstream expression tree,
including the order of operands, the direction-dependent negation and signed zero.
Coefficients are only rearranged; they are never recalculated or rounded again.
The patched manifest requires Rust 1.63 for `array::from_fn` (the project pins
Rust 1.98). Its fixed-size construction enables vectorization; replacing it with
iterator-based array construction must be benchmarked, even when bits match.
Base lengths not divisible by four use the original expressions for the remaining
columns. Processing performs no allocation. The new module forbids unsafe code.

`mradm-dsp/tests/fft_columns.rs` compiles this same module and compares raw f32/f64
bits against the unchanged upstream `Butterfly4`, after the original scalar
complex multiplies. Both directions, vector boundaries, custom base widths, signed
zeros, cancellation, underflow and varied magnitudes run in the normal Rust test
suite on every platform. Full FFT/renderer validation uses the existing phase-two
Release probes and all 78 PCM gates, plus comparison against pre-patch outputs.

This patch is confined to the scalar radix-4 implementation. The upstream SIMD
implementations remain present but disabled. It can be removed when an upstream
release provides an equivalent optimization and passes the same bit comparisons.
