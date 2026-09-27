#!/usr/bin/env python3
"""Compare six fixed ADM object segments in Dolby and Release SAF WAV renders."""

import argparse
import hashlib
import json
import subprocess
from pathlib import Path

import numpy as np

SAMPLE_RATE = 48_000
SEGMENTS = ("X-min", "X-max", "Y-min", "Y-max", "origin", "Z-max")
FILES = {
    "7.1.4": {
        "channels": 12,
        "dolby": "probe-axes-dar-714_re-render 02.wav",
        "saf_apple": "probe-axes-saf-714-apple.wav",
    },
    "9.1.6": {
        "channels": 16,
        "dolby": "probe-axes-dar-916_re-render 02.wav",
        "saf_apple": "probe-axes-saf-916-apple.wav",
    },
}


def read_float_wav(path: Path, channels: int) -> np.ndarray:
    process = subprocess.run(
        ["ffmpeg", "-v", "error", "-i", str(path), "-f", "f32le", "-acodec", "pcm_f32le", "-"],
        check=True,
        capture_output=True,
    )
    pcm = np.frombuffer(process.stdout, dtype="<f4")
    expected = 6 * SAMPLE_RATE * channels
    if pcm.size != expected:
        raise ValueError(f"{path}: expected {expected} samples, got {pcm.size}")
    pcm = pcm.reshape(6 * SAMPLE_RATE, channels)
    if not np.isfinite(pcm).all():
        raise ValueError(f"{path}: nonfinite PCM")
    return pcm


def summarize(path: Path, channels: int) -> dict:
    pcm = read_float_wav(path, channels)
    segments = []
    for number, name in enumerate(SEGMENTS):
        window = pcm[number * SAMPLE_RATE + 4_800 : number * SAMPLE_RATE + 19_200].astype(np.float64)
        rms = np.sqrt(np.mean(window * window, axis=0))
        segments.append(
            {
                "name": name,
                "rms": [round(float(value), 8) for value in rms],
                "power": round(float(np.sum(rms * rms)), 9),
                "nonzero_channels": int(np.count_nonzero(rms > 1e-7)),
            }
        )
    return {
        "file": str(path.resolve()),
        "sha256": hashlib.sha256(path.read_bytes()).hexdigest(),
        "frames": len(pcm),
        "channels": channels,
        "segments": segments,
    }


def main() -> None:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("directory", type=Path)
    parser.add_argument("--output", type=Path, required=True)
    args = parser.parse_args()
    if args.output.exists():
        parser.error(f"output already exists: {args.output}")
    report = {
        "sample_rate": SAMPLE_RATE,
        "window_samples_in_segment": [4_800, 19_200],
        "layouts": {},
    }
    for layout, files in FILES.items():
        report["layouts"][layout] = {
            name: summarize(args.directory / filename, files["channels"])
            for name, filename in files.items()
            if name != "channels"
        }
    args.output.write_text(json.dumps(report, indent=2, ensure_ascii=False) + "\n", encoding="utf-8")
    print(args.output)


if __name__ == "__main__":
    main()
