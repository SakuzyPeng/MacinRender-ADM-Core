#!/usr/bin/env python3
"""Regenerate the project-owned off-axis SOFA fixture (numpy/netCDF4 required).

CI reads the committed bytes; it never regenerates or downloads SOFA inputs.
After intentional regeneration, update the pinned SHA-256 in phase2_common.py.
"""
from itertools import product
from pathlib import Path

import numpy as np
from netCDF4 import Dataset


def main():
    path = Path(__file__).resolve().parents[1] / 'fixtures/sofa/off-axis-compressed.sofa'
    positions = [(x + y / 8, y * 3 / 4 + z / 16, z * 7 / 8 + x / 32)
                 for x, y, z in product((-1, 0, 1), repeat=3) if (x, y, z) != (0, 0, 0)]
    impulses = np.empty((26, 2, 64), dtype='f8')
    state = 0x12345678
    for index in range(impulses.size):
        state = (state * 1664525 + 1013904223) & 0xffffffff
        impulses.flat[index] = ((state >> 8) - 8388608) / (2 ** (27 + index % 64 // 8))
    with Dataset(path, 'w', format='NETCDF4') as dataset:
        for key, size in dict(I=1, C=3, R=2, E=1, N=64, M=26).items():
            dataset.createDimension(key, size)
        dataset.Conventions = 'SOFA'
        dataset.Version = '1.0'
        dataset.SOFAConventions = 'GeneralFIR'
        dataset.SOFAConventionsVersion = '1.0'
        dataset.DataType = 'FIR'
        dataset.RoomType = 'free field'
        dataset.ListenerShortName = 'MacinRender off-axis synthetic'
        source = dataset.createVariable('SourcePosition', 'f8', ('M', 'C'))
        source.Type = 'cartesian'
        source.Units = 'metre'
        source[:] = positions
        rate = dataset.createVariable('Data.SamplingRate', 'f8', ('I',))
        rate.Units = 'hertz'
        rate[:] = 48000
        ir = dataset.createVariable('Data.IR', 'f8', ('M', 'R', 'N'),
                                    zlib=True, shuffle=True, chunksizes=(7, 2, 64))
        ir[:] = impulses
        delay = dataset.createVariable('Data.Delay', 'f8', ('I', 'R'))
        delay.Units = 'second'
        delay[:] = 0


if __name__ == '__main__':
    main()
