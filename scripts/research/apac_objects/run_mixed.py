#!/usr/bin/env python3
"""Exercise LFE-containing beds mixed with positional objects; decoder defaults stay intact."""

import argparse
import json
import os
from pathlib import Path
import shlex

from make_mixed_inputs import generate
from run_case import run_case
from run_experiments import environment
from verify_mixed import verify
from wrap_caf import wrap


def run(root, reference, name, objects=1, trajectory="left", bed_kind="center_lfe", explicit_bitrates=False):
    expected = generate(root / "inputs" / name, objects, trajectory, bed_kind, explicit_bitrates)
    trace = Path(__file__).resolve().parent / "trace_codec.py"
    debug = ["xcrun", "lldb", "--no-lldbinit", "--batch", "-o",
             f"command script import {shlex.quote(str(trace))}", "-o", "run", "--"]
    for key in ("APAC_PROFILE_CEILING", "APAC_BINARY_METADATA", "APAC_DECODER_METADATA", "APAC_TRACE_ERRORS", "APAC_TRACE_POSITIONS"):
        os.environ.pop(key, None)
    os.environ["APAC_TRACE_LFE"] = "1"
    binary = reference / "bin/codec_probe"
    encoded_dir = root / "cases" / (name + "-encode")
    encoded = run_case(debug + [str(binary), "encode", str(root / "inputs" / name / "settings.plist"),
                               str(root / "inputs" / name / "input.f32"), str(expected["input_channels"]),
                               str(expected["audio_channels"]), str(encoded_dir / "stream"), "0",
                               str(expected["total_bitrate"])], encoded_dir, 60)
    row = {"case": name, "objects": objects, "bed_kind": bed_kind, "audio_channels": expected["audio_channels"],
           "encode_returncode": encoded["returncode"], "encode_timed_out": encoded["timed_out"]}
    if encoded["returncode"] == 0:
        decoded_dir = root / "cases" / (name + "-decode")
        decoded = run_case(debug + [str(binary), "decode", str(encoded_dir / "stream"),
                                   str(decoded_dir / "decoded.f32"), str(expected["audio_channels"])], decoded_dir, 60)
        row.update(decode_returncode=decoded["returncode"], decode_timed_out=decoded["timed_out"])
    path = root / "mixed.json"
    rows = json.loads(path.read_text()) if path.exists() else []
    rows.append(row)
    path.write_text(json.dumps(rows, indent=2) + "\n")
    print(json.dumps(row), flush=True)
    return row


if __name__ == "__main__":
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--output", required=True, type=Path)
    parser.add_argument("--reference", required=True, type=Path)
    args = parser.parse_args()
    root, reference = args.output.resolve(), args.reference.resolve()
    root.mkdir(parents=True, exist_ok=False)
    (root / "environment.json").write_text(json.dumps(environment(), indent=2) + "\n")
    for name, objects, trajectory, bed in [
        ("mixed-left", 1, "left", "center_lfe"), ("mixed-right", 1, "right", "center_lfe"),
        ("mixed-moving", 1, "moving", "center_lfe"), ("mixed-control", 1, "left", "control"),
        ("mixed-24ch", 22, "spread", "center_lfe"), ("mixed-lfe-only", 1, "left", "lfe_only"),
    ]:
        run(root, reference, name, objects, trajectory, bed)
    for name, count, trajectory in [("mixed-lfe-explicit", 1, "left"),
                                    ("mixed-lfe-moving-explicit", 1, "moving"),
                                    ("mixed-23obj-lfe", 23, "spread")]:
        run(root, reference, name, count, trajectory, "lfe_only", explicit_bitrates=True)
    # Keep the same global target as the successful explicit case, but omit
    # per-component rates, to isolate the allocation setting.
    control = root / "cases/lfe-only-total-rate-control"
    run_case([str(reference / "bin/codec_probe"), "encode", str(root / "inputs/mixed-lfe-only/settings.plist"),
              str(root / "inputs/mixed-lfe-only/input.f32"), "3", "2", str(control / "stream"), "0", "272000"], control, 30)
    (root / "fixtures").mkdir()
    containers = []
    for name in ("mixed-left", "mixed-moving", "mixed-24ch", "mixed-lfe-explicit",
                 "mixed-lfe-moving-explicit", "mixed-23obj-lfe"):
        source = root / "cases" / (name + "-encode") / "stream"
        caf = root / "fixtures" / (name + ".caf")
        mp4 = caf.with_suffix(".mp4")
        wrap(source, caf)
        copied = run_case(["afconvert", str(caf), str(mp4), "-f", "mp4f", "-d", "0"], root / "cases" / (name + "-mp4-wrap"), 30)
        if copied["returncode"]:
            raise RuntimeError("MP4 packet-copy failed")
        for file in (caf, mp4):
            case = root / "cases" / (name + "-" + file.suffix[1:] + "-import")
            imported = run_case([str(reference / "bin/codec_probe"), "import", str(file), str(case / "stream")], case, 30)
            if imported["returncode"]:
                raise RuntimeError("native container readback failed")
            containers.append({"case": name, "container": file.suffix[1:], "returncode": 0,
                               "compressed_packets_identical": Path(str(source) + ".packets").read_bytes() == (case / "stream.packets").read_bytes(),
                               "magic_cookie_identical": Path(str(source) + ".cookie").read_bytes() == (case / "stream.cookie").read_bytes(),
                               "timing_matches": json.loads(Path(str(source) + ".timing.json").read_text()) == json.loads((case / "stream.timing.json").read_text())})
    (root / "containers.json").write_text(json.dumps(containers, indent=2) + "\n")
    (root / "verification.json").write_text(json.dumps(verify(root), indent=2) + "\n")
