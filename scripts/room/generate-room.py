#!/usr/bin/env python3
"""Generate a finite hybrid-shoebox-v1 package from a room definition.

The template package provides verified band synthesis, propagation kernels, EQ
and world directions. The new seeded octave-decay late field is a new acoustic
preset; it does not inherit the reference room's validation claims.
"""
from __future__ import annotations

import argparse
import hashlib
import json
from pathlib import Path
import shutil

import numpy as np
import scipy
from scipy.signal import fftconvolve


def read_asset(root, entry):
    name = entry["file"]
    if not name or name in (".", "..") or any(c in name for c in "/\\:\0"):
        raise ValueError("asset name must be a flat relative name")
    payload = (root / name).read_bytes()
    if len(payload) != entry["bytes"] or hashlib.sha256(payload).hexdigest() != entry["sha256"]:
        raise ValueError(f"asset integrity failure: {name}")
    array = np.frombuffer(payload, dtype=entry["dtype"]).reshape(entry["shape"])
    if array.dtype.kind == "f" and not np.isfinite(array).all():
        raise ValueError(f"nonfinite array: {name}")
    return array


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--template-package", type=Path, required=True, help="existing room.json")
    parser.add_argument("--definition", type=Path, required=True, help="copy/edit room-template.json")
    parser.add_argument("--output", type=Path, required=True, help="new directory")
    parser.add_argument("--include-hrir", action="store_true", help="copy the default HRIR binding into the new package")
    args = parser.parse_args()
    base = json.loads(args.template_package.read_text())
    definition_bytes = args.definition.read_bytes()
    definition = json.loads(definition_bytes)
    if (base.get("schema"), base.get("version"), base.get("kind"), base.get("sample_rate")) != (
            "mradm.room", 1, "hybrid-shoebox-v1", 48000):
        raise ValueError("unsupported template package")
    if definition.get("schema") != "mradm.room-definition" or definition.get("version") != 1:
        raise ValueError("unsupported room definition")
    fields = ("id", "name", "dimensions", "listener", "speed_of_sound", "reference_distance", "minimum_distance",
              "boundary_clearance", "reflection_order", "absorption", "early_fade_seconds", "late_predelay_seconds",
              "late_energy")
    for key in fields:
        base[key] = definition[key]
    dimensions = np.asarray(base["dimensions"], dtype=np.float64)
    listener = np.asarray(base["listener"], dtype=np.float64)
    absorption = np.asarray(base["absorption"], dtype=np.float64)
    if dimensions.shape != (3,) or listener.shape != (3,) or absorption.shape != (6, 8):
        raise ValueError("geometry or six-wall/eight-band absorption shape")
    if (not np.isfinite(dimensions).all() or not np.isfinite(listener).all() or
            not np.isfinite(absorption).all() or np.any(dimensions < 1) or np.any(dimensions > 50) or
            np.any(absorption < 0) or np.any(absorption > 1)):
        raise ValueError("invalid geometry/material values")
    margin = float(base["minimum_distance"]) + float(base["boundary_clearance"])
    if np.any(listener < margin) or np.any(listener > dimensions - margin):
        raise ValueError("listener must have clearance for the minimum source radius")
    late = definition["late"]
    if late["generator"] != "octave-decay-noise-v1":
        raise ValueError("unsupported offline late-field generator")
    rt = np.asarray(late["rt60_seconds"], dtype=np.float64)
    energy = np.asarray(late["band_energy"], dtype=np.float64)
    if rt.shape != (8,) or energy.shape != (8,) or not np.isfinite(rt).all() or not np.isfinite(energy).all():
        raise ValueError("late RT60 and energy require eight finite values")
    if np.any(rt < 0.05) or np.any(rt > 3) or np.any(energy < 0) or not energy.sum() > 0:
        raise ValueError("invalid late decay/energy")
    duration = float(late["duration_seconds"])
    if not np.isfinite(duration) or not 0.1 <= duration <= 10 or duration < rt.max():
        raise ValueError("finite tail duration must be 0.1..10 seconds and at least the longest RT60")
    source = args.template_package.parent
    arrays = {name: read_asset(source, entry) for name, entry in base["assets"].items()}
    bands = arrays["bands"]
    directions = arrays["late_directions"]
    frames = int(round(duration * 48000))
    rng = np.random.Generator(np.random.PCG64(int(late["seed"])))
    time = np.arange(frames, dtype=np.float64) / 48000
    field = np.zeros((len(directions), frames), dtype=np.float64)
    # No feedback network is exported: all state has explicit finite support.
    for channel in range(len(directions)):
        for band in range(8):
            noise = rng.standard_normal(frames + bands.shape[1] - 1)
            filtered = fftconvolve(noise, bands[band], mode="valid")
            field[channel] += filtered * np.exp(-np.log(1000.) * time / rt[band]) * np.sqrt(energy[band])
    fade = min(240, frames)
    field[:, -fade:] *= np.linspace(1., 0., fade, dtype=np.float64)
    # One shared calibration of the late field, not per-direction/ear HRIR normalization.
    mean_energy = np.mean(np.sum(field * field, axis=1))
    if not np.isfinite(mean_energy) or mean_energy <= 0:
        raise ValueError("late field has no finite energy")
    field /= np.sqrt(mean_energy)
    args.output.mkdir(parents=True, exist_ok=False)
    for name, entry in base["assets"].items():
        if name != "late":
            shutil.copyfile(source / entry["file"], args.output / entry["file"])
    payload = np.asarray(field, dtype="<f4", order="C").tobytes()
    (args.output / "late.f32").write_bytes(payload)
    base["assets"]["late"] = dict(file="late.f32", dtype="<f4", shape=list(field.shape), bytes=len(payload),
                                    sha256=hashlib.sha256(payload).hexdigest())
    base["generator"] = "mradm-generate-room-v1"
    base["generation"] = dict(definition_sha256=hashlib.sha256(definition_bytes).hexdigest(),
                               numpy=np.__version__, scipy=scipy.__version__, late=late)
    base["validation_scope"] = "New preset. Resource integrity only; requires independent acoustic validation."
    if args.include_hrir:
        hrir_name = base["default_hrir"]
        if not hrir_name or hrir_name in (".", "..") or any(c in hrir_name for c in "/\\:\0"):
            raise ValueError("default HRIR manifest must be a flat relative name")
        hrir = json.loads((source / hrir_name).read_text())
        for entry in hrir["assets"].values():
            read_asset(source, entry)
            shutil.copyfile(source / entry["file"], args.output / entry["file"])
        shutil.copyfile(source / hrir_name, args.output / hrir_name)
    (args.output / "room.json").write_text(json.dumps(base, ensure_ascii=False, indent=2, allow_nan=False) + "\n")
    (args.output / "definition.json").write_bytes(definition_bytes)
    print(json.dumps(dict(room=str(args.output / "room.json"), hrir_included=args.include_hrir,
                         bytes=sum(p.stat().st_size for p in args.output.iterdir()))))


if __name__ == "__main__":
    main()
