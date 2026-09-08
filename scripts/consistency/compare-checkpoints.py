#!/usr/bin/env python3
"""Compare canonical checkpoint words; output the first different value at every boundary."""
import argparse
import json
from pathlib import Path
import struct

FORMATS = {".f32": (4, "f"), ".c32": (4, "f"), ".f64": (8, "d"), ".i32": (4, "i")}


def compare(first, second):
    paths = [set(p.relative_to(root) for p in root.rglob("*") if p.suffix in FORMATS) for root in (first, second)]
    if not paths[0] or not paths[1]:
        raise ValueError("both inputs must contain checkpoints")
    results = []
    for path in sorted(paths[0] | paths[1]):
        row = {"checkpoint": str(path)}
        if path not in paths[0] or path not in paths[1]:
            row["status"] = "missing"
        else:
            a, b = (root.joinpath(path).read_bytes() for root in (first, second))
            width, code = FORMATS[path.suffix]
            if len(a) % width or len(b) % width:
                raise ValueError(f"truncated checkpoint: {path}")
            row["status"] = "identical" if a == b else "different"
            row["words"] = [len(a) // width, len(b) // width]
            if a != b:
                row["different_words"] = sum(a[i:i + width] != b[i:i + width]
                                              for i in range(0, max(len(a), len(b)), width))
                for i in range(0, min(len(a), len(b)), width):
                    if a[i:i + width] != b[i:i + width]:
                        row["first_word"] = i // width
                        row["bits"] = [f"0x{int.from_bytes(data[i:i + width], 'little'):0{width * 2}x}" for data in (a, b)]
                        row["values"] = [struct.unpack_from("<" + code, data, i)[0] for data in (a, b)]
                        break
        results.append(row)
    return results


if __name__ == "__main__":
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("first", type=Path)
    parser.add_argument("second", type=Path)
    args = parser.parse_args()
    print(json.dumps(compare(args.first, args.second), indent=2))
