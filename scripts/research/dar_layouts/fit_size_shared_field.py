#!/usr/bin/env python3
"""Research a single speaker-geometry gain function shared by all output channels."""

import argparse
import json
import math
from pathlib import Path

import numpy as np
import torch

from integer_room_model import point_gain

SPEAKERS = np.array([
    [-1.0, 1.0, 0.0], [1.0, 1.0, 0.0], [0.0, 1.0, 0.0],
    [-1.0, 0.0, 0.0], [1.0, 0.0, 0.0],
    [-1.0, -1.0, 0.0], [1.0, -1.0, 0.0],
    [-80 / 155, 80 / 155, 1.0], [80 / 155, 80 / 155, 1.0],
    [-80 / 155, -80 / 155, 1.0], [80 / 155, -80 / 155, 1.0],
])
CHANNELS = [0, 1, 2, 4, 5, 6, 7, 8, 9, 10, 11]


def features(xyz: list[float], size: float) -> np.ndarray:
    source = np.array(xyz, dtype=np.float64)
    point = np.array(point_gain("7.1.4", tuple(source)))[CHANNELS]
    rows = []
    for speaker, point_value in zip(SPEAKERS, point):
        position = source.copy()
        node = speaker.copy()
        if node[0] > 0.0:
            position[0] = -position[0]
            node[0] = -node[0]
        elif node[0] == 0.0:
            position[0] = abs(position[0])
        relative = position - node
        rows.append([
            *position, size, *node, *relative, *np.abs(relative), point_value,
            float(node[0] == 0.0), float(node[2] > 0.0),
            float(np.linalg.norm(position)), 1.0 - abs(position[0]),
            1.0 - position[1], 1.0 + position[1], 1.0 - position[2],
        ])
    return np.array(rows)


def append_data(rows: list[dict], path: Path, kind: str, weight: float) -> None:
    report = json.loads(path.read_text())["layouts"]["7.1.4"]
    for item in report["points"]:
        gains = item["signed_direct_gain"] if kind == "warm" else (
            item["signed_pure_gain"] if kind == "spatial" else item["coherent_gain"])
        rows.append({"label": item["label"], "xyz": item["xyz"], "size": item["size"],
                     "gains": np.maximum(np.array(gains)[CHANNELS], 0.0),
                     "weight": weight, "source": str(path)})


class Field(torch.nn.Module):
    def __init__(self, count: int):
        super().__init__()
        self.layers = torch.nn.Sequential(
            torch.nn.Linear(count, 128), torch.nn.SiLU(),
            torch.nn.Linear(128, 128), torch.nn.SiLU(),
            torch.nn.Linear(128, 128), torch.nn.SiLU(),
            torch.nn.Linear(128, 128), torch.nn.SiLU(),
            torch.nn.Linear(128, 1),
        )

    def forward(self, values: torch.Tensor) -> torch.Tensor:
        return values[:, :, 13] + values[:, :, 3] * self.layers(values).squeeze(-1)


def main() -> None:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--size-train", type=Path, required=True)
    parser.add_argument("--size-holdout", type=Path, required=True)
    parser.add_argument("--warm-bank", type=Path, required=True)
    parser.add_argument("--spatial-train", type=Path, required=True)
    parser.add_argument("--geometry-validation", type=Path, required=True)
    parser.add_argument("--include-geometry-in-train", action="store_true")
    parser.add_argument("--random-validation", type=Path)
    parser.add_argument("--output", type=Path, required=True)
    args = parser.parse_args()
    if args.output.exists():
        parser.error("output exists")
    torch.manual_seed(0x53495A45)
    torch.set_num_threads(1)
    train = []
    append_data(train, args.size_train, "short", 1.0)
    append_data(train, args.size_holdout, "short", 1.0)
    append_data(train, args.warm_bank, "warm", 4.0)
    append_data(train, args.spatial_train, "spatial", 2.0)
    if args.include_geometry_in_train:
        append_data(train, args.geometry_validation, "warm", 4.0)
    for x in (-1.0, -0.5, 0.0, 0.5, 1.0):
        for y in (-1.0, -0.5, 0.0, 0.5, 1.0):
            for z in (0.0, 0.5, 1.0):
                xyz = [x, y, z]
                train.append({"label": f"point_anchor_{x}_{y}_{z}", "xyz": xyz, "size": 0.0,
                              "gains": np.array(point_gain("7.1.4", tuple(xyz)))[CHANNELS],
                              "weight": 3.0, "source": "point-kernel"})
    validation = []
    if args.random_validation is not None:
        append_data(validation, args.random_validation, "spatial", 1.0)
    else:
        append_data(validation, args.geometry_validation, "warm", 1.0)
    xs = torch.tensor(np.array([features(item["xyz"], item["size"]) for item in train]),
                      dtype=torch.float32)
    ys = torch.tensor(np.array([item["gains"] for item in train]), dtype=torch.float32)
    vs = torch.tensor(np.array([features(item["xyz"], item["size"]) for item in validation]),
                      dtype=torch.float32)
    weights = torch.tensor(np.array([item["weight"] for item in train]), dtype=torch.float32)
    model = Field(xs.shape[-1])
    optimizer = torch.optim.AdamW(model.parameters(), lr=0.003, weight_decay=1e-7)
    best = None
    for step in range(15000):
        optimizer.zero_grad()
        predicted = model(xs)
        relative = (torch.linalg.vector_norm(predicted - ys, dim=1) /
                    torch.clamp(torch.linalg.vector_norm(ys, dim=1), min=1e-5))
        loss = torch.sum(weights * relative ** 2) / torch.sum(weights)
        loss.backward()
        optimizer.step()
        if step % 100 == 0:
            value = float(loss.detach())
            if best is None or value < best[0]:
                best = (value, {name: value.detach().clone() for name, value in model.state_dict().items()})
    model.load_state_dict(best[1])
    with torch.no_grad():
        trained = model(xs).numpy()
        predicted = model(vs).numpy()
    def scores(rows: list[dict], fitted: np.ndarray) -> list[dict]:
        return [{"label": row["label"], "size": row["size"], "source": row["source"],
                 "relative_l2": float(np.linalg.norm(a - row["gains"]) /
                                      max(np.linalg.norm(row["gains"]), 1e-12))}
                for row, a in zip(rows, fitted)]
    train_scores = scores(train, trained)
    valid_scores = scores(validation, predicted)
    report = {"model": "shared source-and-speaker geometry field; no per-channel parameters",
              "speaker_positions": SPEAKERS.tolist(), "feature_count": xs.shape[-1],
              "network": {name: value.numpy().tolist() for name, value in best[1].items()},
              "training": train_scores, "validation": valid_scores,
              "train_max_relative_l2": max(item["relative_l2"] for item in train_scores),
              "validation_mean_relative_l2": float(np.mean([item["relative_l2"] for item in valid_scores])),
              "validation_max_relative_l2": max(item["relative_l2"] for item in valid_scores)}
    args.output.write_text(json.dumps(report, indent=2) + "\n")
    print(args.output)


if __name__ == "__main__":
    main()
