#!/usr/bin/env python3
"""Regenerate equivalent SOFA storage variants; tests need no Python packages."""
from pathlib import Path
import shutil

import h5py
import numpy as np


root = Path(__file__).resolve().parents[1] / "fixtures/sofa"
variants = {
    "chunked": {},
    "shuffle": {"shuffle": True},
    "deflate": {"compression": "gzip"},
    "shuffle-deflate": {"shuffle": True, "compression": "gzip"},
    "skipped": {"shuffle": True, "compression": "gzip"},
    "unsupported": {"compression": "gzip", "fletcher32": True},
}
for name, filters in variants.items():
    path = root / f"filters-{name}.sofa"
    shutil.copyfile(root / "simple.sofa", path)
    with h5py.File(path, "r+", libver=("v108", "v108")) as file:
        for key, chunks in [("Data.IR", (1, 1, 16)), ("SourcePosition", (3, 3)),
                            ("Data.SamplingRate", (1,))]:
            original = file[key]
            values = original[...]
            if key == "Data.IR":
                # Dense coefficients expose silent byte transposition, not just
                # rejection of an accidentally zero/nonfinite decoded response.
                values += 0.125
            attributes = dict(original.attrs)
            scales = [list(dimension.values()) for dimension in original.dims]
            for axis, dimensions in enumerate(scales):
                for dimension in dimensions:
                    original.dims[axis].detach_scale(dimension)
            del file[key]
            dataset = file.create_dataset(key, data=values, chunks=chunks, track_order=True, **filters)
            for attribute, value in attributes.items():
                if attribute != "DIMENSION_LIST":
                    dataset.attrs[attribute] = value
            for axis, dimensions in enumerate(scales):
                for dimension in dimensions:
                    dataset.dims[axis].attach_scale(dimension)
            if name == "skipped":
                for start in np.ndindex(*(s // c for s, c in zip(values.shape, chunks))):
                    offset = tuple(i * c for i, c in zip(start, chunks))
                    selection = tuple(slice(i, i + c) for i, c in zip(offset, chunks))
                    dataset.id.write_direct_chunk(offset, values[selection].tobytes(), filter_mask=3)
