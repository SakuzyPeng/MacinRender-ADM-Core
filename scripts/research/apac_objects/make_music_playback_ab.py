#!/usr/bin/env python3
"""Sequence an existing short staged ADM excerpt as full, BED, then objects.

Keep its complete 42-channel scene and metadata in every passage; only the audio
mask changes. This is a local diagnostic derivative, never a replacement master.
"""

import argparse
import hashlib
import json
from pathlib import Path

import numpy as np

from run_case import run_case
from wrap_caf import wrap


def make(source, pcm_path, output, codec, vocals=False):
    output.mkdir(parents=True, exist_ok=False)
    expected = json.loads((source / "expected.json").read_text())
    if expected["input_channels"] != 43 or expected["metadata_channels"] != 1:
        raise ValueError("this diagnostic requires the known 42-channel scene")
    x = np.fromfile(pcm_path, dtype="<f4").reshape(-1, 43)
    if len(x) != expected["staged_frames"] or len(x) % 1024:
        raise ValueError("staged input does not match its expected frame count")
    gap = np.zeros((49152, 43), dtype="<f4")
    # The source is static. Carry its first metadata frame throughout each gap.
    gap[:, 42] = np.tile(x[:1024, 42], len(gap) // 1024)
    passages, chunks, cursor = [], [gap], len(gap)
    masks = (("full", 0, 42), ("bed_only", 0, 10), ("vocals_only", 10, 18) if vocals else ("objects_only", 10, 42))
    for name, first, last in masks:
        segment = x.copy()
        segment[:, :first] = 0
        segment[:, last:42] = 0
        # Equal short fades suppress splice clicks in all three derivatives.
        fade = 2400
        envelope = np.ones(len(segment), dtype=np.float32)
        ramp = .5 - .5 * np.cos(np.linspace(0, np.pi, fade))
        envelope[:fade], envelope[-fade:] = ramp, ramp[::-1]
        segment[:, :42] *= envelope[:, None]
        passages.append({"name": name, "start": cursor / 48000, "end": (cursor + len(segment)) / 48000,
                         "source_start_sample": expected["start_sample"], "source_frames": expected["frames"]})
        chunks += [segment, gap]
        cursor += len(segment) + len(gap)
    data = np.concatenate(chunks)
    data.tofile(output / "input.f32")
    (output / "settings.plist").write_bytes((source / "settings.plist").read_bytes())
    description = {"sample_rate": 48000, "frames": len(data), "input_channels": 43,
                   "source": expected["source"], "source_excerpt_sha256": hashlib.sha256(pcm_path.read_bytes()).hexdigest(),
                   "passages": passages, "audio_fade_samples": 2400,
                   "metadata_change": "none; original pos-diffuse semantics, including omitted extents",
                   "component_groups": expected["component_groups"], "total_bitrate": expected["total_bitrate"]}
    (output / "expected.json").write_text(json.dumps(description, indent=2) + "\n")
    prefix = output / "encode/stream"
    result = run_case([str(codec), "encode", str(output / "settings.plist"), str(output / "input.f32"),
                       "43", "42", str(prefix), "0", str(expected["total_bitrate"])], output / "encode", 30)
    if result["returncode"]:
        raise RuntimeError("encoding failed")
    stem = "music-full-bed-vocals" if vocals else "music-full-bed-objects"
    caf, mp4 = output / (stem + ".caf"), output / (stem + ".mp4")
    wrap(prefix, caf)
    result = run_case(["afconvert", str(caf), str(mp4), "-f", "mp4f", "-d", "0"], output / "wrap", 30)
    if result["returncode"]:
        raise RuntimeError("wrapping failed")
    print(json.dumps(description, indent=2))


if __name__ == "__main__":
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("source", type=Path)
    parser.add_argument("pcm", type=Path)
    parser.add_argument("output", type=Path)
    parser.add_argument("--codec", required=True, type=Path)
    parser.add_argument("--vocals", action="store_true", help="last passage isolates source channels 10..17 (VOCAL/BV)")
    args = parser.parse_args()
    make(args.source.resolve(), args.pcm.resolve(), args.output.resolve(), args.codec.resolve(), args.vocals)
