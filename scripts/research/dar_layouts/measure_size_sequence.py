#!/usr/bin/env python3
"""Measure channel energy and coherent gain for one active size object at a time."""

import argparse
import json
import math
from pathlib import Path

import numpy as np

from measure_point_suite import decode_wav
from measure_static_bank import file_sha256, read_multichannel_adm


def analyze_segment(source: np.ndarray, rendered: np.ndarray, segment: dict,
                    canonical_order: list[int]) -> dict:
    start = segment["start_sample"]
    first, last = start + 4800, start + 14_400
    x = source[first:last, segment["input_channel"]].astype(np.float64)
    y = rendered[first:last].astype(np.float64)
    denominator = float(np.mean(x * x))
    if denominator < 1e-12:
        raise ValueError(f"no input signal in segment {segment['label']}")
    coherent = (x @ y) / float(x @ x)
    energies = np.sqrt(np.mean(y * y, axis=0) / denominator)
    residual = y - x[:, None] * coherent[None, :]
    residual_ratio = float(np.linalg.norm(residual) / max(np.linalg.norm(y), 1e-12))
    half = len(x) // 2
    early = np.sqrt(np.mean(y[:half] ** 2, axis=0) / np.mean(x[:half] ** 2))
    late = np.sqrt(np.mean(y[half:] ** 2, axis=0) / np.mean(x[half:] ** 2))
    energy_drift = float(np.linalg.norm(early - late) / max(np.linalg.norm(energies), 1e-12))
    canonical_energy = np.zeros_like(energies)
    canonical_coherent = np.zeros_like(coherent)
    for interleaved, mono in enumerate(canonical_order):
        canonical_energy[mono] = energies[interleaved]
        canonical_coherent[mono] = coherent[interleaved]
    return {"label": segment["label"], "xyz": segment["xyz"], "size": segment["size"],
            "energy": [round(float(v), 8) for v in canonical_energy],
            "coherent_gain": [round(float(v), 8) for v in canonical_coherent],
            "power_ratio": round(float(canonical_energy @ canonical_energy), 7),
            "residual_ratio": round(residual_ratio, 6),
            "energy_drift": round(energy_drift, 6)}


def main() -> None:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--suite-manifest", type=Path, required=True)
    parser.add_argument("--case", required=True)
    parser.add_argument("--dar-run", type=Path, required=True)
    parser.add_argument("--channel-map", type=Path, required=True)
    parser.add_argument("--candidate-714", type=Path)
    parser.add_argument("--candidate-916", type=Path)
    parser.add_argument("--output", type=Path, required=True)
    args = parser.parse_args()
    if args.output.exists():
        parser.error("output exists")
    manifest = json.loads(args.suite_manifest.read_text())
    case = next((item for item in manifest["cases"] if item["case_id"] == args.case), None)
    if case is None or "segments" not in case:
        parser.error("case is not a sequential size probe")
    adm = Path(case["adm"])
    if file_sha256(adm) != case["sha256"]:
        raise ValueError("final ADM differs from manifest")
    run = json.loads(args.dar_run.read_text())
    if not run["success"] or Path(run["adm"]).resolve() != adm.resolve():
        raise ValueError("Dolby run did not export the same ADM")
    channel_map = json.loads(args.channel_map.read_text())["layouts"]
    frames = case["duration_samples"]
    source = read_multichannel_adm(adm, case["num_channels"], frames)
    report = {"case_id": args.case, "adm_sha256": case["sha256"], "layouts": {}}
    for artifact in run["outputs"]:
        layout = artifact["layout"]
        output_path = Path(artifact["path"])
        if file_sha256(output_path) != artifact["sha256"]:
            raise ValueError(f"Dolby WAV changed: {output_path}")
        rendered = decode_wav(output_path, artifact["channels"], frames).astype(np.float64)
        if not channel_map[layout]["all_samples_exact"]:
            raise ValueError(f"unverified channel map for {layout}")
        order = [item["mono_index"] for item in channel_map[layout]["interleaved_to_mono"]]
        rows = [analyze_segment(source, rendered, segment, order) for segment in case["segments"]]
        report["layouts"][layout] = {"wav": str(output_path.resolve()), "points": rows}
        candidate_path = args.candidate_714 if layout == "7.1.4" else args.candidate_916
        if candidate_path is not None:
            candidate_pcm = decode_wav(candidate_path, artifact["channels"], frames)
            candidate_order = [0, 1, 2, 3, 6, 7, 4, 5, 8, 9, 10, 11] if layout == "7.1.4" else list(range(16))
            candidate_rows = [analyze_segment(source, candidate_pcm, segment, candidate_order)
                              for segment in case["segments"]]
            scores = []
            for expected, observed in zip(rows, candidate_rows):
                wanted = np.array(expected["energy"])
                actual = np.array(observed["energy"])
                relative = float(np.linalg.norm(actual - wanted) / np.linalg.norm(wanted))
                power_db = 10.0 * math.log10(observed["power_ratio"] / expected["power_ratio"])
                scores.append({"label": expected["label"], "size": expected["size"],
                               "energy_relative_l2": relative, "power_error_db": power_db})
            report.setdefault("candidates", {})[layout] = {
                "wav": str(candidate_path.resolve()), "model": "size disabled; Cartesian point baseline",
                "points": candidate_rows, "scores": scores,
                "max_energy_relative_l2": max(row["energy_relative_l2"] for row in scores),
                "max_abs_power_error_db": max(abs(row["power_error_db"]) for row in scores),
                "passes_size_thresholds": all(row["energy_relative_l2"] <= 0.05 and
                                               abs(row["power_error_db"]) <= 0.1 for row in scores),
            }
    args.output.write_text(json.dumps(report, indent=2) + "\n")
    print(args.output)


if __name__ == "__main__":
    main()
