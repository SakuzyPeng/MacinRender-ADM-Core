#!/usr/bin/env python3
"""Run the 1/7/8/24 object matrix after the single-object acceptance checks pass."""

import argparse
import json
import os
from pathlib import Path
import subprocess
import shlex
import sys

from extract_positions import extract
from run_case import run_case


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--output", required=True, type=Path)
    args = parser.parse_args()
    root = args.output.resolve()
    scripts = Path(__file__).resolve().parent
    binary = root / "bin/codec_probe"
    trace = scripts / "trace_codec.py"
    results = []
    os.environ.pop("APAC_BINARY_METADATA", None)
    os.environ.pop("APAC_DECODER_METADATA", None)
    for count in (1, 7, 8, 24):
        fixture = root / "inputs" / f"matrix-{count}"
        subprocess.run([sys.executable, str(scripts / "make_inputs.py"), "--output", str(fixture),
                        "--objects", str(count), "--trajectory", "spread"], check=True)
        for ceiling in (None, 1, 2):
            name = f"matrix-{count}-" + ("default" if ceiling is None else f"ceiling{ceiling}")
            output = root / "cases" / name
            os.environ["APAC_TRACE_POSITIONS"] = "0"
            if ceiling is None:
                os.environ.pop("APAC_PROFILE_CEILING", None)
            else:
                os.environ["APAC_PROFILE_CEILING"] = str(ceiling)
            command = ["xcrun", "lldb", "--no-lldbinit", "--batch", "-o", f"command script import {shlex.quote(str(trace))}", "-o", "run", "--",
                       str(binary), "encode", str(fixture / "settings.plist"), str(fixture / "input.f32"),
                       str(count + 1), str(count), str(output / "stream"), "0", str(256000 * count)]
            encoded = run_case(command, output, 60)
            profiles = [event for event in encoded["events"] if "profile" in event]
            row = {"case": name, "objects": count, "diagnostic_ceiling": ceiling,
                   "encode_returncode": encoded["returncode"], "encode_timed_out": encoded["timed_out"],
                   "profile": profiles[-1] if profiles else None}
            # Every decoder runs with the original system capabilities, even for diagnostic samples.
            os.environ.pop("APAC_PROFILE_CEILING", None)
            os.environ.pop("APAC_TRACE_POSITIONS", None)
            if encoded["returncode"] == 0:
                decoded_output = root / "cases" / (name + "-decode")
                command = ["xcrun", "lldb", "--no-lldbinit", "--batch", "-o", f"command script import {shlex.quote(str(trace))}", "-o", "run", "--",
                           str(binary), "decode", str(output / "stream"), str(decoded_output / "decoded.f32"), str(count)]
                decoded = run_case(command, decoded_output, 60)
                row.update(decode_returncode=decoded["returncode"], decode_timed_out=decoded["timed_out"])
                if decoded["returncode"] == 0:
                    try:
                        positions = extract(output / "stream", decoded_output / "result.json", decoded_output / "decoded.f32",
                                            root / "derived" / name, count)
                        row["decoded_position_rows"] = len(positions)
                    except ValueError as error:
                        row["extraction_error"] = str(error)
            results.append(row)
            (root / "matrix.json").write_text(json.dumps(results, indent=2) + "\n")
            print(json.dumps(row), flush=True)


if __name__ == "__main__":
    main()
