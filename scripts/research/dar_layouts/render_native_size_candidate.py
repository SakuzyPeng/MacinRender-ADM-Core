#!/usr/bin/env python3
"""Render a synthetic ADM through the Release-only private native size driver."""

import argparse
import json
import subprocess
import tempfile
from pathlib import Path

import numpy as np

from gain_trace_adm import inspect_adm
from measure_point_suite import file_sha256
from measure_static_bank import read_multichannel_adm

ROOT = Path(__file__).resolve().parents[3]
DRIVER = ROOT / "build/release/mr_adm_triple_balance_size_probe"


def render(adm_path, layout, output, block_frames=1024):
    if output.exists() or not 1 <= block_frames <= 65536:
        raise ValueError("output must be new and block size must be in [1,65536]")
    if "CMAKE_BUILD_TYPE:STRING=Release" not in (ROOT / "build/release/CMakeCache.txt").read_text():
        raise ValueError("candidate audio requires Release")
    identity = inspect_adm(adm_path)
    pcm = read_multichannel_adm(adm_path, identity["channels"], identity["frames"])
    object_channels = {item["input_channel"] for item in identity["objects"]}
    if any(np.any(pcm[:, i]) for i in range(identity["channels"]) if i not in object_channels):
        raise ValueError("research size driver requires silent bed channels")
    channels = 12 if layout == "7.1.4" else 16
    result = np.zeros((identity["frames"], channels), dtype=np.float32)
    output.parent.mkdir(parents=True, exist_ok=True)
    with tempfile.TemporaryDirectory(prefix="native-size-", dir=ROOT / "local") as directory:
        temporary = Path(directory)
        for obj in identity["objects"]:
            source = temporary / "input.f32"
            target = temporary / "output.f32"
            pcm[:, obj["input_channel"]].copy().tofile(source)
            events = [{"start_sample": item["start_sample"],
                       "xyz": [(item["xyz"][0] + 1) / 2, (1 - item["xyz"][1]) / 2, item["xyz"][2]],
                       "size": item["size"]} for item in obj["events"]]
            request = {"events": events, "layout": layout, "input_f32": str(source),
                       "output_f32": str(target), "block_frames": block_frames}
            path = temporary / "request.json"
            path.write_text(json.dumps(request) + "\n")
            subprocess.run([str(DRIVER), str(path)], check=True)
            rendered = np.fromfile(target, dtype="<f4").reshape(-1, channels)
            if rendered.shape != result.shape or not np.isfinite(rendered).all():
                raise ValueError("invalid native size output length/values")
            result += rendered
        if layout == "7.1.4":
            result = result[:, [0, 1, 2, 3, 6, 7, 4, 5, 8, 9, 10, 11]].copy()
        raw = temporary / "mixed.f32"
        result.tofile(raw)
        subprocess.run(["ffmpeg", "-v", "error", "-f", "f32le", "-ar", "48000", "-ac", str(channels),
                        "-i", str(raw), "-c:a", "pcm_f32le", str(output)], check=True)
    return {"adm_sha256": identity["sha256"], "pcm_sha256": identity["pcm_sha256"],
            "candidate": str(output.resolve()), "candidate_sha256": file_sha256(output),
            "driver_sha256": file_sha256(DRIVER), "layout": layout, "build": "Release", "block_frames": block_frames}


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--adm", type=Path, required=True)
    parser.add_argument("--layout", choices=("7.1.4", "9.1.6"), required=True)
    parser.add_argument("--output", type=Path, required=True)
    parser.add_argument("--block-frames", type=int, default=1024)
    args = parser.parse_args()
    result = render(args.adm, args.layout, args.output, args.block_frames)
    args.output.with_suffix(".json").write_text(json.dumps(result, indent=2) + "\n")
    print(args.output)


if __name__ == "__main__":
    main()
