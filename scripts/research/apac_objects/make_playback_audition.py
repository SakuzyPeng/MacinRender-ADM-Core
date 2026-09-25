#!/usr/bin/env python3
"""Build a faded, pure-object left/right APAC audition using the default codec.

One object, no BED, silent lead-in and gap; positions are repeated every 1024
samples. The same band-limited noise is used for the left and right passages.
"""

import argparse
import json
from pathlib import Path
import plistlib

import numpy as np

from make_inputs import metadata, settings
from run_case import run_case
from wrap_caf import wrap


def make(root, codec):
    root.mkdir(parents=True, exist_ok=False)
    rate, frames = 48000, 12 * 48000
    signal_frames = int(3.5 * rate)
    rng = np.random.default_rng(20260923)
    spectrum = np.fft.rfft(rng.standard_normal(signal_frames))
    hz = np.fft.rfftfreq(signal_frames, 1 / rate)
    spectrum *= ((hz >= 300) & (hz <= 6000)) / np.sqrt(np.maximum(hz, 1))
    noise = np.fft.irfft(spectrum, n=signal_frames)
    noise *= .04 / np.sqrt(np.mean(noise**2))
    fade = int(.1 * rate)
    ramp = .5 - .5 * np.cos(np.linspace(0, np.pi, fade))
    noise[:fade] *= ramp
    noise[-fade:] *= ramp[::-1]
    x = np.zeros((frames, 2), dtype="<f4")
    for start in (rate, int(6.5 * rate)):
        x[start:start + signal_frames, 0] = noise
    timeline = []
    for start in range(0, frames, 1024):
        azimuth = 60 if start < 5.5 * rate else -60
        blob = metadata([azimuth])
        words = np.frombuffer(blob + b"\0" * (len(blob) % 2), dtype="<i2").astype("<f4") / 32768
        x[start:start + len(words), 1] = words
        timeline.append({"sample": start, "azimuth": azimuth, "metadata_hex": blob.hex()})
    x.tofile(root / "input.f32")
    (root / "settings.plist").write_bytes(plistlib.dumps(settings(1)))
    expected = {"sample_rate": rate, "frames": frames, "audio_channels": 1, "metadata_channels": 1,
                "bed_channels": 0, "signal": "identical seeded 300–6000 Hz pink noise, 100 ms fades",
                "passages": [{"start": 1, "end": 4.5, "azimuth": 60},
                             {"start": 6.5, "end": 10, "azimuth": -60}],
                "input_peak": float(np.max(np.abs(x[:, 0]))), "timeline": timeline}
    (root / "expected.json").write_text(json.dumps(expected, indent=2) + "\n")
    prefix = root / "encode" / "stream"
    commands = [("encode", [str(codec), "encode", str(root / "settings.plist"), str(root / "input.f32"),
                             "2", "1", str(prefix), "0", "256000"])]
    for name, command in commands:
        result = run_case(command, root / name, 30)
        if result["returncode"] or result["timed_out"]:
            raise RuntimeError(f"{name} failed")
    caf, mp4 = root / "object-left-then-right.caf", root / "object-left-then-right.mp4"
    wrap(prefix, caf)
    result = run_case(["afconvert", str(caf), str(mp4), "-f", "mp4f", "-d", "0"], root / "wrap", 30)
    if result["returncode"] or result["timed_out"]:
        raise RuntimeError("MP4 wrap failed")
    print(mp4)


if __name__ == "__main__":
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("output", type=Path)
    parser.add_argument("--codec", required=True, type=Path)
    args = parser.parse_args()
    make(args.output.resolve(), args.codec.resolve())
