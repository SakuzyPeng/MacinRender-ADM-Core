# request-v1

Frozen output of the C++ renderer factory and engine at main `6c406de`, before PCM consistency
5.2 B moved the request resolution to `mradm-engine` `request`. A scratchpad generator (not
committed) linked the C++ `ADMRendererFactory` and `ADMIo` of a Linux x64 config A Release build:

- `layouts.jsonl`: `normalize_output_layout` for 49 spellings.
- `renderers.jsonl`: `resolve_renderer` for every `RendererSelection` x the normalized layouts x the
  internal speaker-stereo switch (Apple unavailable): selection, backend, effective layout and
  diagnostics, or the error.
- `routing.jsonl`: `validate_direct_speakers_routing` for every backend x layout x routing mode.
- `matrix.jsonl`: `resolve_direct_speakers_matrix` for the 25 scenes `tests/request.rs` writes
  (`write_inputs`: policy-v1, export-v1, the four `scenes/` DirectSpeakers variants and the masked
  channel beds) x the 13 `matrices/` x routing mode x source (none, JSON, path, both, missing file)
  x layout: the log lines and the validated matrix (weight and gain as f32 bits) or the error;
  the matrices directory in an error context is `<dir>`.
- `general.txt`: fmt `{:.6g}` of special values, random bit patterns and rounded decimals (every
  fifth generated line kept), as `<f64 bits hex>\t<text>`.

`scenes/ds-lowpass-lfe` carries its lowPass frequency on the channel format, so the matrix
coverage skips that unlabelled LFE.
