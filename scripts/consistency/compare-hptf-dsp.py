#!/usr/bin/env python3
"""Compare numeric captures from pre/post migration Release HpTF DSP tests."""
import argparse
import hashlib
import json
import math
from pathlib import Path

LIMITS = {"coefficients": 2e-6, "response_db": 0.002, "pcm": 2e-5}


def read(path):
    records = {}
    for line in path.read_text().splitlines():
        kind, name, count, *samples = line.split()
        values = list(map(float, samples))
        key = kind + "/" + name
        if kind not in LIMITS or key in records or int(count) != len(values) or not values:
            raise ValueError("Invalid capture record: " + key)
        if not all(math.isfinite(x) for x in values):
            raise ValueError("Nonfinite capture record: " + key)
        records[key] = values
    if not records:
        raise ValueError("Empty capture")
    return records


def compare(reference, candidate):
    before, after = read(reference), read(candidate)
    if before.keys() != after.keys():
        raise ValueError("Capture record sets differ")
    rows = []
    for key, a in before.items():
        b = after[key]
        if len(a) != len(b):
            raise ValueError("Capture sample counts differ: " + key)
        error = [x - y for x, y in zip(a, b)]
        maximum = max(map(abs, error))
        limit = LIMITS[key.split("/")[0]]
        rows.append({"case": key, "count": len(a), "max_absolute_error": maximum,
                     "rms_error": math.sqrt(math.fsum(x*x for x in error) / len(error)),
                     "equal_values": a == b, "tolerance": limit, "passed": maximum <= limit})
    return {"schema": "mradm.hptf-numeric-comparison.v1", "reference_sha256": hashlib.sha256(reference.read_bytes()).hexdigest(),
            "candidate_sha256": hashlib.sha256(candidate.read_bytes()).hexdigest(),
            "tolerances": LIMITS, "records": rows, "passed": all(row["passed"] for row in rows)}


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--reference", type=Path, required=True)
    parser.add_argument("--candidate", type=Path, required=True)
    parser.add_argument("--output", type=Path, required=True)
    args = parser.parse_args()
    report = compare(args.reference, args.candidate)
    args.output.parent.mkdir(parents=True, exist_ok=True)
    args.output.write_text(json.dumps(report, indent=2) + "\n")
    for kind in LIMITS:
        rows = [r for r in report["records"] if r["case"].startswith(kind + "/")]
        if rows:
            print(f"{kind}: {len(rows)} records, maximum error {max(r['max_absolute_error'] for r in rows):.9g}")
    return 0 if report["passed"] else 1


if __name__ == "__main__":
    raise SystemExit(main())
