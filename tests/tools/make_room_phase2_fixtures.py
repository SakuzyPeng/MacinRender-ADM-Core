#!/usr/bin/env python3
"""Maintenance-only: write the synthetic room pack of the PCM consistency matrix (phase 4.2).

The pack is the non-personal bundle of tests/unit/room_fixture.py: room.json and the delay-aligned
HRIR hrir.json with their assets. Its propagation phase table uses Python's math.sin/cos, i.e. the
platform libm, so the committed bytes are authoritative and pinned by SHA-256 in
scripts/consistency/phase2_common.py (ROOM_FIXTURES); do not regenerate them on CI. Rerun this only
to change the pack deliberately, then update the pins."""
import importlib.util
from pathlib import Path
import shutil
import tempfile

ROOT = Path(__file__).resolve().parents[2]
TARGET = ROOT / 'tests/fixtures/room/phase2'
FILES = ('room.json', 'hrir.json', 'hrir.f32', 'directions.f64', 'faces.u32', 'delays.f64',
         'bands.f64', 'late.f32', 'late-directions.f64', 'eq.f64', 'phases.f32')


def main():
    spec = importlib.util.spec_from_file_location('room_fixture', ROOT / 'tests/unit/room_fixture.py')
    module = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(module)
    with tempfile.TemporaryDirectory(prefix='mradm-room-phase2-') as directory:
        module.fixture(Path(directory))
        TARGET.mkdir(parents=True, exist_ok=True)
        for name in FILES:
            shutil.copyfile(Path(directory) / name, TARGET / name)


if __name__ == '__main__':
    main()
