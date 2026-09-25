#!/usr/bin/env python3
"""Reproduce the native APAC object experiments in a new output directory."""

import argparse
import hashlib
import json
import os
from pathlib import Path
import plistlib
import struct
import subprocess
import shlex
import sys

import numpy as np

from extract_positions import extract
from make_inputs import settings
from run_case import run_case
from verify_results import inspect, verify
from wrap_caf import wrap

SCRIPTS = Path(__file__).resolve().parent


def environment():
    component = Path("/System/Library/Components/AudioCodecs.component/Contents/MacOS/AudioCodecs")
    with component.open("rb") as file:
        magic, count = struct.unpack(">II", file.read(8))
        if magic != 0xCAFEBABE:
            raise ValueError("unsupported component container")
        slices = [struct.unpack(">IIIII", file.read(20)) for _ in range(count)]
        entry = next(item for item in slices if item[:2] == (0x0100000C, 0x80000002))
        file.seek(entry[2])
        digest = hashlib.sha256(file.read(entry[3])).hexdigest()
    if digest != "82fb858cdbcf9146b1740a5cccd25dbf843ab9fbcdb88e05bbbbaed3e8e9cdd2":
        raise ValueError("AudioCodecs changed; re-establish the private ABI before running")
    return {"component": str(component), "slice_sha256": digest,
            "macos": subprocess.check_output(["sw_vers", "-productVersion"], text=True).strip(),
            "build": subprocess.check_output(["sw_vers", "-buildVersion"], text=True).strip(),
            "python": sys.version, "numpy": np.__version__}


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--output", type=Path, required=True, help="new directory; existing output is never overwritten")
    args = parser.parse_args()
    facts = environment()
    root = args.output.resolve()
    root.mkdir(parents=True, exist_ok=False)
    for directory in ("bin", "inputs", "fixtures", "derived"):
        (root / directory).mkdir()
    (root / "environment.json").write_text(json.dumps(facts, indent=2) + "\n")
    for key in ("APAC_PROFILE_CEILING", "APAC_TRACE_POSITIONS", "APAC_BINARY_METADATA", "APAC_DECODER_METADATA"):
        os.environ.pop(key, None)

    def run(name, command, debugger=False, allow_failure=False):
        result = run_case([str(item) for item in command], root / "cases" / name, 60 if debugger else 30)
        print(json.dumps({"case": name, "returncode": result["returncode"], "timed_out": result["timed_out"]}), flush=True)
        if not allow_failure and result["returncode"]:
            raise RuntimeError(f"{name} failed; see its result.json and stderr.txt")
        return result

    def traced(command):
        return ["xcrun", "lldb", "--no-lldbinit", "--batch", "-o",
                f"command script import {shlex.quote(str(SCRIPTS / 'trace_codec.py'))}", "-o", "run", "--", *command]

    codec, renderer, reader = (root / "bin" / name for name in ("codec_probe", "render_decoded", "read_asset"))
    run("build-codec", ["xcrun", "clang++", "-std=c++20", "-O2", "-g", "-Wno-deprecated-declarations",
                        SCRIPTS / "codec_probe.cpp", "-framework", "AudioToolbox", "-framework", "CoreFoundation", "-o", codec])
    run("build-renderer", ["xcrun", "clang++", "-std=c++20", "-O2", "-g", "-Wno-deprecated-declarations",
                           SCRIPTS / "render_decoded.cpp", "-framework", "AudioToolbox", "-o", renderer])
    run("build-reader", ["xcrun", "clang", "-O2", "-g", "-fobjc-arc", "-Wno-deprecated-declarations",
                         SCRIPTS / "read_asset.m", "-framework", "Foundation", "-framework", "AVFoundation",
                         "-framework", "AudioToolbox", "-framework", "CoreMedia", "-o", reader])

    rendered = []
    for name, trajectory in (("left1", "left"), ("right1", "right"), ("moving1", "moving")):
        source = root / "inputs" / name
        subprocess.run([sys.executable, str(SCRIPTS / "make_inputs.py"), "--output", str(source), "--trajectory", trajectory], check=True)
        encoded = root / "cases" / (name + "-final-encode") / "stream"
        run(name + "-final-encode", [codec, "encode", source / "settings.plist", source / "input.f32", 2, 1, encoded, 0, 256000])
        decoded = root / "cases" / (name + "-final-decode")
        run(name + "-final-decode", traced([codec, "decode", encoded, decoded / "decoded.f32", 1]), debugger=True)
        derived = root / "derived" / name
        extract(encoded, decoded / "result.json", decoded / "decoded.f32", derived, 1)
        run(name + "-render", [renderer, derived / "audio.f32", derived / "positions.csv", 1, derived / "binaural.f32"])
        checked = inspect(root, name, name, 1)
        pcm = np.fromfile(derived / "binaural.f32", dtype="<f4").reshape(-1, 2).astype("float64")
        windows = {}
        for label, begin, end in (("early", .1, .4), ("late", len(pcm) / 48000 - .4, len(pcm) / 48000 - .1)):
            rms = np.sqrt(np.mean(pcm[int(begin * 48000):int(end * 48000)] ** 2, axis=0))
            windows[label] = {"rms": rms.tolist(), "left_minus_right_db": float(20 * np.log10(rms[0] / rms[1]))}
        rendered.append({"case": name, "position_rows": checked["position_rows"],
                         "max_azimuth_error_degrees": checked["max_azimuth_error_degrees"], "windows": windows})
    (root / "single_object_summary.json").write_text(json.dumps(rendered, indent=2) + "\n")
    subprocess.run([sys.executable, str(SCRIPTS / "run_matrix.py"), "--output", str(root)], check=True)

    mono = np.fromfile(root / "inputs/left1/input.f32", dtype="<f4").reshape(-1, 2)[:, 0]
    mono.tofile(root / "inputs/mono.f32")
    np.column_stack((mono, np.zeros_like(mono))).astype("<f4").tofile(root / "inputs/object-empty.f32")
    negatives = []
    for name in ("missing-metadata", "empty-metadata", "wrong-object-range"):
        absent = name == "missing-metadata"
        config = settings(1, 0 if absent else 1)
        if name == "wrong-object-range":
            config["parameters"][0]["ASComponents"][0]["Object"][1]["current maximum range"] = 1
        setting = root / "inputs" / (name + ".plist")
        setting.write_bytes(plistlib.dumps(config))
        encoded = root / "cases" / ("negative-" + name) / "stream"
        result = run("negative-" + name, [codec, "encode", setting, root / "inputs" / ("mono.f32" if absent else "object-empty.f32"),
                                         1 if absent else 2, 1, encoded, 0, 256000], allow_failure=True)
        negatives.append({"case": name, "returncode": result["returncode"], "tail_events": result["events"][-3:]})
        if name != "wrong-object-range":
            target = root / "cases" / ("negative-" + name + "-decode")
            run("negative-" + name + "-decode", traced([codec, "decode", encoded, target / "decoded.f32", 1]), debugger=True)
    (root / "negative_cases.json").write_text(json.dumps(negatives, indent=2) + "\n")

    custom = root / "cases/custom-ancillary-control/stream"
    run("custom-ancillary-control", traced([codec, "encode", root / "inputs/empty-metadata.plist",
        root / "inputs/object-empty.f32", 2, 1, custom, 1, 256000]), debugger=True, allow_failure=True)
    binary = root / "cases/binary-reader-mdpf-trace/stream"
    os.environ["APAC_BINARY_METADATA"] = "1"
    try:
        result = run("binary-reader-mdpf-trace", traced([codec, "encode", root / "inputs/missing-metadata.plist",
            root / "inputs/mono.f32", 1, 1, binary, 0, 256000]), debugger=True)
    finally:
        os.environ.pop("APAC_BINARY_METADATA", None)
    (root / "binary_reader_summary.json").write_text(json.dumps({
        "property": "mdpf", "value": 1, "input_metadata_channels": 0, "profile_table_modified": False,
        "returncode": result["returncode"], "reader_observed": any("BinaryMetadataReader::GetMetadata" in event.get("function", "") for event in result["events"]),
        "payload_supplied": False, "position_roundtrip_validated": False,
    }, indent=2) + "\n")

    containers, assets = [], []
    for name in ("left1", "right1", "moving1", "matrix-7-default", "matrix-8-ceiling1", "matrix-24-ceiling1"):
        encoded = root / "cases" / (name if name.startswith("matrix") else name + "-final-encode") / "stream"
        caf, mp4 = root / "fixtures" / (name + ".caf"), root / "fixtures" / (name + ".mp4")
        wrap(encoded, caf)
        run(name + "-mp4-wrap", ["afconvert", caf, mp4, "-f", "mp4f", "-d", "0"])
        for path in (caf, mp4):
            case = name + "-" + path.suffix[1:]
            imported = root / "cases" / (case + "-import") / "stream"
            result = run(case + "-import", [codec, "import", path, imported])
            containers.append({"case": name, "container": path.suffix[1:], "import_returncode": result["returncode"],
                               "compressed_packets_identical": Path(str(encoded) + ".packets").read_bytes() == Path(str(imported) + ".packets").read_bytes(),
                               "timing": json.loads(Path(str(imported) + ".timing.json").read_text())})
            result = run(case + "-asset-read", [reader, path], allow_failure=True)
            assets.append({"file": path.name, "returncode": result["returncode"], "events": result["events"]})
    (root / "containers.json").write_text(json.dumps(containers, indent=2) + "\n")
    (root / "asset_reads.json").write_text(json.dumps(assets, indent=2) + "\n")
    (root / "verification.json").write_text(json.dumps(verify(root), indent=2) + "\n")


if __name__ == "__main__":
    main()
