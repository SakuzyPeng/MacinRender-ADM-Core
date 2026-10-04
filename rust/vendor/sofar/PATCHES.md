# Local sofar patch

Source: crates.io sofar 0.3.0. Archive SHA-256: 060ea9e290d57f909c19dfd5673a489cfedf4fdcf02eeae65a470a4923225be2.

Expose the existing `Hrtf::from_bytes` parser as `sofar::RawHrtf`. MacinRender owns convention validation and reads unmodified HRIR data; the high-level reader automatically converts coordinates and may normalize/resample. No C sources are included or built. DSP and resampling features stay disabled.
