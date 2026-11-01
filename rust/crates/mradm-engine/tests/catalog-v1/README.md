# catalog-v1

Frozen output of the C++ engine at main `28eac3f`, before PCM consistency 5.2 A moved the
discovery documents and the scene inspect JSON to `mradm-engine`. A scratchpad generator (not
committed) called the C ABI of a Linux x64 config A Release build through ctypes:

- `capabilities.json`, `layouts.json`, `output-formats.json`, `input-layouts.json`,
  `support-matrix.json`: `adm_capabilities_json`, `adm_layouts_json`, `adm_output_formats_json`,
  `adm_input_layouts_json` and `adm_render_support_matrix_json`, plus a newline. Linux facts:
  user SOFA on, no APAC, no IAMF, no Apple backend, no system-spatial layouts.
- `scenes.jsonl`: `adm_inspect_file_json` for the 30 inputs that `tests/catalog.rs` writes
  (`write_inputs`): the `mradm-scene` policy-v1 and export-v1 scenes as 480-frame BW64 files
  (two repeated as float32 and PCM16), the masked channel-bed presets, and rejected inputs. The
  input path in each document and message is replaced by `<input>`; rejected inputs record the
  error code and the C ABI's last error message.

The Apple, Windows and IAMF facts are island data; `tests/catalog.rs` covers how they reach the
documents.
