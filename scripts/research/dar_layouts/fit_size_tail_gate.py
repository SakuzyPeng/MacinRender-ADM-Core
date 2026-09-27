#!/usr/bin/env python3
"""Fit one monotone 512-sample silence gate shared by all speaker channels."""

import argparse
import json
import math
from pathlib import Path

import numpy as np

from measure_point_suite import decode_wav
from measure_static_bank import file_sha256


def monotone_gate(numerator: np.ndarray, denominator: np.ndarray) -> np.ndarray:
    blocks = []
    for index, (value, weight) in enumerate(zip(numerator, denominator)):
        blocks.append([index, index, value, weight])
        while len(blocks) > 1 and blocks[-2][2] / blocks[-2][3] < blocks[-1][2] / blocks[-1][3]:
            final = blocks.pop()
            blocks[-1][1] = final[1]
            blocks[-1][2] += final[2]
            blocks[-1][3] += final[3]
    result = np.empty(len(numerator))
    for first, last, amount, weight in blocks:
        result[first:last + 1] = np.clip(amount / weight, 0.0, 1.0)
    return result


def tail_scores(reference: np.ndarray, candidate: np.ndarray) -> dict:
    expected = np.sum(reference ** 2, axis=1)
    observed = np.sum(candidate ** 2, axis=1)
    power_db = 10.0 * math.log10(float(np.sum(observed) / np.sum(expected)))
    first = np.cumsum(expected[::-1])[::-1]
    second = np.cumsum(observed[::-1])[::-1]
    ref_db = 10.0 * np.log10(np.maximum(first / first[0], 1e-12))
    test_db = 10.0 * np.log10(np.maximum(second / second[0], 1e-12))
    measurable = (ref_db <= -5.0) & (ref_db >= -30.0)
    return {"tail_power_error_db": power_db,
            "max_tail_curve_error_db": float(np.max(np.abs(ref_db[measurable] - test_db[measurable])))}


def main() -> None:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--long-manifest", type=Path, required=True)
    parser.add_argument("--dar-run", type=Path, required=True)
    parser.add_argument("--oracle-manifest", type=Path, required=True)
    parser.add_argument("--output", type=Path, required=True)
    args = parser.parse_args()
    if args.output.exists():
        parser.error("output exists")
    case = json.loads(args.long_manifest.read_text())["cases"][0]
    run = json.loads(args.dar_run.read_text())
    if not run["success"] or run.get("restore_errors") or (
        run["baseline_settings_sha256"] != run["settings_sha256_after"]
    ):
        raise ValueError("Dolby reference or restoration incomplete")
    reference = next(item for item in run["outputs"] if item["layout"] == "7.1.4")
    if file_sha256(Path(reference["path"])) != reference["sha256"]:
        raise ValueError("Dolby WAV changed")
    oracle = json.loads(args.oracle_manifest.read_text())["outputs"]["7.1.4"]
    if file_sha256(Path(oracle["path"])) != oracle["sha256"]:
        raise ValueError("oracle WAV changed")
    wanted = decode_wav(Path(reference["path"]), 12, case["duration_samples"]).astype(np.float64)
    available = decode_wav(Path(oracle["path"]), 12, case["duration_samples"]).astype(np.float64)
    samples = []
    for item in case["objects"]:
        stop = item["signal_stop_sample"]
        samples.append((item["label"], wanted[stop:stop + 512], available[stop:stop + 512]))
    training = samples[:2]
    ref_pcm = np.concatenate([item[1] for item in training], axis=1)
    test_pcm = np.concatenate([item[2] for item in training], axis=1)
    numerator = np.sum(ref_pcm * test_pcm, axis=1)
    denominator = np.maximum(np.sum(test_pcm * test_pcm, axis=1), 1e-15)
    gate = monotone_gate(numerator, denominator)
    rows = []
    for label, ref_pcm, raw_pcm in samples:
        rows.append({"label": label, "used_to_fit": label in (training[0][0], training[1][0]),
                     **tail_scores(ref_pcm, raw_pcm * gate[:, None])})
    report = {"reference_adm_sha256": case["sha256"], "reference_wav_sha256": reference["sha256"],
              "oracle_wav_sha256": oracle["sha256"], "gate_frames": 512,
              "gate": gate.tolist(), "scores": rows}
    args.output.write_text(json.dumps(report, indent=2) + "\n")
    print(args.output)


if __name__ == "__main__":
    main()
