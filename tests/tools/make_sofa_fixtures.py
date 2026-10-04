#!/usr/bin/env python3
"""Regenerate small, project-owned SOFA fixtures (requires numpy and netCDF4).

These synthetic impulses have no external dataset licensing requirements.
Normal tests consume the committed files and do not need these Python packages.
"""
from pathlib import Path
import numpy as np
from netCDF4 import Dataset

root = Path(__file__).resolve().parents[1] / "fixtures/sofa"
root.mkdir(parents=True, exist_ok=True)
polar = np.array([[0, 0, 1], [90, 0, 1], [180, 0, 1], [-90, 0, 1], [0, 90, 1], [0, -90, 1]], dtype="f4")
cartesian = np.array([[1, 0, 0], [0, 1, 0], [-1, 0, 0], [0, -1, 0], [0, 0, 1], [0, 0, -1]], dtype="f8")
for convention, coordinates, compressed, rate, name in [
    ("SimpleFreeFieldHRIR", "spherical", False, 48000, "simple"),
    ("GeneralFIR", "cartesian", True, 44100, "general-compressed"),
]:
    with Dataset(root / (name + ".sofa"), "w", format="NETCDF4") as dataset:
        for key, size in dict(I=1, C=3, R=2, E=1, N=16, M=6).items():
            dataset.createDimension(key, size)
        dataset.Conventions = "SOFA"
        dataset.Version = "1.0"
        dataset.SOFAConventions = convention
        dataset.SOFAConventionsVersion = "1.0"
        dataset.DataType = "FIR"
        dataset.RoomType = "free field"
        dataset.ListenerShortName = "MacinRender synthetic"
        source = dataset.createVariable("SourcePosition", "f8", ("M", "C"))
        source.Type = coordinates
        source.Units = "degree, degree, metre" if coordinates == "spherical" else "metre"
        source[:] = polar if coordinates == "spherical" else cartesian
        sampling_rate = dataset.createVariable("Data.SamplingRate", "f8", ("I",))
        sampling_rate.Units = "hertz"
        sampling_rate[:] = [rate]
        impulses = dataset.createVariable("Data.IR", "f8", ("M", "R", "N"), zlib=compressed)
        data = np.zeros((6, 2, 16), dtype="f8")
        for direction in range(6):
            data[direction, 0, 2] = (direction + 1) / 8
            data[direction, 1, 4] = (6 - direction) / 8
        impulses[:] = data
        delay = dataset.createVariable("Data.Delay", "f8", ("I", "R"))
        delay.Units = "second"
        delay[:] = 0
(root / "malformed.sofa").write_bytes(b"not an HDF5 file\n")

# Identical 836-direction / 1024-tap data with two legal chunk partitions.
# The single chunk exceeds the old parser's 0x130000-element limit while its
# decoded size (about 13 MiB) is comfortably below the 256 MiB byte limit.
# Sparse impulses keep both committed files below 60 KiB.
m, n = 836, 1024
for name, chunks in [("large-single-chunk", (m, 2, n)), ("large-split-chunks", (64, 2, n))]:
    with Dataset(root / (name + ".sofa"), "w", format="NETCDF4") as dataset:
        for key, size in dict(I=1, C=3, R=2, E=1, N=n, M=m).items():
            dataset.createDimension(key, size)
        dataset.Conventions = "SOFA"
        dataset.Version = "1.0"
        dataset.SOFAConventions = "SimpleFreeFieldHRIR"
        dataset.SOFAConventionsVersion = "1.0"
        dataset.DataType = "FIR"
        dataset.RoomType = "free field"
        source = dataset.createVariable("SourcePosition", "f8", ("M", "C"))
        source.Type = "spherical"
        source.Units = "degree, degree, metre"
        positions = np.zeros((m, 3))
        positions[:, 0] = np.arange(m) * 360 / m
        positions[:, 2] = 1
        source[:] = positions
        sampling_rate = dataset.createVariable("Data.SamplingRate", "f8", ("I",))
        sampling_rate.Units = "hertz"
        sampling_rate[:] = 48000
        impulses = dataset.createVariable("Data.IR", "f8", ("M", "R", "N"),
                                          zlib=True, shuffle=True, chunksizes=chunks)
        data = np.zeros((m, 2, n))
        data[:, 0, 2] = 0.125
        data[:, 1, 4] = 0.75
        impulses[:] = data
        delay = dataset.createVariable("Data.Delay", "f8", ("I", "R"))
        delay[:] = 0
