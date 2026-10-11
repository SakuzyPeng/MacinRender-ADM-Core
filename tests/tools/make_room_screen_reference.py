#!/usr/bin/env python3
"""Regenerate room screen vectors with unmodified EAR Python 2.0.0 components.

Uses the shared FetchContent cache and the caller's existing numpy/attrs/scipy
environment. Only the screen component is loaded, avoiding renderer startup.
"""
import hashlib
import io
import json
from pathlib import Path
import subprocess
import sys
import tarfile
import tempfile
import types

import numpy as np

ROOT = Path(__file__).resolve().parents[2]
REVISION = "10149bb07eeb8a92c34a5e10d2f0597e3bcdfbde"
SOURCE = ROOT / ".fc-cache/ear-python-2.0.0-src"
if not SOURCE.exists():
    payload = subprocess.check_output(["curl", "-fLsS", "--max-time", "60",
        "https://codeload.github.com/ebu/ebu_adm_renderer/tar.gz/" + REVISION])
    with tempfile.TemporaryDirectory(prefix="ear-reference-", dir=ROOT / ".fc-cache") as temporary:
        directory = Path(temporary) / "source"
        with tarfile.open(fileobj=io.BytesIO(payload), mode="r:gz") as archive:
            for member in archive.getmembers():
                parts = Path(member.name).parts
                if len(parts) > 2 and parts[1] == "ear" and member.isfile() and ".." not in parts:
                    destination = directory.joinpath(*parts[1:])
                    destination.parent.mkdir(parents=True, exist_ok=True)
                    destination.write_bytes(archive.extractfile(member).read())
        directory.rename(SOURCE)

sys.path.insert(0, str(SOURCE))
# core.__init__ eagerly imports every renderer. Loading this package namespace
# directly does not change screen_scale, screen_common, geom or common.py.
for name in ("ear.core", "ear.fileio"):
    package = types.ModuleType(name)
    package.__path__ = [str(SOURCE.joinpath(*name.split(".")))]
    sys.modules[name] = package
from ear.core.screen_scale import PolarScreenScaler
from ear.common import PolarScreen, CartesianScreen, PolarPosition, CartesianPosition


def geometry(cartesian=False, centre=(0, 0, 1), width=58, aspect_ratio=1.78):
    return dict(cartesian=cartesian, centre=list(centre), width=width, aspect_ratio=aspect_ratio)


def reference(value):
    centre = (CartesianPosition if value["cartesian"] else PolarPosition)(*map(float, value["centre"]))
    return (CartesianScreen if value["cartesian"] else PolarScreen)(float(value["aspect_ratio"]), centre, float(value["width"]))


cases = []
positions = [[0, 1, 0], [-.8, 1.3, .2], [1, -1, .5], [0, 0, 1], [0, 0, -1],
             [1e-6, -1, 0], [-1e-6, -1, 0], [0, 3, .4], [-.25, .2, -.1]]
for first, second in [
    (geometry(), geometry(width=90)),
    (geometry(centre=(10, 5, 2), width=70, aspect_ratio=1.6), geometry(centre=(-5, -3, 1.5), width=54, aspect_ratio=2)),
    (geometry(True, (0, 1, 0), 1), geometry(True, (.1, 1.4, .1), 1.7, 1.6)),
    (geometry(), geometry()),
]:
    scaler = PolarScreenScaler(reference(first), reference(second))
    for position in positions:
        point = np.asarray(position, dtype=np.float32).astype(float)
        expected = scaler.scale_position(point)
        cases.append(dict(reference=first, reproduction=second, position=point.tolist(), expected=expected.tolist()))
files = ["ear/core/screen_scale.py", "ear/core/screen_common.py", "ear/core/geom.py", "ear/common.py"]
report = dict(schema="mradm.screen-reference", version=1, reference="EBU EAR Python 2.0.0", numpy=np.__version__,
              revision=REVISION, source_sha256={name: hashlib.sha256((SOURCE / name).read_bytes()).hexdigest() for name in files},
              scope="PolarScreenScaler physical-direction mapping, without loudspeaker-layout compensation",
              absolute_tolerance=3e-6, cases=cases)
path = ROOT / "tests/fixtures/room_screen_ear_2_0_0.json"
path.write_text(json.dumps(report, indent=2, allow_nan=False) + "\n")
print(f"Wrote {len(cases)} reference vectors to {path.relative_to(ROOT)}")
