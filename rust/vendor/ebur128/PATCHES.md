# Local ebur128 patch

Source: crates.io ebur128 0.1.10 (upstream commit `0052abf06d8844f0972657c71e327d7eeee8967c`). Archive SHA-256:
`e227cc62d64d6fe01abbef48134b9c1f17d470cef1e7a56337ad05b1f81df7f9`.
`PROVENANCE.json` records every unchanged file after LF/trailing-whitespace normalization. The upstream lockfiles and
GitHub configuration are omitted. The license text is kept in `third_party/licenses/rust-ebur128-0.1.10/`.

The only change is `InterpF::new` in `src/interp.rs`: the Hanning window and sinc of the true-peak interpolation filter
take their f64 cosine and sine from the pure-Rust `libm` 0.2.16 port instead of the platform C library, then convert to
f32 exactly as before (PCM consistency phase 2). Both manifests add the `libm` dependency. The 48-tap filter, the 4x and
2x oversampling selection, the accumulation `*self += other * coeff` and every loudness path are unchanged, so the
coefficient table is identical on all platforms by construction rather than by observation.

The phase-two kernel probe gates the true-peak results of the production meter at 44.1, 48, 96 and 192 kHz.

`src/filter.rs` adds `k_weighting_coefficients(rate)`, re-exported from `src/lib.rs`, which returns the K-weighting
numerator and denominator the meter builds for that rate. It only exposes existing values so the kernel probe can record
them (PCM consistency 2.2).
