#!/usr/bin/env python3
"""Paired treatment checks for the zoned reference candidate using an ideal omni receiver."""
import argparse
import copy
from concurrent.futures import ProcessPoolExecutor
import hashlib
import json
from pathlib import Path
import shutil
import subprocess
import tempfile

import numpy as np

import ray_energy as ray
from room_surfaces import SurfaceMap, UV


def band_observation(room, late, directions, band, jobs):
    config = ray.validate(late, room)
    sources = ray.source_positions(config, room, Path(__file__).parent)
    tasks = [(config, room, directions, p, i, [band]) for i, p in enumerate(sources)]
    energy = np.zeros((len(directions), ray.bin_count(config)))
    with ProcessPoolExecutor(max_workers=jobs) as pool:
        for part in pool.map(ray.trace_task, tasks):
            energy += part["energy"][:, 0]
    width = config["histogram_seconds"]
    result = ray.decay_times(energy.sum(axis=0), width)
    times = (np.arange(energy.shape[1]) + .5) * width
    result["windows"] = []
    for lo, hi in [(0., .05), (.05, .15), (.15, .35), (.35, config["duration_seconds"]), (.05, .35)]:
        if hi <= lo:
            continue
        weights = energy[:, (times >= lo) & (times < hi)].sum(axis=1)
        window_energy = weights.sum()
        if window_energy > 0:
            weights /= window_energy
            tensor = np.einsum("d,di,dj->ij", weights, directions, directions)
            result["windows"].append(dict(seconds=[lo, hi], energy_fraction=float(window_energy / energy.sum()),
                                          tensor=tensor.tolist(),
                                          y_minus_z=float(tensor[1, 1] - tensor[2, 2])))
    return result


def panel_beam(room, seed, band=4):
    """Illuminate the actual rear panel at normal incidence and measure the outgoing energy tensor."""
    layout = room["surface_model"]
    surfaces = SurfaceMap(layout, room["dimensions"])
    wall = 0
    patch = next(p for p in layout["faces"][wall]["regions"] if p["material"] == "rear-fan")
    point = np.zeros((1, 3))
    point[0, list(UV[0])] = np.mean(patch["polygon"], axis=0)
    slot = surfaces.bindings(np.array([wall]), point)[0]
    if not surfaces.fans[slot]:
        raise ValueError("beam misses the rear diffuser")
    count = 65536
    rng = np.random.default_rng(seed)
    scattered = rng.random(count) < surfaces.scattering[slot, band]
    rays = np.zeros((count, 3))
    rays[:, 0] = 1.
    n = int(scattered.sum())
    rays[scattered] = surfaces.scatter_fan(-rays[scattered], np.full(n, slot), np.zeros(n, int),
                                         np.zeros(n, bool), rng.random(n))
    tensor = rays.T @ rays / count
    return dict(tensor=tensor.tolist(), reflected_fraction=float(1. - surfaces.absorption[slot, band]),
                y_minus_z=float(tensor[1, 1] - tensor[2, 2]))


def measure_wall(probe, room, scratch, index):
    output = scratch / f"wall-{index}.f32"
    meta = json.loads(subprocess.check_output([str(probe), str(room), "0", "30", "2.1", str(output)],
                                              text=True, encoding="utf-8"))
    return next(r for r in meta["early_reflections"] if r["walls"] == [0, 0, 1, 0, 0, 0])


