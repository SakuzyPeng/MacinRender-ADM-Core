# Local sofar patch

Source: crates.io sofar 0.3.0. Archive SHA-256: 060ea9e290d57f909c19dfd5673a489cfedf4fdcf02eeae65a470a4923225be2.

Expose the existing `Hrtf::from_bytes` parser as `sofar::RawHrtf`. MacinRender owns convention validation and reads unmodified HRIR data; the high-level reader automatically converts coordinates and may normalize/resample. No C sources are included or built. DSP and resampling features stay disabled.

Replace the HDF5 B-tree reader's fixed chunk element-count limit with checked
dimension and byte-size multiplication, retaining the existing 256 MiB decoded
byte limit for each chunk. Invalid chunk dimensions return a parse error rather
than triggering a debug assertion. This preserves support for legal single-chunk
HRTF datasets; the workspace dataset tests cover equivalent single- and multi-chunk
SOFA fixtures.

Defer dataset decoding until the entire object header has been collected, so the
filter pipeline and dataspace are available regardless of message order. Honor
deflate and shuffle independently, including unfiltered chunks and per-chunk
filter masks; reject unknown filters, unsupported orderings and mismatched shuffle
widths. This prevents gzip-only data from being silently unshuffled. The workspace
room SOFA tests compare exact samples across equivalent storage variants and
reject an unsupported checksum filter. Filter metadata follows the
[HDF5 file format specification](https://support.hdfgroup.org/documentation/hdf5/latest/_f_m_t2.html).

Bound fractal-heap direct-block decoding by its doubling-table block size.
A fully occupied direct block has no trailing zero sentinel; previously the
reader continued into unrelated file bytes and rejected a valid SOFA. Root
direct blocks use the starting size; indirect entries use the starting size
for the first two rows, doubling thereafter. The synthetic `full-heap.sofa`
fixture reproduces the boundary with anonymous metadata and six impulse nodes.
