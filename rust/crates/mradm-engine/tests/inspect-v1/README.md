# inspect-v1

Frozen `mradm inspect` text of the C++ CLI at main `eb17077`, before PCM consistency 5.6 C moved
the summary to `mradm-engine` (`scene_json::inspect_text`). A Linux x64 debug build of `mradm`
ran `mradm inspect <input>` on the 31 inputs that `tests/catalog.rs` writes: the 30 inputs of
`catalog-v1` (`write_inputs`) and `rich`, a 480-frame PCM24 BW64 file from `rich.xml` and
`rich.json`.

`rich` reaches the branches the policy scenes do not: programme language, labels, start and end,
a reference screen and every loudness field; content language, labels, the three dialogue kinds
with and without a content kind; object labels, importance and dialogue; Cartesian and polar
blocks with extent and diffuse, values on rounding ties; DirectSpeakers with several labels and
an LFE channel; and a track UID without a CHNA entry (`ch=<unmapped>`).

`scenes.jsonl` holds one record per input in replay order: `text` is the standard output after
the `File: <input>` line (which the C++ caller still writes), or `null` when the CLI rejected the
input (the error codes are in `catalog-v1/scenes.jsonl`).
