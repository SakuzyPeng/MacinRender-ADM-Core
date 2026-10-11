#!/usr/bin/env python3
"""Package the frozen room and independent HRIR bank; never render a direction library.

Preparation-only dependencies: numpy, scipy, h5py, pyroomacoustics. Runtime reads
the resulting typed little-endian arrays without these dependencies.
"""
from __future__ import annotations

import argparse
import hashlib
import importlib.util
import json
from pathlib import Path
import sys

import numpy as np


def write_json(path: Path, value: dict) -> None:
    path.write_text(json.dumps(value, ensure_ascii=False, indent=2, allow_nan=False) + "\n")


def asset(root: Path, name: str, data: np.ndarray, dtype: str) -> dict:
    array = np.asarray(data, dtype=np.dtype(dtype), order="C")
    if array.dtype.kind == "f" and not np.isfinite(array).all():
        raise ValueError(f"nonfinite asset: {name}")
    payload = array.tobytes(order="C")
    (root / name).write_bytes(payload)
    return {"file": name, "dtype": array.dtype.str, "shape": list(array.shape),
            "sha256": hashlib.sha256(payload).hexdigest(), "bytes": len(payload)}


def load_module(name: str, path: Path):
    spec = importlib.util.spec_from_file_location(name, path)
    if spec is None or spec.loader is None:
        raise ValueError(f"cannot import {path.name}")
    module = importlib.util.module_from_spec(spec)
    sys.modules[name] = module
    spec.loader.exec_module(module)
    return module


def main() -> None:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--components", type=Path, required=True,
                        help="model_components directory containing shared_components.json")
    parser.add_argument("--reference", type=Path, required=True,
                        help="reference_layouts_9_1_6_22_2_v1 directory")
    parser.add_argument("--continuous-source", type=Path, required=True,
                        help="verified continuous_hrir.py")
    parser.add_argument("--output", type=Path, required=True,
                        help="new output directory (must not exist)")
    parser.add_argument("--update", action="store_true",
                        help="replace a bundle previously created by this generator")
    args = parser.parse_args()
    sources = json.loads((args.components / "shared_components.json").read_text())["files"]
    paths = {}
    for name, entry in sources.items():
        path = Path(entry["path"])
        if hashlib.sha256(path.read_bytes()).hexdigest() != entry["sha256"]:
            raise ValueError(f"frozen source hash mismatch: {name}")
        paths[name] = path
    model = json.loads((args.reference / "model.json").read_text())
    source_dir = args.reference / "source"
    sys.path.insert(0, str(source_dir))
    room_opt = load_module("room_opt", source_dir / "room_opt.py")
    continuous = load_module("room_pack_continuous", args.continuous_source)
    bank = continuous.ContinuousHrirBank(paths["HRIR.sofa"], paths["Nodes.txt"], paths["Elements.txt"])
    cfg = dict(model["geometry"])
    reference = room_opt.build(cfg, ("bank", 0, 0, 2.1), 0)
    bands = reference.octave_bands.synthesis(np.eye(8), min_phase=False)
    field = np.load(paths["late_world_field.npz"])
    eq = np.load(paths["common_room_eq.npy"])
    if args.update and args.output.exists():
        previous = json.loads((args.output / "room.json").read_text())
        if previous.get("generator") != "mradm-pack-reference-v1":
            raise ValueError("--update requires this generator's existing bundle")
    else:
        args.output.mkdir(parents=True, exist_ok=False)
    out = args.output
    hrir = {
        "schema": "mradm.hrir", "version": 1, "kind": "delay-aligned-v1",
        "sample_rate": 48000, "guard_samples": 96, "output_taps": 768,
        "fractional_taps": 81,
        "source_sofa_sha256": sources["HRIR.sofa"]["sha256"],
        "delay_estimation": "excess-phase slope, 300-1500 Hz; original response retained",
        "assets": {
            "impulses": asset(out, "hrir.f32", bank.ir, "<f4"),
            "directions": asset(out, "hrir-directions.f64", bank.unit, "<f8"),
            "faces": asset(out, "hrir-faces.u32", bank.faces, "<u4"),
            "delays": asset(out, "hrir-delays.f64", bank.delays, "<f8"),
        },
    }
    write_json(out / "hrir.json", hrir)
    propagation = np.zeros((1025, 81), dtype=np.float32)
    room_opt.pra.libroom.fractional_delay(
        propagation, np.arange(1025, dtype=np.float32) / 1024,
        room_opt.pra.constants.get("sinc_lut_granularity"), 1)
    room = {
        "schema": "mradm.room", "version": 1, "kind": "hybrid-shoebox-v1",
        "id": "reference-7x6x3.2-v1", "name": "Reference listening room",
        "sample_rate": 48000, "dimensions": cfg["dimensions"], "listener": cfg["listener"],
        "speed_of_sound": 344.02, "reference_distance": 2.1,
        "minimum_distance": 0.25, "boundary_clearance": 0.15,
        "reflection_order": 4, "absorption": [cfg["walls"] for _ in range(6)],
        "source_directivity": "cosine2-listener", "early_fade_seconds": [0.015, 0.025],
        "late_predelay_seconds": 0.018, "late_energy": model["late_energy"],
        "alignment_guard_samples": 40, "band_filter_offset": int(bands.shape[1] // 2),
        "default_hrir": "hrir.json", "generator": "mradm-pack-reference-v1",
        "assets": {
            "bands": asset(out, "bands.f64", bands, "<f8"),
            "late": asset(out, "late.f32", field["impulse_responses"], "<f4"),
            "late_directions": asset(out, "late-directions.f64", field["directions"], "<f8"),
            "eq": asset(out, "eq.f64", eq, "<f8"),
            "propagation_phases": asset(out, "propagation-phases.f32", propagation, "<f4"),
        },
        "validation_scope": "Frozen reference sources at one listener; arbitrary distances are not certified.",
    }
    write_json(out / "room.json", room)
    print(json.dumps({"room": str(out / "room.json"), "hrir": str(out / "hrir.json"),
                      "bytes": sum(p.stat().st_size for p in out.iterdir() if p.is_file())}))


if __name__ == "__main__":
    main()
