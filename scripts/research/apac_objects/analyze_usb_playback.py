#!/usr/bin/env python3
"""Measure captured final USB stereo without re-rendering source positions."""

import argparse
import json
from pathlib import Path

import numpy as np


def analyze(folder, tones=None, window=None):
    summary = json.loads((folder / "summary.json").read_text())
    rate = summary["sample_rate_assumption"]
    pcm = np.fromfile(folder / "audio.s32le", dtype="<i4").reshape(-1, 2).astype(float) / 2**31
    size = round(.1 * rate)
    windows = []
    for start in range(0, len(pcm) - size + 1, size):
        x = pcm[start:start + size]
        rms = np.sqrt(np.mean(x**2, axis=0))
        windows.append({"start": start / rate, "end": (start + size) / rate, "rms": rms.tolist(),
                        "lr_db": float(20 * np.log10(max(rms[0], 1e-15) / max(rms[1], 1e-15)))})
    result = {"capture": str(folder), "frames": len(pcm), "windows": windows}
    if tones:
        if not window:
            raise ValueError("select a verified steady playback window for the tone analysis")
        expected = json.loads(tones.read_text())
        hz = np.array(expected["audio_frequencies"])
        x = pcm[round(window[0] * rate):round(window[1] * rate)]
        if len(x) != round((window[1] - window[0]) * rate):
            raise ValueError("analysis window extends beyond captured audio")
        phase = 2 * np.pi * np.arange(len(x))[:, None] / rate * hz[None, :]
        basis = np.concatenate((np.cos(phase), np.sin(phase)), axis=1)
        coefficients = np.linalg.lstsq(basis, x, rcond=None)[0]
        amplitudes = np.hypot(coefficients[:len(hz)], coefficients[len(hz):])
        residual = x - basis @ coefficients
        bed = expected["bed_channels"]
        result["tones"] = {"window": window, "frequencies": hz.tolist(), "bed_channels": bed,
                           "object_indices_detected": np.flatnonzero(np.max(amplitudes[bed:], axis=1) > 1e-4).tolist(),
                           "detection_threshold": 1e-4, "amplitudes": amplitudes.tolist(),
                           "object_lr_db": (20 * np.log10(amplitudes[bed:, 0] / amplitudes[bed:, 1])).tolist(),
                           "residual_energy_fraction": float(np.sum(residual**2) / np.sum(x**2))}
        print(json.dumps({k: result["tones"][k] for k in ("object_indices_detected", "residual_energy_fraction")}))
    (folder / "analysis.json").write_text(json.dumps(result, indent=2) + "\n")


if __name__ == "__main__":
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("folder", type=Path)
    parser.add_argument("--tones", type=Path)
    parser.add_argument("--window", nargs=2, type=float)
    args = parser.parse_args()
    analyze(args.folder, args.tones, args.window)
