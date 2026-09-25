#!/usr/bin/env python3
"""Reproduce grouped-object loss in public AVFoundation stereo conversion.

The fixtures have the real 7.1.2 BED labels and 32 independently identifiable
object tones. This checks audibility in generic stereo fold-down, not spatial
rendering, and does not change decoder capabilities or private properties.
"""

import argparse
import json
from pathlib import Path
import plistlib

import numpy as np

from make_adm_input import BED, require
from run_case import run_case
from run_mixed_capacity import generate
from wrap_caf import wrap


def run(root, codec, reader):
    root.mkdir(parents=True, exist_ok=False)
    rows = []
    for name, groups in (("groups7", [7, 7, 7, 7, 4]), ("groups1", [1] * 32)):
        source = root / (name + "-input")
        expected = generate(source, [10], groups, frames=49152)
        # The capacity fixture's first ten labels use height-front positions.
        # Use the actual ADM trial's standard Atmos 7.1.2 labels here.
        settings_path = source / "settings.plist"
        config = plistlib.loads(settings_path.read_bytes())
        for side in config["parameters"][:2]:
            label = next(item for item in side["ASComponents"][0]["Channel Bed"]
                         if item["key"] == "ChannelLayoutLabel")
            label["current value"] = ["kAudioChannelLabel_" + row[1] for row in BED]
        settings_path.write_bytes(plistlib.dumps(config))
        expected["bed_labels"] = [row[2] for row in BED]
        (source / "expected.json").write_text(json.dumps(expected, indent=2) + "\n")

        def invoke(case, command):
            result = run_case([str(value) for value in command], root / case, 30)
            require(result["returncode"] == 0 and not result["timed_out"], case + " failed")
            return result

        prefix = root / (name + "-encode") / "stream"
        invoke(name + "-encode", [codec, "encode", settings_path, source / "input.f32", 43, 42,
                                  prefix, 0, expected["total_bitrate"]])
        caf, mp4 = root / (name + ".caf"), root / (name + ".mp4")
        wrap(prefix, caf)
        invoke(name + "-wrap", ["afconvert", caf, mp4, "-f", "mp4f", "-d", "0"])
        output = root / (name + "-stereo") / "audio.f32"
        invoke(name + "-stereo", [reader, mp4, output, 2])
        pcm = np.fromfile(output, dtype="<f4").reshape(-1, 2).astype(np.float64)
        require(len(pcm) == expected["frames"] and np.isfinite(pcm).all(), "invalid stereo output")
        amplitudes = 2 * np.abs(np.fft.rfft(pcm, axis=0)) / len(pcm)
        bins = [round(hz * len(pcm) / 48000) for hz in expected["audio_frequencies"]]
        objects = amplitudes[bins][10:]
        audible = [i for i, value in enumerate(objects) if np.linalg.norm(value) > .001]
        wanted = [0, 7, 14, 21, 28] if name == "groups7" else list(range(32))
        require(audible == wanted, "stereo behavior differs from the documented boundary")
        if name == "groups1":
            require(np.max(np.abs(objects - .04)) < .0001, "unexpected object fold-down gain")
        row = {"case": name, "bed_labels": expected["bed_labels"], "objects": 32,
               "audible_objects": len(audible), "audible_object_indices": audible,
               "object_tone_amplitudes": objects.tolist(), "all_expected_frames": True,
               "decoder_modified": False, "direct_quicktime_capture": False,
               "spatial_rendering_validated": False}
        rows.append(row)
        print(json.dumps({key: row[key] for key in ("case", "audible_objects", "audible_object_indices")}), flush=True)
    (root / "verification.json").write_text(json.dumps(rows, indent=2) + "\n")


if __name__ == "__main__":
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("output", type=Path)
    parser.add_argument("--codec", required=True, type=Path)
    parser.add_argument("--reader", required=True, type=Path, help="read_asset built from the extended current source")
    args = parser.parse_args()
    run(args.output.resolve(), args.codec.resolve(), args.reader.resolve())
