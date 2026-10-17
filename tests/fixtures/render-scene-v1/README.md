# Render-scene version 1 encoding fixtures

These are frozen little-endian word streams from the production C++
`mradm::scene_checkpoint::encode` in commit
`cb6e9a11486a2323f74c05e34e13f09ca6986039`, emitted by a temporary standalone
Apple Clang 21 C++20 Debug probe with `MR_ADM_CONSISTENCY_DIAGNOSTICS` enabled.
The probe only serializes metadata; it does not render audio. The Python validator
did not generate these fixtures. SHA-256 values are pinned in `phase2_tools_test.py`.

- `empty.i32`: 48 kHz, three channels, `0x100000003` frames, empty scene lists.
- `defaults.i32`: the same info with one default programme, content, object and HOA
  pack; the object has one track with an Objects block and a DirectSpeakers block,
  and the HOA pack has one channel and block. All optionals are absent.
- `populated.i32`: the same structure with every optional present, plus default
  blocks/tracks/channels to exercise repeated elements and absent optionals together.
  It includes Chinese and emoji UTF-8, empty and padded strings, negative zero,
  64-bit times, a negative HOA degree, screen/loudness metadata, authored gain/time
  provenance, user levels, position ranges and two import warnings (count only).

The tests reject truncation at every word boundary, extra words, invalid booleans
and presence flags, oversized lengths, malformed UTF-8 and nonzero padding.
