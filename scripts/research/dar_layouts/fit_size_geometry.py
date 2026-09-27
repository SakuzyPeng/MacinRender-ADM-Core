#!/usr/bin/env python3
"""Fit one room-cloud geometry to measured coherent size gains; research only."""

import argparse
import json
import math
from pathlib import Path

import numpy as np

NODES, WEIGHTS = np.polynomial.legendre.leggauss(32)
WEIGHTS *= 0.5
UPPER = 80.0 / 155.0


def pair(fraction: np.ndarray) -> tuple[np.ndarray, np.ndarray]:
    angle = np.clip(fraction, 0.0, 1.0) * math.pi * 0.5
    return np.cos(angle), np.sin(angle)


def axis_samples(center: float, radius: float, lo: float, hi: float, edge_hold: float) -> np.ndarray:
    if abs(center - lo) < 1e-7 or abs(center - hi) < 1e-7:
        radius *= 1.0 - edge_hold
    return np.clip(center + radius * NODES, lo, hi)


def expected_gains(xyz: list[float], rxy: float, rz: float, front_y: float, edge_hold: float) -> np.ndarray:
    x0, y0, z0 = xyz
    xx = axis_samples(x0, rxy, -1.0, 1.0, edge_hold)
    yy = axis_samples(y0, rxy, -1.0, 1.0, edge_hold)
    zz = axis_samples(z0, rz, 0.0, 1.0, edge_hold if z0 >= 1.0 - 1e-7 else 0.0)
    middle, upper = pair(zz)
    zmid = WEIGHTS @ middle
    zupper = WEIGHTS @ upper

    xrear_l, xrear_r = pair((xx + 1.0) * 0.5)
    rear = np.zeros(12)
    rear[6] = WEIGHTS @ xrear_l
    rear[7] = WEIGHTS @ xrear_r
    side = np.zeros(12)
    side[4:6] = rear[6:8]
    front = np.zeros(12)
    left = xx <= 0
    left_l, left_c = pair(xx[left] + 1.0)
    right_c, right_r = pair(xx[~left])
    front[0] = WEIGHTS[left] @ left_l
    front[1] = WEIGHTS[~left] @ right_r
    front[2] = WEIGHTS[left] @ left_c + WEIGHTS[~left] @ right_c

    rear_weight = np.zeros_like(yy)
    side_weight = np.zeros_like(yy)
    front_weight = np.zeros_like(yy)
    behind = yy < 0
    a, b = pair(yy[behind] + 1.0)
    rear_weight[behind] = a
    side_weight[behind] = b
    ahead = ~behind
    a, b = pair(yy[ahead] / front_y)
    side_weight[ahead] = a
    front_weight[ahead] = b
    middle_gain = (WEIGHTS @ rear_weight) * rear + (WEIGHTS @ side_weight) * side + (
        WEIGHTS @ front_weight
    ) * front

    upper_l, upper_r = pair((xx + UPPER) / (2 * UPPER))
    top_rear = np.zeros(12)
    top_rear[10] = WEIGHTS @ upper_l
    top_rear[11] = WEIGHTS @ upper_r
    top_front = np.zeros(12)
    top_front[8:10] = top_rear[10:12]
    top_rear_w, top_front_w = pair((yy + UPPER) / (2 * UPPER))
    upper_gain = (WEIGHTS @ top_rear_w) * top_rear + (WEIGHTS @ top_front_w) * top_front
    return zmid * middle_gain + zupper * upper_gain


