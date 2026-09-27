#!/usr/bin/env python3
"""Score a candidate renderer's signed point gains against the frozen Dolby map."""

import argparse
import json
import math
from pathlib import Path

import numpy as np


def main() -> None:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--reference", type=Path, required=True)
    parser.add_argument("--candidate", type=Path, required=True)
    parser.add_argument("--channel-map", type=Path, required=True)
    parser.add_argument("--output", type=Path, required=True)
    args = parser.parse_args()
    if args.output.exists():
        parser.error("output exists")
    reference = json.loads(args.reference.read_text())
    candidate = json.loads(args.candidate.read_text())
    mapping = json.loads(args.channel_map.read_text())["layouts"]
    if reference["adm_sha256"] != candidate["adm_sha256"]:
        raise ValueError("candidate and Dolby reference used different ADMs")
    report = {"adm_sha256": reference["adm_sha256"], "layouts": {}}
    for layout, ref in reference["layouts"].items():
        test = candidate["candidates"][layout]
        if ref["delay_samples"] != test["delay_samples"]:
            raise ValueError(f"{layout}: candidate and reference delay differ")
        channel_map = mapping[layout]["interleaved_to_mono"]
        if not mapping[layout]["all_samples_exact"]:
            raise ValueError(f"{layout}: channel map has not passed sample comparison")
        by_label = {point["label"]: point for point in test["points"]}
        rows = []
        for point in ref["points"]:
            if ref.get("channel_order") == "Dolby numbered multi-mono":
                wanted = np.array(point["gains"], dtype=np.float64)
            else:
                wanted = np.zeros(len(channel_map), dtype=np.float64)
                for item in channel_map:
                    wanted[item["mono_index"]] = point["gains"][item["interleaved_index"]]
            observed = np.array(by_label[point["label"]]["gains"], dtype=np.float64)
            relative = float(np.linalg.norm(observed - wanted) / max(np.linalg.norm(wanted), 1e-12))
            maximum = float(np.max(np.abs(observed - wanted)))
            wanted_power = float(wanted @ wanted)
            observed_power = float(observed @ observed)
            power_error_db = 10 * math.log10(observed_power / wanted_power) if observed_power > 0 else None
            rows.append({"label": point["label"], "xyz": point["xyz"],
                         "relative_l2": relative, "max_abs": maximum,
                         "power_error_db": power_error_db,
                         "blackout": wanted_power > 0.01 and observed_power < 1e-6})
        relative_values = np.array([row["relative_l2"] for row in rows])
        report["layouts"][layout] = {
            "points": len(rows),
            "mean_relative_l2": float(np.mean(relative_values)),
            "p95_relative_l2": float(np.quantile(relative_values, .95)),
            "max_relative_l2": float(np.max(relative_values)),
            "max_abs_gain_error": max(row["max_abs"] for row in rows),
            "max_abs_power_error_db": max(abs(row["power_error_db"]) for row in rows
                                          if row["power_error_db"] is not None),
            "over_one_percent": int(np.count_nonzero(relative_values > .01)),
            "blackouts": [row["label"] for row in rows if row["blackout"]],
            "passes_point_thresholds": all(
                row["relative_l2"] <= 0.01 and row["max_abs"] <= 0.02
                and row["power_error_db"] is not None and abs(row["power_error_db"]) <= 0.1
                and not row["blackout"] for row in rows
            ),
            "worst": sorted(rows, key=lambda row: row["relative_l2"], reverse=True)[:12],
        }
    args.output.write_text(json.dumps(report, indent=2) + "\n")
    print(args.output)


if __name__ == "__main__":
    main()
