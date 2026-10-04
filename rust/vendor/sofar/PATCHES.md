# Local sofar patch

Source: crates.io sofar 0.3.0. Archive SHA-256: 060ea9e290d57f909c19dfd5673a489cfedf4fdcf02eeae65a470a4923225be2.

Expose the existing `Hrtf::from_bytes` parser as `sofar::RawHrtf`. MacinRender owns convention validation and reads unmodified HRIR data; the high-level reader automatically converts coordinates and may normalize/resample. No C sources are included or built. DSP and resampling features stay disabled.

Replace the HDF5 B-tree reader's fixed chunk element-count limit with checked
dimension and byte-size multiplication, retaining the existing 256 MiB decoded
byte limit for each chunk. Invalid chunk dimensions return a parse error rather
than triggering a debug assertion. This preserves support for legal single-chunk
HRTF datasets; the workspace dataset tests cover equivalent single- and multi-chunk
SOFA fixtures.
