#!/usr/bin/env python3
"""Compare Release room_impulse output with the frozen 22.2 + six BRIRs.

Only this verifier applies the reference speakers' distance calibration. The
renderer continues to use real distance. Dependencies match pack-reference.py,
plus soundfile; no direction library is retained.
"""
from __future__ import annotations

import argparse
import hashlib
import json
from pathlib import Path
import subprocess
import tempfile

import numpy as np
import pyroomacoustics as pra
from scipy import signal
import soundfile as sf


def main() -> None:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--probe", type=Path, required=True)
    parser.add_argument("--room", type=Path, required=True)
    parser.add_argument("--reference", type=Path, required=True)
    parser.add_argument("--output", type=Path, required=True)
    args = parser.parse_args()
    rows = [json.loads(line) for line in (args.reference / "index.jsonl").read_text().splitlines()]
    results = []
    with tempfile.TemporaryDirectory(prefix="mradm-room-reference-") as temporary:
        for row in rows:
            path = Path(temporary) / "probe.f32"
            command = [str(args.probe.resolve()), str(args.room.resolve()), str(row["azimuth_deg"]),
                       str(row["elevation_deg"]), str(row["distance_m"]), str(path)]
            metadata = json.loads(subprocess.check_output(command, text=True))
            actual = np.fromfile(path, dtype="<f4").reshape(-1, 2).astype(np.float64)
            # The runtime retains causal material support at near distances.
            # Remove only its declared common protection delay for this frozen
            # reference comparison; real playback retains that delay.
            actual = actual[metadata["material_filter_guard_frames"]:]
            path.unlink()
            actual *= row["source"]["whole_channel_gain"] / 2.1
            extra = row["source"]["extra_alignment_samples"]
            if extra > 1e-9:
                integer = int(np.floor(extra))
                fraction = np.array([extra - integer], dtype=np.float32)
                kernel = np.zeros((1, 81), np.float32)
                pra.libroom.fractional_delay(kernel, fraction, pra.constants.get("sinc_lut_granularity"), 1)
                kernel /= kernel.sum()
                # The runtime already contains the common 40-sample alignment
                # guard. Replace that guard by the reference fractional delay.
                actual = np.pad(signal.fftconvolve(actual, kernel[0, :, None], axes=0),
                                ((integer, 0), (0, 0)))[40:]
            expected, rate = sf.read(args.reference / row["file"], always_2d=True)
            if rate != 48000 or expected.shape[1] != 2:
                raise ValueError("reference must be 48 kHz stereo")
            length = max(len(actual), len(expected))
            actual = np.pad(actual, ((0, length - len(actual)), (0, 0)))
            expected = np.pad(expected, ((0, length - len(expected)), (0, 0)))
            error = float(np.linalg.norm(actual - expected) / np.linalg.norm(expected))
            result = {"id": row["id"], "label": row.get("label"), "relative_l2": error,
                      "passed": bool(np.isfinite(actual).all() and error <= 2e-4), **metadata}
            results.append(result)
            print(f"{row['id']:2d} {row.get('label', '')}: relative L2={error:.9g}", flush=True)
    report = {
        "schema": "mradm.room-reference-validation", "version": 1,
        "passed": all(row["passed"] for row in results), "threshold_relative_l2": 2e-4,
        "maximum_relative_l2": max(row["relative_l2"] for row in results),
        "probe_sha256": hashlib.sha256(args.probe.read_bytes()).hexdigest(),
        "room_manifest_sha256": hashlib.sha256(args.room.read_bytes()).hexdigest(),
        "probes": results,
        "scope": "Static calibrated BRIR reconstruction; not a dynamic, realtime or full standards certification.",
    }
    args.output.write_text(json.dumps(report, indent=2, ensure_ascii=False, allow_nan=False) + "\n")
    print(f"passed={report['passed']}; maximum relative L2={report['maximum_relative_l2']:.9g}")
    if not report["passed"]:
        raise SystemExit(1)


if __name__ == "__main__":
    main()
