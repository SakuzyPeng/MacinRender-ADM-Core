#!/usr/bin/env python3
"""Test geometry/spread interventions after verifying an arithmetic model against captured SAF gains.

The dot hypothesis is f32 products accumulated in f64, then narrowed to f32. It must
match every captured dot on both inputs before any intervention result is reported.
This describes the measured three-element Linux/Windows dots, not arbitrary BLAS calls.
"""
import argparse
import importlib.util
import json
import math
from pathlib import Path
import struct

spec = importlib.util.spec_from_file_location("spread", Path(__file__).with_name("replay-vbap-spread.py"))
spread = importlib.util.module_from_spec(spec)
spec.loader.exec_module(spread)
f32, read, words = spread.f32, spread.read, spread.words


def calculate(faces, inverse, directions, speakers):
    if len(inverse) != len(faces) * 3 or len(directions) != 27 or any(i < 0 or i >= speakers for i in faces):
        raise ValueError("invalid MDAP replay dimensions")
    gains, dots = [0.0] * speakers, []
    for source in range(9):
        u = directions[source * 3:source * 3 + 3]
        for face in range(len(faces) // 3):
            dot = [f32(sum(f32(inverse[face * 9 + row * 3 + j] * u[j]) for j in range(3))) for row in range(3)]
            dots.extend(dot)
            squared = 0.0
            for value in dot:
                squared = f32(squared + f32(value * value))
            norm = f32(math.sqrt(squared))
            if min(dot) > -0.001:
                for j in range(3):
                    index = faces[face * 3 + j]
                    gains[index] = f32(gains[index] + f32(dot[j] / norm))
    before = gains[:]
    squared = 0.0
    for value in gains:
        squared = f32(squared + f32(value * value))
    norm = f32(math.sqrt(squared))
    gains = [max(f32(value / norm), 0.0) for value in gains]
    return dots, before, (norm,), gains


def capture(root):
    face_bytes = (root / "geometry.02-faces.i32").read_bytes()
    if not face_bytes or len(face_bytes) % 12:
        raise ValueError("invalid triangle checkpoint")
    faces = struct.unpack("<" + "i" * (len(face_bytes) // 4), face_bytes)
    inverse = read(root / "geometry.03-inverse.f32")
    directions = read(root / "spread.08-directions.f32")
    vertices = read(root / "geometry.01-vertices.f32")
    if len(vertices) != 33:
        raise ValueError("expected the 5.1.4 probe with two dummy speakers")
    result = calculate(faces, inverse, directions, 11)
    for value, suffix in zip(result, ("dots", "before-normalize", "norm", "gains")):
        if words(value) != (root / ("native-" + suffix + ".f32")).read_bytes():
            raise ValueError(f"arithmetic hypothesis does not reproduce native {suffix}: {root}")
    if words(result[3][:9]) != (root / "output.01-full-gains.f32").read_bytes():
        raise ValueError("replay differs from the complete SAF wrapper")
    return faces, inverse, directions, result[3]


def read_pcm(path, channels):
    data = path.read_bytes()
    if len(data) < 24 or data[:4] != b"MRPB":
        raise ValueError("invalid canonical PCM image")
    version, count, rate, frames = struct.unpack_from("<IIIQ", data, 4)
    if (version, count, rate, frames) != (1, channels, 48000, 48000) or len(data) != 24 + frames * count * 4:
        raise ValueError("PCM replay requires the one-second 48 kHz fixture and 5.1.4 output")
    return data[24:]


def pcm_replay(args, first_gains, second_gains, tangent_gains, geometry_gains):
    for root in (args.first, args.second):
        if read(root / "input.01-source.f32")[2] != 1.0:
            raise ValueError("PCM replay requires unity source gain")
    source = read_pcm(args.input_pcm, 1)
    samples = struct.unpack("<48000f", source)
    first, second = read_pcm(args.first_pcm, 10), read_pcm(args.second_pcm, 10)

    def render(gains):
        # The fixture's layout inserts the LFE at channel 3; Objects leave it zero.
        expanded = list(gains[:3]) + [0.0] + list(gains[3:9])
        return words([0.0 if gain == 0.0 else f32(sample * gain) for sample in samples for gain in expanded])

    if render(first_gains) != first or render(second_gains) != second:
        raise ValueError("constant-gain PCM replay does not match both native renders")

    def differences(a, b):
        return sum(a[i:i + 4] != b[i:i + 4] for i in range(0, len(a), 4))

    return {
        "native_pcm_replays_verified": True,
        "samples": 480000,
        "original_different_samples": differences(first, second),
        "tangent_swap_different_samples_vs_second": differences(render(tangent_gains), second),
        "geometry_swap_different_samples_vs_second": differences(render(geometry_gains), second),
    }


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("first", type=Path)
    parser.add_argument("second", type=Path)
    parser.add_argument("--input-pcm", type=Path, help="canonical mono input image extracted by mr_adm_pcm_bits")
    parser.add_argument("--first-pcm", type=Path)
    parser.add_argument("--second-pcm", type=Path)
    args = parser.parse_args()
    pcm_options = (args.input_pcm, args.first_pcm, args.second_pcm)
    if any(pcm_options) and not all(pcm_options):
        parser.error("provide all three PCM paths together")
    fa, ia, da, ga = capture(args.first)
    fb, ib, db, gb = capture(args.second)
    axis_a, ring_a, trig_a, _ = spread.capture(args.first)
    axis_b, _, trig_b, _ = spread.capture(args.second)
    if words(axis_a) != words(axis_b) or words(trig_a[:10]) != words(trig_b[:10]):
        raise ValueError("inputs differ before the tangent intervention")
    tangent_directions = spread.replay(axis_a, ring_a, trig_b[10])[2]
    tangent_gains = calculate(fa, ia, tangent_directions, 11)[3]
    geometry_gains = calculate(fb, ib, da, 11)[3]

    def output_compare(candidate):
        return spread.compare(candidate[:9], gb[:9])

    result = {
        "native_dots_and_gains_verified": True,
        "dot_model": "round_f32(sum_f64(round_f32(a_i*b_i)))",
        "face_indices_identical": fa == fb,
        "original_first_vs_second": output_compare(ga),
        "first_with_second_tangent_vs_second": output_compare(tangent_gains),
        "first_with_second_spread_vs_second": output_compare(calculate(fa, ia, db, 11)[3]),
        "first_with_second_geometry_vs_second": output_compare(geometry_gains),
        "first_with_both_factors_vs_second": output_compare(calculate(fb, ib, db, 11)[3]),
    }
    if all(pcm_options):
        result["pcm_replay"] = pcm_replay(args, ga, gb, tangent_gains, geometry_gains)
    print(json.dumps(result, indent=2))


if __name__ == "__main__":
    main()
