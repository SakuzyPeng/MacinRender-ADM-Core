#!/usr/bin/env python3
"""Fit a compact Cartesian row/layer panner to the ADM point probes."""

import argparse
import json
import math
from pathlib import Path

import numpy as np

LABELS = {
    "7.1.4": ["L", "R", "C", "LFE", "Lss", "Rss", "Lrs", "Rrs", "Ltf", "Rtf", "Ltr", "Rtr"],
    "9.1.6": ["L", "R", "C", "LFE", "Lss", "Rss", "Lrs", "Rrs", "Lw", "Rw",
              "Ltf", "Rtf", "Ltm", "Rtm", "Ltr", "Rtr"],
}
PARAMETERS = ("middle_x", "top_x_negative", "top_x_positive", "top_y_negative", "top_y_positive",
              "wide_y", "top_z", "positive_axis_bend")


def pair_weights(t: float) -> tuple[float, float]:
    t = max(0.0, min(1.0, t))
    return math.cos(t * math.pi * 0.5), math.sin(t * math.pi * 0.5)


def pan_row(x: float, nodes: list[tuple[float, int]], channels: int) -> np.ndarray:
    result = np.zeros(channels, dtype=np.float64)
    if x <= nodes[0][0]:
        result[nodes[0][1]] = 1.0
    elif x >= nodes[-1][0]:
        result[nodes[-1][1]] = 1.0
    else:
        for (x0, ch0), (x1, ch1) in zip(nodes, nodes[1:]):
            if x0 <= x <= x1:
                result[ch0], result[ch1] = pair_weights((x - x0) / (x1 - x0))
                break
    return result


def pan_layer(x: float, y: float, rows: list[tuple[float, list]], channels: int) -> np.ndarray:
    if y <= rows[0][0]:
        return pan_row(x, rows[0][1], channels)
    if y >= rows[-1][0]:
        return pan_row(x, rows[-1][1], channels)
    for (y0, low), (y1, high) in zip(rows, rows[1:]):
        if y0 <= y <= y1:
            a, b = pair_weights((y - y0) / (y1 - y0))
            return a * pan_row(x, low, channels) + b * pan_row(x, high, channels)
    raise AssertionError("row interval not found")


def model_gain(layout: str, xyz: list[float], parameters: dict[str, float]) -> np.ndarray:
    x, y, z = xyz
    bend = parameters["positive_axis_bend"]
    if 0.0 < x < 1.0:
        x += bend * x * (1.0 - x) * (1.0 - 2.0 * x)
    if 0.0 < y < 1.0:
        y += bend * y * (1.0 - y) * (1.0 - 2.0 * y)
    channels = len(LABELS[layout])
    mid_x = parameters["middle_x"]
    top_x_negative = parameters["top_x_negative"]
    top_x_positive = parameters["top_x_positive"]
    top_y_negative = parameters["top_y_negative"]
    top_y_positive = parameters["top_y_positive"]
    sides = [(-mid_x, 4), (mid_x, 5)]
    rears = [(-mid_x, 6), (mid_x, 7)]
    fronts = [(-mid_x, 0), (0.0, 2), (mid_x, 1)]
    middle = [(-1.0, rears), (0.0, sides)]
    if layout == "9.1.6":
        middle.append((parameters["wide_y"], [(-mid_x, 8), (mid_x, 9)]))
    middle.append((1.0, fronts))
    if layout == "7.1.4":
        upper = [(-top_y_negative, [(-top_x_negative, 10), (top_x_positive, 11)]),
                 (top_y_positive, [(-top_x_negative, 8), (top_x_positive, 9)])]
    else:
        upper = [(-top_y_negative, [(-top_x_negative, 14), (top_x_positive, 15)]),
                 (0.0, [(-top_x_negative, 12), (top_x_positive, 13)]),
                 (top_y_positive, [(-top_x_negative, 10), (top_x_positive, 11)])]
    if parameters.get("vertical_steps", 0) > 0:
        steps = parameters["vertical_steps"]
        # Dolby's static 0.1/0.3/0.7/0.9 observations match a 75-step
        # nearest-integer code after storing ADM coordinates as float32.
        stored_z = float(np.float32(max(0.0, min(1.0, z))))
        vertical_position = math.floor(stored_z * steps + 0.5) / steps
    else:
        vertical_position = z / parameters["top_z"]
    a, b = pair_weights(vertical_position)
    return a * pan_layer(x, y, middle, channels) + b * pan_layer(x, y, upper, channels)


