#!/usr/bin/env python3
"""Score a shared geometry gain field against untouched size spatial points."""

import argparse
import json
import math
from pathlib import Path

import numpy as np
import torch

from fit_size_shared_field import CHANNELS, Field, features

TAIL_POWER = 0.42005 ** 2


def main() -> None:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--field", type=Path, required=True)
    parser.add_argument("--spatial-report", type=Path, required=True)
    parser.add_argument("--warm-bank", type=Path, required=True)
    parser.add_argument("--output", type=Path, required=True)
    args = parser.parse_args()
    if args.output.exists():
        parser.error("output exists")
    model_data = json.loads(args.field.read_text())
    model = Field(model_data["feature_count"])
    model.load_state_dict({name: torch.tensor(value, dtype=torch.float32)
                           for name, value in model_data["network"].items()})
    model.eval()
    probes = json.loads(args.spatial_report.read_text())["layouts"]["7.1.4"]["points"]
    warm = json.loads(args.warm_bank.read_text())["layouts"]["7.1.4"]["points"]
    knots = {0.0: 1.0}
    for point in warm:
        knots.setdefault(point["size"], []).append(point["total_energy"])
    knots = {size: float(np.mean(value)) if isinstance(value, list) else value for size, value in knots.items()}
    sizes = np.array(sorted(knots))
    values = np.array([knots[size] for size in sizes])
    rows = []
    for point in probes:
        with torch.no_grad():
            direct = model(torch.tensor(features(point["xyz"], point["size"])[None],
                                        dtype=torch.float32)).numpy()[0]
        direct = np.maximum(direct, 0.0)
        expected = np.array(point["speaker_energy"])[CHANNELS]
        estimated = direct * direct * (1.0 + TAIL_POWER)
        estimated[2] = direct[2] ** 2
        predicted_power = float(np.interp(point["size"], sizes, values))
        estimated *= predicted_power / max(float(np.sum(estimated)), 1e-12)
        relative = float(np.linalg.norm((estimated / estimated.sum()) - (expected / expected.sum())) /
                         np.linalg.norm(expected / expected.sum()))
        power_db = 10.0 * math.log10(float(estimated.sum() / expected.sum()))
        rows.append({"label": point["label"], "xyz": point["xyz"], "size": point["size"],
                     "normalized_energy_share_l2": relative, "total_power_error_db": power_db,
                     "passes_energy": relative <= 0.05 and abs(power_db) <= 0.1})
    report = {"model": "shared geometry function plus four global FIRs and size-only power rule",
              "reference": str(args.spatial_report.resolve()), "field": str(args.field.resolve()),
              "cases": rows, "max_energy_share_l2": max(row["normalized_energy_share_l2"] for row in rows),
              "max_abs_power_error_db": max(abs(row["total_power_error_db"]) for row in rows),
              "passes_all": all(row["passes_energy"] for row in rows)}
    args.output.write_text(json.dumps(report, indent=2) + "\n")
    print(args.output)


if __name__ == "__main__":
    main()