def main():
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument("--room", type=Path, required=True)
    ap.add_argument("--probe", type=Path, required=True)
    ap.add_argument("--output", type=Path, required=True)
    ap.add_argument("--rays", type=int, default=100000)
    ap.add_argument("--jobs", type=int, default=2)
    args = ap.parse_args()
    root = args.room.parent
    base = json.loads(args.room.read_text(encoding="utf-8"))
    if base["kind"] != "hybrid-shoebox-v2":
        raise ValueError("expected the zoned reference room")
    definition = json.loads((root / "definition.json").read_text(encoding="utf-8"))
    variants = {k: copy.deepcopy(base) for k in ("base", "rotated", "traps_removed", "panel_moved")}
    for wall in variants["rotated"]["surface_model"]["faces"]:
        for p in wall["regions"]:
            if p["material"] == "rear-fan":
                p["angle_deg"] += 90
    for wall in variants["traps_removed"]["surface_model"]["faces"][:4]:
        wall["regions"] = [p for p in wall["regions"] if p.get("role") != "bass-trap"]
    moved = next(p for p in variants["panel_moved"]["surface_model"]["faces"][2]["regions"]
                 if p.get("role") == "side-reflection")
    shift = min(pt[0] for pt in moved["polygon"])
    moved["polygon"] = [[u-shift, v] for u,v in moved["polygon"]]
    def checked_file(entry):
        name = entry["file"]
        if not name or name in (".", "..") or any(c in name for c in "/\\:\0"):
            raise ValueError("invalid room asset name")
        data = (root/name).read_bytes()
        if hashlib.sha256(data).hexdigest() != entry["sha256"]:
            raise ValueError(f"room asset integrity failure: {name}")
        return data
    dr = base["assets"]["late_directions"]
    directions = np.frombuffer(checked_file(dr), dtype="<f8").reshape(dr["shape"])
    with tempfile.TemporaryDirectory() as t:
        tmp = Path(t)
        # Isolate the early path by holding all FIR bytes fixed. The all-ray
        # comparisons below independently retrace the affected frequency band.
        for entry in base["assets"].values():
            checked_file(entry)
            shutil.copyfile(root/entry["file"], tmp/entry["file"])
        m = tmp/"room.json"
        m.write_text(json.dumps(variants["panel_moved"]), encoding="utf-8")
        initial = measure_wall(args.probe.resolve(), args.room.resolve(), tmp, 0)
        changed = measure_wall(args.probe.resolve(), m, tmp, 1)
    early_ok = (initial["surface_hits"][0]["material"] != changed["surface_hits"][0]["material"]
                and initial["delay_after_direct_ms"] == changed["delay_after_direct_ms"]
                and initial["band_gains"] != changed["band_gains"])
    report = dict(room_hash=hashlib.sha256(args.room.read_bytes()).hexdigest(), rays_per_source=args.rays,
                  early_wall=dict(base=initial, moved=changed, passed=early_ok), seeds=[])
    for seed in (3, 11):
        late = dict(definition["late"], seed=seed, rays=args.rays)
        low = {k: band_observation(variants[k], late, directions, 0, args.jobs) for k in ("base","traps_removed")}
        high = {k: band_observation(variants[k], late, directions, 4, args.jobs) for k in ("base","rotated")}
        beam = {k: panel_beam(variants[k], seed) for k in ("base", "rotated")}
        contrasts = [dict(seconds=p["seconds"], y_minus_z_change=q["y_minus_z"] - p["y_minus_z"])
                     for p, q in zip(high["base"]["windows"], high["rotated"]["windows"])]
        report["seeds"].append(dict(seed=seed, low=low, high=high, panel_beam=beam,
            trap_t20_reduction_s=low["traps_removed"]["t20"] - low["base"]["t20"],
            rotation_contrasts=contrasts))
    report["traps_shorten_low_band"] = all(s["trap_t20_reduction_s"] > 0 for s in report["seeds"])
    report["fan_rotates"] = all(s["panel_beam"]["base"]["y_minus_z"] > 0
                                and s["panel_beam"]["rotated"]["y_minus_z"] < 0
                                and abs(s["panel_beam"]["base"]["y_minus_z"] + s["panel_beam"]["rotated"]["y_minus_z"]) < 1e-12
                                for s in report["seeds"])
    # Predeclared receiver window. Keep the dimmer tail windows visible separately.
    changes = [next(row["y_minus_z_change"] for row in s["rotation_contrasts"]
                    if row["seconds"] == [.05, .35]) for s in report["seeds"]]
    report["receiver_rotation"] = dict(seconds=[.05, .35], paired_changes=changes,
        same_sign=bool(np.prod(changes) > 0), seed_range=float(np.ptp(changes)))
    report["passed"] = bool(early_ok and report["traps_shorten_low_band"] and report["fan_rotates"]
                            and report["receiver_rotation"]["same_sign"]
                            and min(abs(x) for x in changes) > report["receiver_rotation"]["seed_range"])
    args.output.write_text(json.dumps(report,ensure_ascii=False,indent=2,allow_nan=False)+"\n",encoding="utf-8")
    if not report["passed"]:
        raise SystemExit(1)


if __name__ == "__main__":
    main()
