#!/usr/bin/env python3
"""Compare room-only Release checkpoints produced from identical resources/events.

Inputs are LABEL=manifest.json, with labels macos-arm64/windows-x64.
An incomplete platform set never produces a complete_pass result.
"""
import argparse
import json
from pathlib import Path


def differences(a, b, path=""):
    if type(a) is not type(b):
        return [path]
    if isinstance(a, dict):
        result = []
        for key in sorted(set(a) | set(b)):
            target = path + "/" + key
            result.extend([target] if key not in a or key not in b else differences(a[key], b[key], target))
        return result
    if isinstance(a, list):
        if len(a) != len(b):
            return [path + "/length"]
        return [item for i, (left, right) in enumerate(zip(a, b)) for item in differences(left, right, path + "/" + str(i))]
    return [] if a == b else [path]


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("manifests", nargs="+")
    parser.add_argument("--output", type=Path, required=True)
    parser.add_argument("--allow-partial", action="store_true", help="allow exit 0 for identical supplied platforms; coverage stays incomplete")
    args = parser.parse_args()
    inputs = {}
    for argument in args.manifests:
        label, path = argument.split("=", 1)
        if label in inputs:
            raise ValueError("duplicate platform label")
        value = json.loads(Path(path).read_text())
        if value.get("schema") != "mradm.room-replay" or value.get("version") != 1:
            raise ValueError("unsupported room replay manifest")
        inputs[label] = value
    reference = next(iter(inputs.values()))
    comparison = {label: differences(reference, value) for label, value in inputs.items()}
    missing = sorted({"macos-arm64", "windows-x64"} - set(inputs))
    identical = all(not value for value in comparison.values())
    report = dict(schema="mradm.room-comparison", version=1, platforms=list(inputs), missing_platforms=missing,
                  identical_for_compared=identical, complete_pass=identical and not missing,
                  differing_checkpoints=comparison, frames=reference["frames"],
                  pcm_sha256=reference["files"]["session.f32"]["sha256"],
                  scope="Prepared coefficients, HRIR/branch output, PCM and frame counts; room module only")
    args.output.write_text(json.dumps(report, indent=2) + "\n")
    print(json.dumps(report))
    if not identical or (missing and not args.allow_partial):
        raise SystemExit(1)


if __name__ == "__main__":
    main()
