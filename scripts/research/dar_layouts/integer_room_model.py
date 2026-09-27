"""Measured Renderer 5.5 Cartesian point-gain geometry, for research checks."""

import math
import struct

XY_STEPS = 155
Z_STEPS = 75


def as_float32(value: float) -> float:
    return struct.unpack("<f", struct.pack("<f", value))[0]


def pair_weights(position: float) -> tuple[float, float]:
    fraction = max(0.0, min(1.0, position))
    return math.cos(math.pi * fraction / 2), math.sin(math.pi * fraction / 2)


def row_gain(x: float, anchors: list[tuple[int, int]], channels: int) -> list[float]:
    result = [0.0] * channels
    if x <= anchors[0][0]:
        result[anchors[0][1]] = 1.0
        return result
    if x >= anchors[-1][0]:
        result[anchors[-1][1]] = 1.0
        return result
    for (left_x, left_ch), (right_x, right_ch) in zip(anchors, anchors[1:]):
        if x <= right_x:
            result[left_ch], result[right_ch] = pair_weights((x - left_x) / (right_x - left_x))
            break
    return result


def layer_gain(x: float, y: float, rows: list[tuple[int, list]], channels: int) -> list[float]:
    if y <= rows[0][0]:
        return row_gain(x, rows[0][1], channels)
    if y >= rows[-1][0]:
        return row_gain(x, rows[-1][1], channels)
    for (lower_y, lower_row), (upper_y, upper_row) in zip(rows, rows[1:]):
        if y <= upper_y:
            low_weight, high_weight = pair_weights((y - lower_y) / (upper_y - lower_y))
            low = row_gain(x, lower_row, channels)
            high = row_gain(x, upper_row, channels)
            return [a * low_weight + b * high_weight for a, b in zip(low, high)]
    raise AssertionError("no row interval")


def point_gain(layout: str, xyz: tuple[float, float, float], quantize: bool = True) -> list[float]:
    x, y, z = xyz
    if quantize:
        x = math.floor(as_float32(x) * XY_STEPS + 0.5)
        y = -math.floor(-as_float32(y) * XY_STEPS + 0.5)
        z = math.floor(as_float32(z) * Z_STEPS + 0.5)
    else:
        x *= XY_STEPS
        y *= XY_STEPS
        z *= Z_STEPS
    channels = 12 if layout == "7.1.4" else 16
    middle = [(-155, [(-155, 6), (155, 7)]), (0, [(-155, 4), (155, 5)])]
    if layout == "9.1.6":
        middle.append((105, [(-155, 8), (155, 9)]))
    middle.append((155, [(-155, 0), (0, 2), (155, 1)]))
    if layout == "7.1.4":
        upper = [(-80, [(-80, 10), (80, 11)]), (80, [(-80, 8), (80, 9)])]
    elif layout == "9.1.6":
        upper = [(-80, [(-80, 14), (80, 15)]),
                 (0, [(-80, 12), (80, 13)]),
                 (80, [(-80, 10), (80, 11)])]
    else:
        raise ValueError(layout)
    a, b = pair_weights(z / Z_STEPS)
    low = layer_gain(x, y, middle, channels)
    high = layer_gain(x, y, upper, channels)
    return [a * v + b * w for v, w in zip(low, high)]
