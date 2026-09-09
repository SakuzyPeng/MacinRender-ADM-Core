#!/usr/bin/env python3
"""Isolate tanf and SGEMM outputs using captured, bit-verified 3D VBAP spread stages."""
import argparse
import json
import math
from pathlib import Path
import struct


def f32(value):
    return struct.unpack("<f", struct.pack("<f", value))[0]


def words(values):
    return struct.pack("<" + "f" * len(values), *values)


def read(path):
    data = path.read_bytes()
    if not data or len(data) % 4:
        raise ValueError(f"invalid float checkpoint: {path}")
    return struct.unpack("<" + "f" * (len(data) // 4), data)


def replay(axis, ring, tangent):
    if len(axis) != 3 or len(ring) != 24:
        raise ValueError("expected the eight-source, one-ring MDAP case")
    raw = [f32(axis[i % 3] + f32(ring[i] * tangent)) for i in range(24)]
    squares = [f32(x * x) for x in raw[:3]]
    norm = f32(math.sqrt(f32(f32(squares[0] + squares[1]) + squares[2])))
    directions = [f32(x / norm) for x in raw] + list(axis)
    return raw + [0.0] * 3, (norm,), directions


def capture(root):
    axis = read(root / "spread.02-axis.f32")
    ring = read(root / "spread.05-rotated-ring.f32")
    trig = read(root / "spread.01-trig.f32")
    if len(trig) != 11:
        raise ValueError("unexpected trig checkpoint size")
    calculated = replay(axis, ring, trig[10])
    for actual, name in zip(calculated, ("spread.06-before-normalize.f32", "spread.07-norm.f32",
                                        "spread.08-directions.f32")):
        if words(actual) != (root / name).read_bytes():
            raise ValueError(f"native scalar replay is not bit-identical: {root / name}")
    return axis, ring, trig, calculated[2]


def compare(a, b):
    aw, bw = words(a), words(b)
    changed = [i for i in range(len(a)) if aw[i * 4:i * 4 + 4] != bw[i * 4:i * 4 + 4]]
    return {"identical": not changed, "different_words": len(changed), "words": len(a)}


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("first", type=Path)
    parser.add_argument("second", type=Path)
    args = parser.parse_args()
    axis_a, ring_a, trig_a, dirs_a = capture(args.first)
    axis_b, ring_b, trig_b, dirs_b = capture(args.second)
    if words(axis_a) != words(axis_b) or words(trig_a[:10]) != words(trig_b[:10]):
        raise ValueError("source/trigonometric inputs differ before the factors being isolated")
    for name in ("spread.03-rotation.f32", "spread.04-base.f32"):
        if (args.first / name).read_bytes() != (args.second / name).read_bytes():
            raise ValueError(f"SGEMM inputs differ: {name}")
    print(json.dumps({
        "native_scalar_replays_verified": True,
        "tangent_bits": [f"0x{struct.unpack('<I', words((v,)))[0]:08x}" for v in (trig_a[10], trig_b[10])],
        "rotated_ring": compare(ring_a, ring_b),
        "directions": compare(dirs_a, dirs_b),
        "first_with_second_tangent_vs_second": compare(replay(axis_a, ring_a, trig_b[10])[2], dirs_b),
        "first_with_second_ring_vs_second": compare(replay(axis_a, ring_b, trig_a[10])[2], dirs_b),
        "first_with_both_factors_vs_second": compare(replay(axis_a, ring_b, trig_b[10])[2], dirs_b),
    }, indent=2))


if __name__ == "__main__":
    main()
