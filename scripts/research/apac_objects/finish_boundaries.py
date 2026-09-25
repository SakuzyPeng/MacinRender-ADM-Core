#!/usr/bin/env python3
"""Add read-only decoder capacity and container checks to a fresh boundary run."""

import argparse
import json
import os
from pathlib import Path
import plistlib
import shlex

import numpy as np

from make_inputs import settings
from run_case import run_case
from verify_boundaries import verify
from wrap_caf import wrap


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--output", type=Path, required=True)
    parser.add_argument("--reference", type=Path, required=True)
    args = parser.parse_args()
    root, reference = args.output.resolve(), args.reference.resolve()
    scripts = Path(__file__).resolve().parent
    for key in ("APAC_PROFILE_CEILING", "APAC_BINARY_METADATA", "APAC_DECODER_METADATA", "APAC_TRACE_ERRORS"):
        os.environ.pop(key, None)
    rows = json.loads((root / "boundaries.json").read_text())
    (root / "bin").mkdir(exist_ok=True)
    (root / "fixtures").mkdir(exist_ok=True)
    reader = root / "bin/read_asset"
    built = run_case(["xcrun", "clang", "-O2", "-g", "-fobjc-arc", "-Wno-deprecated-declarations",
                      str(scripts / "read_asset.m"), "-framework", "Foundation", "-framework", "AVFoundation",
                      "-framework", "AudioToolbox", "-framework", "CoreMedia", "-o", str(reader)], root / "cases/build-reader", 30)
    if built["returncode"]:
        raise RuntimeError("reader build failed")
    capacities, containers = [], []
    for count in (70, 71):
        row = next(item for item in rows if item["objects"] == count)
        name = f"default-decoder-capacity-{count}"
        os.environ["APAC_TRACE_ERRORS"] = "1"
        os.environ["APAC_TRACE_POSITIONS"] = "0"
        command = ["xcrun", "lldb", "--no-lldbinit", "--batch", "-o",
                   f"command script import {shlex.quote(str(scripts / 'trace_codec.py'))}", "-o", "run", "--",
                   str(reference / "bin/codec_probe"), "decode", str(root / "cases" / row["case"] / "stream"),
                   str(root / "cases" / name / "decoded.f32"), str(count)]
        result = run_case(command, root / "cases" / name, 60)
        capacities.append({"objects": count, "returncode": result["returncode"], "decoder_modified": False,
                           "observations": [event for event in result["events"] if event.get("stage") == "metadata_sink_capacity"]})
        os.environ.pop("APAC_TRACE_ERRORS", None)
        os.environ.pop("APAC_TRACE_POSITIONS", None)
        caf = root / "fixtures" / f"default-objects-{count}.caf"
        mp4 = caf.with_suffix(".mp4")
        wrap(root / "cases" / row["case"] / "stream", caf)
        copied = run_case(["afconvert", str(caf), str(mp4), "-f", "mp4f", "-d", "0"],
                          root / "cases" / f"default-objects-{count}-mp4-wrap", 30)
        if copied["returncode"]:
            raise RuntimeError("MP4 packet-copy failed")
        for file in (caf, mp4):
            result = run_case([str(reader), str(file)], root / "cases" / (file.stem + "-" + file.suffix[1:] + "-asset"), 30)
            event = result["events"][-1] if result["events"] else {}
            containers.append({"objects": count, "file": file.name, "returncode": result["returncode"],
                               "event": event, "complete_frame_count": event.get("frames") == 8192 and event.get("channels") == count})
    (root / "decoder_capacity.json").write_text(json.dumps(capacities, indent=2) + "\n")
    (root / "container_boundary_reads.json").write_text(json.dumps(containers, indent=2) + "\n")

    case = root / "cases/asset70-pcm-check"
    result = run_case([str(reader), str(root / "fixtures/default-objects-70.caf"), str(case / "audio.f32")], case, 30)
    if result["returncode"]:
        raise RuntimeError("AVAssetReader PCM check failed")
    pcm = np.fromfile(case / "audio.f32", dtype="<f4").reshape(-1, 70).astype("float64")
    source = next(row for row in rows if row["objects"] == 70)
    expected = json.loads((root / "inputs" / source["case"] / "expected.json").read_text())
    spectrum = np.abs(np.fft.rfft(pcm, axis=0)) ** 2
    bins = [round(f * len(pcm) / 48000) for f in expected["audio_frequencies"]]
    fractions = [float(spectrum[bin_index, i] / spectrum[:, i].sum()) for i, bin_index in enumerate(bins)]
    semantics = {"returncode": result["returncode"], "frames": len(pcm),
                 "dominant_tones_match": np.argmax(spectrum, axis=0).tolist() == bins,
                 "minimum_expected_tone_energy_fraction": min(fractions),
                 "per_channel_rms": np.sqrt(np.mean(pcm * pcm, axis=0)).tolist(),
                 "note": "AVAssetReader output is checked separately from the native AudioCodec object stream."}
    (root / "asset_pcm_semantics.json").write_text(json.dumps(semantics, indent=2) + "\n")

    name = "objects-253-channel-map-edge"
    source = root / "inputs" / name
    source.mkdir()
    (source / "input.f32").write_bytes(b"")
    (source / "settings.plist").write_bytes(plistlib.dumps(settings(253, 3, [127, 126])))
    result = run_case([str(reference / "bin/codec_probe"), "encode", str(source / "settings.plist"),
                       str(source / "input.f32"), "256", "253", str(root / "cases" / name / "stream"), "0", "16384000"],
                      root / "cases" / name, 30)
    mapping = {"case": name, "purpose": "configuration-only invalid channel-map endpoint", "objects": 253,
               "input_channels": 256, "metadata_channels": 3, "encode_returncode": result["returncode"],
               "last_event": result["events"][-1:]}
    (root / "channel_map_boundary.json").write_text(json.dumps(mapping, indent=2) + "\n")
    (root / "verification.json").write_text(json.dumps(verify(root), indent=2) + "\n")
    print(json.dumps({"observations_verified": True, "decoder_policy": "system defaults"}))


if __name__ == "__main__":
    main()
