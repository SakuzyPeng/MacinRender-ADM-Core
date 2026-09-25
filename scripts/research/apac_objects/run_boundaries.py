#!/usr/bin/env python3
"""Extend the APAC experiments across profile and per-component object boundaries."""

import argparse
import hashlib
import json
import os
from pathlib import Path
import shlex

from extract_positions import extract
from make_inputs import generate
from run_case import run_case
from run_experiments import environment
from verify_results import inspect


def run_boundary(root, reference, count, ceiling, groups=None, metadata_channels=None, frames=8192, encode_only=False):
    groups = groups or [count]
    suffix = "-".join(map(str, groups))
    name = f"objects-{count}-groups-{suffix}-" + ("default" if ceiling is None else f"ceiling{ceiling}")
    if metadata_channels is not None:
        name += f"-meta{metadata_channels}"
    source = root / "inputs" / name
    expected = generate(source, count, "spread", frames, groups, metadata_channels)
    binary = reference / "bin/codec_probe"
    trace = Path(__file__).resolve().parent / "trace_codec.py"
    debug = ["xcrun", "lldb", "--no-lldbinit", "--batch", "-o",
             f"command script import {shlex.quote(str(trace))}", "-o", "run", "--"]
    output = root / "cases" / name
    os.environ.pop("APAC_BINARY_METADATA", None)
    os.environ.pop("APAC_DECODER_METADATA", None)
    os.environ.pop("APAC_TRACE_ERRORS", None)
    if ceiling is None:
        os.environ.pop("APAC_PROFILE_CEILING", None)
    else:
        os.environ["APAC_PROFILE_CEILING"] = str(ceiling)
    os.environ["APAC_TRACE_POSITIONS"] = "0"
    encoded = run_case(debug + [str(binary), "encode", str(source / "settings.plist"), str(source / "input.f32"),
                               str(count + expected["metadata_channels"]), str(count), str(output / "stream"), "0",
                               str(256000 * min(count, 64))], output, 60)
    profiles = [event for event in encoded["events"] if "profile" in event]
    row = {"case": name, "objects": count, "component_groups": groups, "metadata_channels": expected["metadata_channels"],
           "frames": frames, "diagnostic_ceiling": ceiling, "encode_returncode": encoded["returncode"],
           "encode_timed_out": encoded["timed_out"], "encode_last_event": encoded["events"][-1:]}
    row["profile"] = profiles[-1] if profiles else None
    os.environ.pop("APAC_PROFILE_CEILING", None)
    os.environ.pop("APAC_TRACE_POSITIONS", None)
    if encoded["returncode"] == 0 and not encode_only:
        decoded_dir = root / "cases" / (name + "-decode")
        decoded = run_case(debug + [str(binary), "decode", str(output / "stream"), str(decoded_dir / "decoded.f32"), str(count)],
                           decoded_dir, 60)
        row.update(decode_returncode=decoded["returncode"], decode_timed_out=decoded["timed_out"],
                   decode_last_event=decoded["events"][-1:])
        if decoded["returncode"] == 0:
            try:
                extract(output / "stream", decoded_dir / "result.json", decoded_dir / "decoded.f32", root / "derived" / name, count)
                row["verification"] = inspect(root, name, name, count)
            except (ValueError, AssertionError) as error:
                row["verification_error"] = str(error)
    if encode_only:
        row["decode_validation"] = "not_run_encoder_capacity_probe"
    result_file = root / "boundaries.json"
    rows = json.loads(result_file.read_text()) if result_file.exists() else []
    rows.append(row)
    result_file.write_text(json.dumps(rows, indent=2) + "\n")
    print(json.dumps(row), flush=True)
    return row


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--output", type=Path, required=True)
    parser.add_argument("--reference", type=Path, required=True, help="existing complete run with bin/codec_probe")
    args = parser.parse_args()
    facts = environment()
    root, reference = args.output.resolve(), args.reference.resolve()
    facts["codec_probe_sha256"] = hashlib.sha256((reference / "bin/codec_probe").read_bytes()).hexdigest()
    root.mkdir(parents=True, exist_ok=False)
    (root / "environment.json").write_text(json.dumps(facts, indent=2) + "\n")
    schedule = [(32, 1, None), (33, 1, None), (33, 2, None), (64, 2, None), (65, 2, None),
                (127, 2, None), (128, 2, None), (128, 2, [64, 64]), (129, 2, [65, 64]),
                (85, 2, None), (86, 2, None), (126, 2, None), (14, None, [7, 7]),
                (64, 1, [32, 32]), (64, None, [7] * 9 + [1]),
                (70, None, [7] * 10), (71, None, [7] * 10 + [1])]
    for count, ceiling, groups in schedule:
        run_boundary(root, reference, count, ceiling, groups)
    run_boundary(root, reference, 32, 1, metadata_channels=2)
    for count, ceiling, groups in [(128, None, [7] * 18 + [2]), (252, None, [7] * 36),
                                   (129, None, [7] * 18 + [3]), (252, 2, [126, 126]),
                                   (214, 2, [127, 87]), (215, 2, [127, 88])]:
        run_boundary(root, reference, count, ceiling, groups, encode_only=True)


if __name__ == "__main__":
    main()