def fit_size(rows: list[dict], size: float) -> dict:
    measurements = [row for row in rows if abs(row["size"] - size) < 1e-8]
    reference = np.array([item["gains"] for item in measurements], dtype=np.float64)
    weights = np.array([item["weight"] for item in measurements], dtype=np.float64)
    best = None
    initial = [(min(4.0, size * r), min(2.0, size * z), f, hold)
               for r in (1.6, 2.4, 3.2) for z in (1.2, 1.8) for f in (0.75, 0.9)
               for hold in (0.0, 0.5, 1.0)]
    for start in initial:
        values = list(start)
        spans = [max(0.2, size * 0.8), max(0.2, size * 0.6), 0.16, 0.25]
        for _ in range(4):
            for axis in range(4):
                candidates = np.linspace(values[axis] - spans[axis], values[axis] + spans[axis], 9)
                trial_results = []
                for candidate in candidates:
                    trial = values.copy()
                    trial[axis] = float(candidate)
                    if (not 0.0 <= trial[0] <= 5.0 or not 0.0 <= trial[1] <= 3.0 or
                        not 0.4 <= trial[2] <= 1.0 or not 0.0 <= trial[3] <= 1.0):
                        continue
                    predicted = np.array([expected_gains(item["xyz"], *trial) for item in measurements])
                    global_scale = float(np.sum(weights[:, None] * predicted * reference) /
                                         max(np.sum(weights[:, None] * predicted * predicted), 1e-12))
                    errors = (np.linalg.norm(predicted * global_scale - reference, axis=1) /
                              np.maximum(np.linalg.norm(reference, axis=1), 1e-12))
                    loss = float(np.average(errors ** 2, weights=weights))
                    trial_results.append((loss, trial, global_scale, errors))
                if trial_results:
                    _, values, _, _ = min(trial_results, key=lambda result: result[0])
            spans = [span * 0.5 for span in spans]
        predicted = np.array([expected_gains(item["xyz"], *values) for item in measurements])
        global_scale = float(np.sum(weights[:, None] * predicted * reference) /
                             max(np.sum(weights[:, None] * predicted * predicted), 1e-12))
        errors = (np.linalg.norm(predicted * global_scale - reference, axis=1) /
                  np.maximum(np.linalg.norm(reference, axis=1), 1e-12))
        loss = float(np.average(errors ** 2, weights=weights))
        if best is None or loss < best[0]:
            best = (loss, values.copy(), global_scale, errors)
    _, values, scale, errors = best
    return {"size": size, "rxy": values[0], "rz": values[1], "front_row_y": values[2],
            "edge_hold": values[3],
            "global_scale": scale, "weighted_rms_l2": math.sqrt(best[0]),
            "max_relative_l2": float(np.max(errors)),
            "cases": [{"label": item["label"], "relative_l2": float(error)}
                      for item, error in zip(measurements, errors)]}


def main() -> None:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--size-train", type=Path, required=True)
    parser.add_argument("--warm-bank", type=Path, required=True)
    parser.add_argument("--size-validation", type=Path, required=True)
    parser.add_argument("--output", type=Path, required=True)
    args = parser.parse_args()
    if args.output.exists():
        parser.error("output exists")
    rows = []
    for item in json.loads(args.size_train.read_text())["layouts"]["7.1.4"]["points"]:
        if item["size"] > 0:
            rows.append({"label": item["label"], "xyz": item["xyz"], "size": item["size"],
                         "gains": item["coherent_gain"], "weight": 1.0})
    for item in json.loads(args.warm_bank.read_text())["layouts"]["7.1.4"]["points"]:
        rows.append({"label": item["label"], "xyz": item["xyz"], "size": item["size"],
                     "gains": item["signed_direct_gain"], "weight": 3.0})
    fitted = [fit_size(rows, size) for size in (0.25, 0.5, 1.0)]
    knots = [{"size": 0.0, "rxy": 0.0, "rz": 0.0, "front_row_y": 1.0,
              "edge_hold": 1.0, "global_scale": 1.0}]
    knots += [{key: fit[key] for key in ("size", "rxy", "rz", "front_row_y", "edge_hold", "global_scale")}
              for fit in fitted]
    validation = []
    for item in json.loads(args.size_validation.read_text())["layouts"]["7.1.4"]["points"]:
        size = item["size"]
        low = next(knot for knot in reversed(knots) if knot["size"] <= size)
        high = next(knot for knot in knots if knot["size"] >= size)
        fraction = (size - low["size"]) / (high["size"] - low["size"]) if high != low else 0.0
        parameters = {key: low[key] + fraction * (high[key] - low[key])
                      for key in ("rxy", "rz", "front_row_y", "edge_hold", "global_scale")}
        predicted = expected_gains(item["xyz"], parameters["rxy"], parameters["rz"],
                                   parameters["front_row_y"], parameters["edge_hold"]) * parameters["global_scale"]
        reference = np.array(item["coherent_gain"])
        error = float(np.linalg.norm(predicted - reference) / max(np.linalg.norm(reference), 1e-12))
        validation.append({"label": item["label"], "size": size, "relative_l2": error})
    report = {"model": "boundary-frozen uniform room cloud; global size geometry only",
              "training": fitted, "validation": validation,
              "validation_mean_relative_l2": float(np.mean([item["relative_l2"] for item in validation])),
              "validation_max_relative_l2": max(item["relative_l2"] for item in validation)}
    args.output.write_text(json.dumps(report, indent=2) + "\n")
    print(args.output)


if __name__ == "__main__":
    main()
