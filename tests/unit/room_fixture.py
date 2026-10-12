#!/usr/bin/env python3
"""Run native room integration tests with a tiny, non-personal synthetic bundle."""
import hashlib
import json
import math
from pathlib import Path
import struct
import subprocess
import sys
import tempfile


def fixture(root):
    def asset(name, values, shape, dtype):
        code = {"<f4": "f", "<f8": "d", "<u4": "I"}[dtype]
        payload = struct.pack("<" + str(len(values)) + code, *values)
        (root / name).write_bytes(payload)
        return dict(file=name, shape=shape, dtype=dtype, bytes=len(payload),
                    sha256=hashlib.sha256(payload).hexdigest())

    def write(name, value):
        (root / name).write_text(json.dumps(value, sort_keys=True) + "\n")

    directions = [1, 0, 0, 0, 1, 0, -1, 0, 0, 0, -1, 0, 0, 0, 1, 0, 0, -1]
    faces = [v for i in range(4) for f in ([i, (i + 1) % 4, 4], [(i + 1) % 4, i, 5]) for v in f]
    impulses = [0.] * (6 * 2 * 8)
    for d in range(6):
        impulses[d * 16 + 2] = 1. - d * 0.05
        impulses[d * 16 + 12] = 0.75 + d * 0.02
    hrir = dict(schema="mradm.hrir", version=1, kind="delay-aligned-v1", sample_rate=48000,
                fractional_taps=81, guard_samples=96, output_taps=192, assets={
                    "impulses": asset("hrir.f32", impulses, [6, 2, 8], "<f4"),
                    "directions": asset("directions.f64", directions, [6, 3], "<f8"),
                    "faces": asset("faces.u32", faces, [8, 3], "<u4"),
                    "delays": asset("delays.f64", [2., 4.] * 6, [6, 2], "<f8")})
    write("hrir.json", hrir)
    phases = []
    for p in range(1025):
        values = []
        for i in range(81):
            x = i - 40. - p / 1024
            sinc = 1. if abs(x) < 1e-12 else math.sin(math.pi * x) / (math.pi * x)
            values.append(sinc * (0.5 - 0.5 * math.cos(2 * math.pi * i / 80)))
        scale = sum(values)
        phases.extend(v / scale for v in values)
    late = [0.] * (6 * 2301)
    for d in range(6):
        late[d * 2301 + 17 + d * 11] = 0.1
        late[d * 2301 + 2200 + d * 9] = 0.01
    room = dict(schema="mradm.room", version=1, kind="hybrid-shoebox-v1", id="synthetic",
                name="Synthetic test room", sample_rate=48000, dimensions=[7, 6, 3.2], listener=[3.4, 3, 1.2],
                speed_of_sound=344.02, reference_distance=2.1, minimum_distance=0.25, boundary_clearance=0.15,
                reflection_order=4, absorption=[[0.94] * 8 for _ in range(6)],
                source_directivity="cosine2-listener", early_fade_seconds=[0.015, 0.025],
                late_predelay_seconds=0.018, late_energy=0.25, alignment_guard_samples=40, band_filter_offset=0,
                default_hrir="hrir.json", assets={
                    "bands": asset("bands.f64", [0.125] * 8, [8, 1], "<f8"),
                    "late": asset("late.f32", late, [6, 2301], "<f4"),
                    "late_directions": asset("late-directions.f64", directions, [6, 3], "<f8"),
                    "eq": asset("eq.f64", [1., 0., 0., 0.05], [4], "<f8"),
                    "propagation_phases": asset("phases.f32", phases, [1025, 81], "<f4")})
    write("room.json", room)
    room["id"] = "second"
    room["dimensions"] = [8, 7, 3.6]
    write("second.json", room)
    room["id"] = "incompatible-guard"
    room["alignment_guard_samples"] = 41
    write("incompatible.json", room)
    room["alignment_guard_samples"] = 40
    room["id"] = "bad"
    room["assets"]["eq"]["sha256"] = "0" * 64
    write("bad.json", room)


if __name__ == "__main__":
    with tempfile.TemporaryDirectory(prefix="mradm-room-test-") as directory:
        root = Path(directory)
        fixture(root)
        subprocess.run([sys.argv[1], str(root), str(Path(__file__).resolve().parents[1] / "fixtures/sofa/simple.sofa")], check=True)
