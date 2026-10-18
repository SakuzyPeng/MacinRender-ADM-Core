#!/usr/bin/env python3
"""Compare complete same-host realtime replays, including their untrimmed tails.

No alignment search, gain fitting, resampling or cropping is performed. Exact
replays must continue to use compare-replays.py and their frozen bitwise gate.
Only the Python standard library is required.
"""
import argparse
import array
import hashlib
import json
import math
from pathlib import Path
import re
import sys


def structure(value):
    """Keep preparation dimensions/geometry while reporting spectral hashes separately."""
    if isinstance(value, dict):
        return {key: structure(item) for key, item in value.items()}
    if isinstance(value, list):
        return [structure(item) for item in value]
    if isinstance(value, str) and re.fullmatch(r"[0-9a-f]{64}", value):
        return "<checkpoint-sha256>"
    return value


def samples(root, name, entry):
    if Path(name).name != name or not name.endswith(".f32"):
        raise ValueError("invalid replay payload name")
    payload = (root / name).read_bytes()
    if len(payload) != entry["samples"] * 4 or hashlib.sha256(payload).hexdigest() != entry["sha256"]:
        raise ValueError(f"payload length/hash mismatch: {root / name}")
    values = array.array("f", payload)
    if sys.byteorder != "little":
        values.byteswap()
    if not all(math.isfinite(value) for value in values):
        raise ValueError(f"nonfinite output: {root / name}")
    return values


def compare(reference_path, candidate_path):
    paths = [Path(reference_path), Path(candidate_path)]
    manifests = [json.loads(path.read_text(encoding="utf-8")) for path in paths]
    for manifest in manifests:
        if manifest.get("schema") != "mradm.room-replay" or manifest.get("version") != 1:
            raise ValueError("unsupported room replay manifest")
        if not manifest.get("processing_policy", "").startswith("room-realtime-v"):
            raise ValueError("rounding tolerances are only valid for realtime replays")
        if "experiment" in manifest:
            raise ValueError("experimental acoustic models are outside the rounding contract")
    reference, candidate = manifests
    exclude = {"files", "prepared", "lfe", "processing_policy"}
    metadata = [{k: v for k, v in value.items() if k not in exclude} for value in manifests]
    if metadata[0] != metadata[1]:
        raise ValueError("resources, events, latency, frames or tail metadata differ")
    for key in ["prepared", "lfe"]:
        if structure(reference[key]) != structure(candidate[key]):
            raise ValueError(f"preparation dimensions or geometry differ: {key}")
    if reference["files"].keys() != candidate["files"].keys() or not reference["files"]:
        raise ValueError("checkpoint set differs or is empty")
    if reference["files"]["session.f32"]["samples"] != reference["frames"] * 2:
        raise ValueError("stereo PCM frame count differs from manifest")
    rows = {}
    for name, entry in reference["files"].items():
        a = samples(paths[0].parent, name, entry)
        b = samples(paths[1].parent, name, candidate["files"][name])
        if len(a) != len(b):
            raise ValueError(f"checkpoint sample count differs: {name}")
        peak = max(map(abs, a), default=0.)
        difference = max((abs(x - y) for x, y in zip(a, b)), default=0.)
        energy = math.fsum(x * x for x in a)
        error = math.fsum((x - y) ** 2 for x, y in zip(a, b))
        silent = peak == 0.
        relative_l2 = math.sqrt(error / energy) if energy else None
        relative_peak = difference / peak if peak else None
        passed = difference == 0. if silent else relative_l2 <= 2e-5 and relative_peak <= 2e-4
        rows[name] = dict(samples=len(a), reference_peak=peak, maximum_error=difference,
                          relative_l2=relative_l2, maximum_over_reference_peak=relative_peak,
                          silent=silent, passed=passed)
    return dict(schema="mradm.room-realtime-rounding", version=1,
                reference_policy=reference["processing_policy"], candidate_policy=candidate["processing_policy"],
                thresholds=dict(relative_l2=2e-5, maximum_over_reference_peak=2e-4),
                frames=reference["frames"], latency_frames=reference["latency_frames"],
                tail_frames=reference["tail_frames"],
                preparation_hashes_identical={key: reference[key] == candidate[key] for key in ["prepared", "lfe"]},
                checkpoints=rows, passed=all(row["passed"] for row in rows.values()),
                scope="Complete HRIR, branch and stereo PCM outputs including tails; same host, inputs and event schedule required")


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("reference", type=Path)
    parser.add_argument("candidate", type=Path)
    parser.add_argument("--output", required=True, type=Path)
    args = parser.parse_args()
    report = compare(args.reference, args.candidate)
    args.output.write_text(json.dumps(report, indent=2, allow_nan=False) + "\n", encoding="utf-8")
    print(json.dumps(report, allow_nan=False))
    if not report["passed"]:
        raise SystemExit(1)


if __name__ == "__main__":
    main()
