#!/usr/bin/env python3
"""Small synthetic distance fixtures; normal tests need no HDF5 Python runtime."""
from pathlib import Path
import hashlib
import json
import shutil

import h5py
import numpy as np

root = Path(__file__).resolve().parents[1] / "fixtures/sofa/distance"
root.mkdir(exist_ok=True)
layers = []
for name, radius, gain, shift in [("near", 0.2, 2., 4), ("far", 1.5, 1., 0),
                                   ("bad-grid", 0.2, 2., 4), ("bad-ears", 0.2, 2., 4)]:
    path = root / f"{name}.sofa"
    shutil.copyfile(root.parent / "simple.sofa", path)
    with h5py.File(path, "r+", libver="latest") as f:
        positions = f["SourcePosition"][:]
        positions[:, 2] = radius
        if name == "bad-grid":
            positions[[0, 1]] = positions[[1, 0]]
        f["SourcePosition"][:] = positions
        f["Data.IR"][:] = np.roll(f["Data.IR"][:] * gain, shift, axis=-1)
        ears = f.create_dataset("ReceiverPosition", data=np.array([[0., .09, 0.], [0., -.09, 0.]]))
        ears.attrs["Type"] = np.bytes_("cartesian")
        ears.attrs["Units"] = np.bytes_("metre")
        if name == "bad-ears":
            ears[:] = ears[:][::-1]
    if name in ("near", "far"):
        payload = path.read_bytes()
        layers.append(dict(radius_m=radius, reference_layer=name == "far", directions=6,
                           primary=path.name, provenance=dict(original=6, virtual_dvf=0),
                           files=dict(HRIR_DFEQ100=dict(path=path.name, bytes=len(payload),
                                                      sha256=hashlib.sha256(payload).hexdigest()))))
manifest = dict(schema="mradm.hrtf-collection", version=1, collection="Synthetic distance fixture",
                primary_variant="HRIR_DFEQ100", sampling_rate_hz=48000,
                directions_per_distance=6, radii_m=[.2, 1.5], layers=layers)
(root / "distance_index.json").write_text(json.dumps(manifest, indent=2) + "\n")

# A legal dense-attribute heap whose first 1024-byte direct block is full.
# Only attribute lengths reproduce the layout; no personal data is retained.
lengths = [("Conventions", 4), ("Version", 3), ("SOFAConventions", 19),
           ("SOFAConventionsVersion", 3), ("APIName", 37), ("APIVersion", 73),
           ("ApplicationName", 9), ("ApplicationVersion", 5), ("AuthorContact", 0),
           ("Comment", 470), ("DataType", 3), ("History", 135), ("License", 50),
           ("Organization", 0), ("References", 0), ("RoomType", 10), ("Origin", 0),
           ("DateCreated", 19), ("DateModified", 19), ("Title", 29),
           ("DatabaseName", 0), ("ListenerShortName", 0), ("_NCProperties", 34)]
with h5py.File(root / "full-heap.sofa", "w", libver="v108", track_order=True) as f:
    required = dict(Conventions="SOFA", Version="1.0", SOFAConventions="SimpleFreeFieldHRIR",
                    SOFAConventionsVersion="1.0", DataType="FIR", RoomType="free field")
    for name, length in lengths:
        value = required.get(name, "x" * length)
        kind = h5py.h5t.C_S1.copy()
        kind.set_size(max(1, length))
        kind.set_strpad(h5py.h5t.STR_NULLTERM)
        attr = h5py.h5a.create(f.id, name.encode(), kind, h5py.h5s.create(h5py.h5s.SCALAR))
        attr.write(np.array(value.encode(), dtype=f"S{max(1, length)}"), mtype=kind)
    with h5py.File(root / "far.sofa") as source:
        for name in ["SourcePosition", "Data.IR", "Data.SamplingRate", "Data.Delay", "ReceiverPosition"]:
            dataset = f.create_dataset(name, data=source[name][:])
            for attr in ["Units", "Type"]:
                if attr in source[name].attrs:
                    dataset.attrs[attr] = source[name].attrs[attr]
    for i in range(4):
        f.create_dataset(f"dimension{i}", data=[0.])
