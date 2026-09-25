#!/usr/bin/env python3
"""Isolate eight source VOCAL/BV channels; compare original diffuse with zero.

The zero-diffuse file is an explicitly changed diagnostic, not an ADM-faithful
deliverable. Both retain the 42-audio-channel layout, positions and source gain.
"""

import argparse
import copy
import json
from pathlib import Path
import wave

import numpy as np

from make_adm_input import metadata
from run_case import run_case
from wrap_caf import wrap


def make(source, output, codec):
    output.mkdir(parents=True, exist_ok=False)
    expected = json.loads((source / "expected.json").read_text())
    source_pcm = np.fromfile(source / "input.f32", dtype="<f4").reshape(-1, 43)
    for mode in ("original", "diagnostic-diffuse0"):
        case = output / mode
        case.mkdir()
        x = source_pcm.copy()
        x[:, :10], x[:, 18:42] = 0, 0
        if mode != "original":
            tracks = copy.deepcopy(expected["tracks"][10:])
            for track in tracks:
                track["width"] = track["height"] = track["depth"] = 0
                if track["channel"] < 18:
                    track["diffuse"] = 0
            blob = metadata(tracks)
            words = np.frombuffer(blob + b"\0" * (len(blob) % 2), dtype="<i2").astype("<f4") / 32768
            x[:, 42] = 0
            for start in range(0, len(x), 1024):
                x[start:start + len(words), 42] = words
        x.tofile(case / "input.f32")
        (case / "settings.plist").write_bytes((source / "settings.plist").read_bytes())
        info = {"mode": mode, "source": str(source.resolve()), "start_sample": expected["start_sample"],
                "frames": len(x), "active_channels": list(range(10, 18)), "original_gain": True,
                "diffuse": 1 if mode == "original" else 0, "extents": "omitted in both",
                "source_channel_rms": np.sqrt(np.mean(x[:, 10:18].astype(float)**2, axis=0)).tolist()}
        (case / "expected.json").write_text(json.dumps(info, indent=2) + "\n")
        prefix = case / "encode/stream"
        result = run_case([str(codec), "encode", str(case / "settings.plist"), str(case / "input.f32"),
                           "43", "42", str(prefix), "0", "12000000"], case / "encode", 30)
        if result["returncode"]:
            raise RuntimeError("encode failed")
        caf, mp4 = case / "vocals.caf", case / "vocals.mp4"
        wrap(prefix, caf)
        result = run_case(["afconvert", str(caf), str(mp4), "-f", "mp4f", "-d", "0"], case / "wrap", 30)
        if result["returncode"]:
            raise RuntimeError("wrap failed")
    # A source-only listening reference, without APAC or rendering. It is a sum
    # of the four named left/right pairs, not a spatial reference renderer.
    stereo = np.column_stack((source_pcm[:, 10:18:2].sum(axis=1), source_pcm[:, 11:18:2].sum(axis=1)))
    if np.max(np.abs(stereo)) >= 1:
        raise ValueError("source reference would clip")
    samples = np.rint(stereo * 8388608).astype("<i4")
    with wave.open(str(output / "source-vocal-pairs-unrendered.wav"), "wb") as wav:
        wav.setnchannels(2)
        wav.setsampwidth(3)
        wav.setframerate(48000)
        wav.writeframes(samples.view(np.uint8).reshape(-1, 4)[:, :3].tobytes())
    print(output)


if __name__ == "__main__":
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("source", type=Path)
    parser.add_argument("output", type=Path)
    parser.add_argument("--codec", required=True, type=Path)
    args = parser.parse_args()
    make(args.source.resolve(), args.output.resolve(), args.codec.resolve())