def load_reference(gains_path: Path, mapping_path: Path) -> dict:
    gains = json.loads(gains_path.read_text())["layouts"]
    mapping = json.loads(mapping_path.read_text())["layouts"]
    result = {}
    for layout in LABELS:
        channels = len(LABELS[layout])
        inter_to_mono = mapping[layout]["interleaved_to_mono"]
        if not mapping[layout]["all_samples_exact"] or len(inter_to_mono) != channels:
            raise ValueError(f"unverified channel map for {layout}")
        mono_labels = [None] * channels
        for item in inter_to_mono:
            mono_labels[item["mono_index"]] = item["label"]
        if mono_labels != LABELS[layout]:
            raise ValueError(f"unexpected channel order for {layout}: {mono_labels}")
        points = []
        for point in gains[layout]["points"]:
            raw = np.array(point["gains"], dtype=np.float64)
            if gains[layout].get("channel_order") == "Dolby numbered multi-mono":
                canonical = raw
            else:
                canonical = np.zeros(channels, dtype=np.float64)
                for item in inter_to_mono:
                    canonical[item["mono_index"]] = raw[item["interleaved_index"]]
            points.append((point["label"], point["xyz"], canonical))
        result[layout] = points
    return result


def errors(reference: dict, parameters: dict[str, float]) -> dict:
    result = {}
    for layout, points in reference.items():
        rows = []
        for label, xyz, actual in points:
            predicted = model_gain(layout, xyz, parameters)
            relative = float(np.linalg.norm(predicted - actual) / max(np.linalg.norm(actual), 1e-12))
            rows.append({"label": label, "xyz": xyz, "relative_l2": relative,
                         "max_abs": float(np.max(np.abs(predicted - actual))),
                         "predicted_power": float(predicted @ predicted)})
        values = np.array([row["relative_l2"] for row in rows])
        result[layout] = {"mean_relative_l2": float(np.mean(values)),
                          "p95_relative_l2": float(np.quantile(values, .95)),
                          "max_relative_l2": float(np.max(values)),
                          "over_one_percent": int(np.count_nonzero(values > .01)),
                          "worst": sorted(rows, key=lambda row: row["relative_l2"], reverse=True)[:12]}
    return result


def loss(reference: dict, parameters: dict[str, float]) -> float:
    squares = []
    for layout, points in reference.items():
        for _, xyz, actual in points:
            predicted = model_gain(layout, xyz, parameters)
            squares.append(float(np.sum((predicted - actual) ** 2)))
    return float(np.mean(squares))


def fit(reference: dict, vertical_steps: int = 0) -> dict[str, float]:
    params = {"middle_x": 1.0, "top_x_negative": .513, "top_x_positive": .519,
              "top_y_negative": .513, "top_y_positive": .519,
              "wide_y": 2.0 / 3.0, "top_z": 1.0, "positive_axis_bend": 0.0}
    if vertical_steps:
        params["vertical_steps"] = vertical_steps
    bounds = {"middle_x": (.95, 1.05), "top_x_negative": (.45, .6), "top_x_positive": (.45, .6),
              "top_y_negative": (.45, .6), "top_y_positive": (.45, .6),
              "wide_y": (.5, .85), "top_z": (.9, 1.1), "positive_axis_bend": (0.0, .08)}
    for step in (.025, .005, .001, .0002, .00005):
        for _ in range(3):
            changed = False
            for name in PARAMETERS:
                if vertical_steps and name == "top_z":
                    continue
                low, high = bounds[name]
                center = params[name]
                candidates = [max(low, min(high, center + offset * step)) for offset in range(-8, 9)]
                best = min(candidates, key=lambda value: loss(reference, {**params, name: value}))
                if best != params[name]:
                    params[name] = best
                    changed = True
            if not changed:
                break
    return params


def main() -> None:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--train", type=Path, required=True, action="append")
    parser.add_argument("--holdout", type=Path)
    parser.add_argument("--channel-map", type=Path, required=True)
    parser.add_argument("--vertical-steps", type=int, default=0)
    parser.add_argument("--output", type=Path, required=True)
    args = parser.parse_args()
    if args.output.exists():
        parser.error("output already exists")
    datasets = [load_reference(path, args.channel_map) for path in args.train]
    train = {layout: [point for dataset in datasets for point in dataset[layout]] for layout in LABELS}
    initial = {"middle_x": 1.0, "top_x_negative": .513, "top_x_positive": .519,
               "top_y_negative": .513, "top_y_positive": .519,
               "wide_y": 2.0 / 3.0, "top_z": 1.0, "positive_axis_bend": 0.0}
    if args.vertical_steps < 0:
        parser.error("--vertical-steps must be nonnegative")
    if args.vertical_steps:
        initial["vertical_steps"] = args.vertical_steps
    fitted = fit(train, args.vertical_steps)
    report = {"model": "separable_equal_power_rows_and_layers",
              "initial_parameters": initial, "fitted_parameters": fitted,
              "train_before": errors(train, initial), "train_after": errors(train, fitted)}
    if args.holdout:
        holdout = load_reference(args.holdout, args.channel_map)
        report["holdout"] = errors(holdout, fitted)
    args.output.write_text(json.dumps(report, indent=2) + "\n")
    print(args.output)


if __name__ == "__main__":
    main()
