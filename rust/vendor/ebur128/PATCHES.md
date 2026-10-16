# Local ebur128 patch

Source: crates.io ebur128 0.1.10 (upstream commit `0052abf06d8844f0972657c71e327d7eeee8967c`). Archive SHA-256:
`e227cc62d64d6fe01abbef48134b9c1f17d470cef1e7a56337ad05b1f81df7f9`.
`PROVENANCE.json` records every unchanged file after LF/trailing-whitespace normalization. The upstream lockfiles and
GitHub configuration are omitted. The license text is kept in `third_party/licenses/rust-ebur128-0.1.10/`.

Every change serves PCM consistency phase 2: the meter must give bit-identical results on macOS arm64, Linux x64 and
Windows x64. Both manifests add the pure-Rust `libm` 0.2.16 port.

True peak (2.1):

- `InterpF::new` in `src/interp.rs` takes the f64 cosine and sine of the Hanning window and sinc from `libm` instead of
  the platform C library, then converts to f32 exactly as before. The 48-tap filter, the 4x and 2x oversampling
  selection and the accumulation `*self += other * coeff` are unchanged.

Loudness (2.2):

- `filter_coefficients` in `src/filter.rs` takes the K-weighting `tan` and `pow` from `libm`.
- `src/history.rs` takes the -20 dB and -10 dB gate factors from `libm::pow`; `energy_to_loudness` in `src/utils.rs`
  takes `log10` from `libm` (the `cfg(test)` variant is unchanged and not built here).
- `src/filter.rs` no longer sets the x86 MXCSR flush-to-zero bit. Upstream used it on x86 with SSE2 and, on every other
  architecture, zeroed filter states below `f64::EPSILON` after each call; the zeroing now runs everywhere, so a state
  just below the threshold is handled the same way on all three platforms. The `ftz` module keeps its shape so the
  call sites are unchanged.
- `src/filter.rs` adds `k_weighting_coefficients(rate)`, re-exported from `src/lib.rs`, which returns the K-weighting
  numerator and denominator the meter builds for that rate so the kernel probe can record them.

Chunking (2.4):

- Filter states below `f64::EPSILON` are no longer zeroed at the end of every `Filter::process` call, which made
  momentary and short-term readings of near-silence depend on how the caller split its input. `Filter` gains
  `clear_small_states`, and `add_frames` in `src/ebur128.rs` calls it each time a 100 ms block completes: the zeroing
  points depend only on the frame count since the last reset. `Filter::seed` (not used by the project) keeps the
  per-call zeroing.

The histogram tables, block energies, gating order and summation order are unchanged. The phase-two kernel probe gates
the coefficients, true peak and loudness of the production meter at 44.1, 48, 96 and 192 kHz.
