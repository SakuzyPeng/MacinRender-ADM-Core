#!/usr/bin/env python3
"""Prove Dolby multi-mono channel names against the same ADM's interleaved WAV."""

import argparse
import json
import re
import subprocess
from pathlib import Path

import numpy as np


def decode(path: Path, channels: int) -> np.ndarray:
    result = subprocess.run(
        ["ffmpeg", "-v", "error", "-i", str(path), "-f", "f32le", "-acodec", "pcm_f32le", "-"],
        check=True, capture_output=True,
    )
    samples = np.frombuffer(result.stdout, dtype="<f4")
    if samples.size % channels:
        raise ValueError(f"incomplete frames: {path}")
    return samples.reshape(-1, channels)


def main() -> None:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--interleaved-run", type=Path, required=True)
    parser.add_argument("--mono-run", type=Path, required=True)
    parser.add_argument("--output", type=Path, required=True)
    args = parser.parse_args()
    if args.output.exists():
        parser.error(f"output exists: {args.output}")
    interleaved = json.loads(args.interleaved_run.read_text())
    mono = json.loads(args.mono_run.read_text())
    if not interleaved["success"] or not mono["success"]:
        raise ValueError("both export runs must succeed")
    if interleaved["adm"] != mono["adm"]:
        raise ValueError("exports must read the same ADM")
    if interleaved["file_type"] != "interleaved" or mono["file_type"] != "multi-mono":
        raise ValueError("wrong export file types")
    report = {"adm": interleaved["adm"], "layouts": {}}
    for artifact in interleaved["outputs"]:
        layout = artifact["layout"]
        mono_artifact = next(item for item in mono["outputs"] if item["layout"] == layout)
        channels = artifact["channels"]
        inter = decode(Path(artifact["path"]), channels)
        mono_files = mono_artifact["files"]
        if len(mono_files) != channels:
            raise ValueError(f"{layout}: expected {channels} mono files")
        named = []
        for file in mono_files:
            match = re.search(r"_(\d{2})\.([^.]+)\.wav$", Path(file["path"]).name)
            if not match:
                raise ValueError(f"missing numbered channel label in {file['path']}")
            index = int(match.group(1)) - 1
            named.append((index, match.group(2), decode(Path(file["path"]), 1)[:, 0]))
        named.sort(key=lambda value: value[0])
        if [entry[0] for entry in named] != list(range(channels)):
            raise ValueError(f"{layout}: duplicate or missing mono channel number")
        if any(len(entry[2]) != len(inter) for entry in named):
            raise ValueError(f"{layout}: mono and interleaved frame counts differ")
        mapping = []
        for inter_index in range(channels):
            matches = [(mono_index, label) for mono_index, label, signal in named
                       if np.array_equal(inter[:, inter_index], signal)]
            if len(matches) != 1:
                raise ValueError(f"{layout} interleaved channel {inter_index}: {len(matches)} exact matches")
            mono_index, label = matches[0]
            mapping.append({"interleaved_index": inter_index, "mono_index": mono_index, "label": label})
        if sorted(item["mono_index"] for item in mapping) != list(range(channels)):
            raise ValueError(f"{layout}: mapping is not a permutation")
        report["layouts"][layout] = {"channels": channels, "frames": len(inter),
                                     "interleaved_to_mono": mapping, "all_samples_exact": True}
    args.output.write_text(json.dumps(report, indent=2) + "\n", encoding="utf-8")
    print(args.output)


if __name__ == "__main__":
    main()
