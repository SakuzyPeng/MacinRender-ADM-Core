#!/usr/bin/env python3
"""Small 96 kHz SOFA-bank fixtures, independent of private HRTF assets."""
from pathlib import Path
import hashlib
import json
import shutil
import h5py
import numpy as np

root = Path(__file__).resolve().parents[1] / "fixtures/sofa/distance-96"
root.mkdir(exist_ok=True)
layers = []
for name, radius in [("near", .2), ("far", 1.5)]:
    path = root / f"{name}.sofa"
    shutil.copyfile(root.parent / "distance" / path.name, path)
    with h5py.File(path, "r+", libver="latest") as sofa:
        sofa["Data.SamplingRate"][:] = 96000
        sofa.create_dataset("MRADMPointOrigin", data=np.array([0, 0, 0, 0, 1, 2], dtype="<f8"))
        sofa.create_dataset("MRADMPointOriginHighBand", data=np.array([1, 1, 1, 1, 1, 2], dtype="<f8"))
    payload = path.read_bytes()
    layers.append(dict(radius_m=radius, directions=6, primary=path.name,
                       files={"HRIR_DFEQ100": dict(path=path.name, bytes=len(payload),
                                                 sha256=hashlib.sha256(payload).hexdigest())}))
index = dict(schema="mradm.sofa-bank", version=1, primary_variant="HRIR_DFEQ100",
             sampling_rate_hz=96000, radii_m=[.2, 1.5], layers=layers)
(root / "distance_index.json").write_text(json.dumps(index, indent=2) + "\n", encoding="utf-8")
