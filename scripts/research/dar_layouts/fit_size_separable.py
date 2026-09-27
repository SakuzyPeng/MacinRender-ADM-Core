#!/usr/bin/env python3
"""Fit topology-constrained 1D room interpolation curves at one measured size."""

import argparse
import json
import math
from pathlib import Path

import numpy as np
import torch

GRID = np.array([-1.0, -0.5, 0.0, 0.5, 1.0])
ZGRID = np.array([0.0, 0.25, 0.5, 0.75, 1.0])
CGRID = np.array([0.0, 0.5, 1.0])
FACTOR_GRID = {"xr": GRID, "xf": GRID, "xc": CGRID, "xu": GRID,
               "yr": GRID, "ys": GRID, "yf": GRID, "uyr": GRID, "uyf": GRID,
               "zm": ZGRID, "zu": ZGRID}


def weights(values: np.ndarray, grid: np.ndarray) -> torch.Tensor:
    result = np.zeros((len(values), len(grid)))
    for index, value in enumerate(values):
        value = float(np.clip(value, grid[0], grid[-1]))
        upper = int(np.searchsorted(grid, value, side="right"))
        if upper == 0:
            result[index, 0] = 1.0
        elif upper >= len(grid):
            result[index, -1] = 1.0
        else:
            t = (value - grid[upper - 1]) / (grid[upper] - grid[upper - 1])
            result[index, upper - 1] = 1.0 - t
            result[index, upper] = t
    return torch.tensor(result, dtype=torch.float64)


def initial_curve(name: str, grid: np.ndarray) -> np.ndarray:
    value = []
    for x in grid:
        if name == "xr":
            v = math.cos(math.pi * (x + 1.0) / 4)
        elif name == "xf":
            v = math.cos(math.pi * (x + 1.0) / 2) if x < 0 else 0.0
        elif name == "xc":
            v = math.cos(math.pi * x / 2)
        elif name == "xu":
            v = math.cos(math.pi * np.clip((x + 80 / 155) / (160 / 155), 0, 1) / 2)
        elif name == "yr":
            v = math.cos(math.pi * (x + 1.0) / 2) if x < 0 else 0.0
        elif name == "ys":
            v = math.sin(math.pi * (x + 1.0) / 2) if x < 0 else math.cos(math.pi * x / 2)
        elif name == "yf":
            v = math.sin(math.pi * x / 2) if x > 0 else 0.0
        elif name == "uyr":
            v = math.cos(math.pi * np.clip((x + 80 / 155) / (160 / 155), 0, 1) / 2)
        elif name == "uyf":
            v = math.sin(math.pi * np.clip((x + 80 / 155) / (160 / 155), 0, 1) / 2)
        elif name == "zm":
            v = math.cos(math.pi * x / 2)
        else:
            v = math.sin(math.pi * x / 2)
        value.append(max(0.015, v))
    return np.array(value)


def inputs(rows: list[dict]) -> dict[str, torch.Tensor]:
    xyz = np.array([row["xyz"] for row in rows])
    x, y, z = xyz[:, 0], xyz[:, 1], xyz[:, 2]
    coords = {"xr": x, "xr_right": -x, "xf": x, "xf_right": -x,
              "xc": np.abs(x), "xu": x, "xu_right": -x,
              "yr": y, "ys": y, "yf": y, "uyr": y, "uyf": y,
              "zm": z, "zu": z}
    return {name: weights(value, FACTOR_GRID[name.replace("_right", "")])
            for name, value in coords.items()}


def forward(parameters: dict[str, torch.Tensor], matrix: dict[str, torch.Tensor]) -> torch.Tensor:
    factors = {name: matrix[name] @ torch.nn.functional.softplus(parameters[name.replace("_right", "")])
               for name in matrix}
    result = torch.zeros((len(next(iter(matrix.values()))), 12), dtype=torch.float64)
    result[:, 0] = factors["zm"] * factors["yf"] * factors["xf"]
    result[:, 1] = factors["zm"] * factors["yf"] * factors["xf_right"]
    result[:, 2] = factors["zm"] * factors["yf"] * factors["xc"]
    result[:, 4] = factors["zm"] * factors["ys"] * factors["xr"]
    result[:, 5] = factors["zm"] * factors["ys"] * factors["xr_right"]
    result[:, 6] = factors["zm"] * factors["yr"] * factors["xr"]
    result[:, 7] = factors["zm"] * factors["yr"] * factors["xr_right"]
    result[:, 8] = factors["zu"] * factors["uyf"] * factors["xu"]
    result[:, 9] = factors["zu"] * factors["uyf"] * factors["xu_right"]
    result[:, 10] = factors["zu"] * factors["uyr"] * factors["xu"]
    result[:, 11] = factors["zu"] * factors["uyr"] * factors["xu_right"]
    return result


