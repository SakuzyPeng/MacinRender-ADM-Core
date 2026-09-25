#!/usr/bin/env python3
"""Summarize the retained QuickTime USB measurements and diffuse controls."""

import argparse
import json
from pathlib import Path

import numpy as np


def pcm(folder):
    return np.fromfile(folder / "audio.s32le", dtype="<i4").reshape(-1, 2).astype(float) / 2**31


def summarize(root):
    usb = root / "usb-airpods"
    positional = pcm(usb / "qt-pure-object")
    static = []
    for name, start, end in (("left", 12.2, 15.2), ("right", 17.7, 20.7)):
        x = positional[round(start * 48000):round(end * 48000)]
        rms = np.sqrt(np.mean(x**2, axis=0))
        static.append({"passage": name, "capture_window": [start, end], "rms": rms.tolist(),
                       "lr_db": float(20 * np.log10(rms[0] / rms[1]))})
    vocals = []
    for name in ("qt-vocal-original", "qt-vocal-diffuse0"):
        x = pcm(usb / name)
        active = np.flatnonzero(np.max(np.abs(x), axis=1) > 1e-6)
        if not len(active):
            raise ValueError(name + ": no onset to align")
        first = active[0]
        steady = x[first + round(.3 * 48000):first + round(2.8 * 48000)]
        vocals.append({"case": name, "threshold": 1e-6, "first_active_seconds": first / 48000,
                       "last_active_seconds": active[-1] / 48000,
                       "active_span_seconds": (active[-1] - first + 1) / 48000,
                       "steady_window_relative_to_onset": [.3, 2.8],
                       "steady_rms": np.sqrt(np.mean(steady**2, axis=0)).tolist(),
                       "steady_exact_zero": bool(np.all(steady == 0))})
    assert vocals[0]["steady_exact_zero"] and not vocals[1]["steady_exact_zero"]
    tones = json.loads((usb / "qt-groups7/analysis.json").read_text())["tones"]
    assert tones["object_indices_detected"] == list(range(32))
    matrix = json.loads((usb / "diffuse-matrix-v2/verification.json").read_text())
    reference = np.array(matrix[0]["steady_rms"])
    for row in matrix[:6]:
        gain = np.array(row["steady_rms"]) / reference
        row["gain_vs_diffuse0"] = gain.tolist()
        row["energy_vs_diffuse0"] = (gain**2).tolist()
    summary = {"system_decoder_modified": False, "quicktime_modified": False,
               "usb_format": "48000 Hz stereo, signed LE 24-bit in 32-bit; low byte verified zero",
               "usb_interface": "XHC2", "usb_location_id": "0x02100000", "usb_endpoint": "0x05 OUT",
               "static_position": static, "listener_confirmed_left_then_right": True,
               "objects32": tones, "vocal_comparison": vocals, "diffuse_gain_matrix": matrix,
               "simple_numeric_diffuse_limit_proven": False,
               "limitation": "diffuse=0 is a changed diagnostic, not complete ADM semantic preservation"}
    (usb / "FINAL_VERIFICATION.json").write_text(json.dumps(summary, indent=2) + "\n")
    print(json.dumps({"static_position": static, "vocal_comparison": vocals}, indent=2))
    try:
        import matplotlib
        matplotlib.use("Agg")
        import matplotlib.pyplot as plt
    except ImportError:
        return
    fig, axes = plt.subplots(1, 2, figsize=(11, 4), layout="constrained")
    for row, color, label in zip(vocals, ("#b33a3a", "#176d9c"), ("Original diffuse=1", "Diagnostic diffuse=0")):
        x = pcm(usb / row["case"])
        onset = round(row["first_active_seconds"] * 48000)
        values = [np.sqrt(np.mean(x[onset + i:onset + i + 480]**2)) for i in range(0, 3 * 48000, 480)]
        axes[0].plot(np.arange(len(values)) * .01, 20 * np.log10(np.maximum(values, 1e-8)), color=color, label=label)
    axes[0].set(xlabel="Seconds after first output", ylabel="USB stereo RMS (dBFS)", ylim=(-165, -25),
                title="QuickTime → AirPods Max USB: isolated vocals")
    axes[0].legend(fontsize=8)
    axes[0].grid(alpha=.2)
    axes[1].scatter([r["diffuse"] for r in matrix[:6]], [r["energy_vs_diffuse0"][0] for r in matrix[:6]],
                    label="Observed output energy", color="#176d9c")
    axes[1].plot([0, 1], [1, 0], color="#666666", linestyle="--", label="1 − diffuse")
    axes[1].set(xlabel="Input diffuse", ylabel="Energy relative to diffuse=0", title="Native mixer: 42-channel scene")
    axes[1].legend(fontsize=8)
    axes[1].grid(alpha=.2)
    fig.savefig(usb / "usb-vocal-evidence.png", dpi=160)
    plt.close(fig)


if __name__ == "__main__":
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("root", type=Path)
    args = parser.parse_args()
    summarize(args.root)