def append_data(rows: list[dict], path: Path, kind: str, size: float, weight: float) -> None:
    report = json.loads(path.read_text())["layouts"]["7.1.4"]
    for item in report.get("points", []):
        if abs(item["size"] - size) > 1e-8:
            continue
        gains = item["signed_direct_gain"] if kind == "warm" else (
            item["signed_pure_gain"] if kind == "spatial" else item["coherent_gain"])
        rows.append({"label": item["label"], "xyz": item["xyz"], "gains": gains,
                     "weight": weight, "source": str(path)})


def main() -> None:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--size", type=float, default=0.25)
    parser.add_argument("--size-train", type=Path, required=True)
    parser.add_argument("--warm-bank", type=Path, required=True)
    parser.add_argument("--spatial-train", type=Path, required=True)
    parser.add_argument("--geometry-validation", type=Path, required=True)
    parser.add_argument("--output", type=Path, required=True)
    args = parser.parse_args()
    if args.output.exists():
        parser.error("output exists")
    torch.set_num_threads(1)
    rows = []
    append_data(rows, args.size_train, "short", args.size, 1.0)
    append_data(rows, args.warm_bank, "warm", args.size, 3.0)
    append_data(rows, args.spatial_train, "spatial", args.size, 2.0)
    validation = []
    append_data(validation, args.geometry_validation, "warm", args.size, 1.0)
    if not rows or not validation:
        raise ValueError("training and validation points are required")
    train_inputs = inputs(rows)
    valid_inputs = inputs(validation)
    targets = torch.tensor([item["gains"] for item in rows], dtype=torch.float64)
    importance = torch.tensor([item["weight"] for item in rows], dtype=torch.float64)
    norm = torch.maximum(torch.linalg.vector_norm(targets, dim=1), torch.tensor(1e-9))
    parameters = {}
    original = {}
    for name, grid in FACTOR_GRID.items():
        values = initial_curve(name, grid)
        original[name] = torch.tensor(values, dtype=torch.float64)
        parameters[name] = torch.nn.Parameter(torch.tensor(np.log(np.expm1(values)), dtype=torch.float64))
    optimizer = torch.optim.Adam(parameters.values(), lr=0.03)
    best = None
    for step in range(5000):
        optimizer.zero_grad()
        prediction = forward(parameters, train_inputs)
        error = torch.linalg.vector_norm(prediction - targets, dim=1) / norm
        loss = torch.sum(importance * error * error) / torch.sum(importance)
        smooth = sum(torch.mean(torch.diff(torch.nn.functional.softplus(value), n=2) ** 2)
                     for value in parameters.values())
        anchor = sum(torch.mean((torch.nn.functional.softplus(value) - original[name]) ** 2)
                     for name, value in parameters.items())
        objective = loss + 0.0003 * smooth + 0.00002 * anchor
        objective.backward()
        optimizer.step()
        if step % 100 == 0:
            score = float(loss.detach())
            if best is None or score < best[0]:
                best = (score, {name: torch.nn.functional.softplus(value).detach().clone()
                                for name, value in parameters.items()})
    for name, value in best[1].items():
        parameters[name] = torch.nn.Parameter(torch.log(torch.expm1(value)))
    with torch.no_grad():
        train_prediction = forward(parameters, train_inputs).numpy()
        valid_prediction = forward(parameters, valid_inputs).numpy()
    def scores(items: list[dict], predicted: np.ndarray) -> list[dict]:
        answer = []
        for row, gains in zip(items, predicted):
            reference = np.array(row["gains"])
            error = float(np.linalg.norm(gains - reference) / np.linalg.norm(reference))
            answer.append({"label": row["label"], "source": row["source"], "relative_l2": error})
        return answer
    train_scores = scores(rows, train_prediction)
    valid_scores = scores(validation, valid_prediction)
    report = {"size": args.size, "model": "separable room layer/row/X interpolation",
              "factor_grid": {name: grid.tolist() for name, grid in FACTOR_GRID.items()},
              "factors": {name: torch.nn.functional.softplus(value).detach().numpy().tolist()
                          for name, value in parameters.items()},
              "train": train_scores, "validation": valid_scores,
              "train_max_relative_l2": max(item["relative_l2"] for item in train_scores),
              "validation_max_relative_l2": max(item["relative_l2"] for item in valid_scores),
              "validation_mean_relative_l2": float(np.mean([item["relative_l2"] for item in valid_scores]))}
    args.output.write_text(json.dumps(report, indent=2) + "\n")
    print(args.output)


if __name__ == "__main__":
    main()
