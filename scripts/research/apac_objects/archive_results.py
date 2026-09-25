#!/usr/bin/env python3
"""Archive research sources, verified results and short self-generated fixtures."""

import argparse
import hashlib
import json
from pathlib import Path
import struct
from zipfile import ZipFile, ZIP_DEFLATED


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--results", type=Path, required=True)
    parser.add_argument("--output", type=Path, required=True)
    parser.add_argument("--extra-results", type=Path, help="optional verified boundary run")
    parser.add_argument("--mixed-results", type=Path, help="optional verified object/LFE mixed run")
    parser.add_argument("--capacity-results", type=Path, help="optional verified mixed BED/object capacity run")
    args = parser.parse_args()
    root = args.results.resolve()
    verification = json.loads((root / "verification.json").read_text())
    if not verification["all_checks_passed"]:
        raise ValueError("results did not pass verification")
    repository = Path(__file__).resolve().parents[3]
    files = {}
    for path in Path(__file__).resolve().parent.iterdir():
        if path.suffix in (".py", ".cpp", ".m", ".md"):
            files[str(path.relative_to(repository))] = path.read_bytes()
    for report in (repository / "docs/architecture").glob("APAC_OBJECT*.md"):
        files[str(report.relative_to(repository))] = report.read_bytes()
    for path in root.glob("*.json"):
        files["results/" + path.name] = path.read_bytes()
    for path in (root / "fixtures").glob("*"):
        if path.suffix in (".caf", ".mp4"):
            files["fixtures/" + path.name] = path.read_bytes()
    for path in (root / "cases").glob("*/*"):
        if path.is_file() and (path.name in ("result.json", "stdout.txt", "stderr.txt") or path.name.startswith("stream.")):
            files["results/" + str(path.relative_to(root))] = path.read_bytes()
    for pattern in ("derived/*/positions.json", "derived/*/positions.csv", "inputs/*/expected.json", "inputs/*/settings.plist"):
        for path in root.glob(pattern):
            files["results/" + str(path.relative_to(root))] = path.read_bytes()
    for name in ("left1", "right1", "moving1"):
        pcm = (root / "derived" / name / "binaural.f32").read_bytes()
        if len(pcm) % 8:
            raise ValueError("invalid stereo float32 PCM")
        fmt = struct.pack("<HHIIHH", 3, 2, 48000, 48000 * 8, 8, 32)
        body = b"WAVEfmt " + struct.pack("<I", len(fmt)) + fmt
        body += b"fact" + struct.pack("<II", 4, len(pcm) // 8)
        body += b"data" + struct.pack("<I", len(pcm)) + pcm
        wav = b"RIFF" + struct.pack("<I", len(body)) + body
        files["fixtures/" + name + "-decoded-binaural.wav"] = wav
    if args.extra_results:
        extra = args.extra_results.resolve()
        checked = json.loads((extra / "verification.json").read_text())
        if not checked["all_checks_passed"]:
            raise ValueError("boundary observations did not pass verification")
        rows = json.loads((extra / "boundaries.json").read_text())
        selected_cases = {row["case"] for row in rows} | {row["case"] + "-decode" for row in rows}
        selected_cases.update({"default-decoder-capacity-70", "default-decoder-capacity-71",
                               "objects-253-channel-map-edge", "asset70-pcm-check"})
        for pattern in ("default-objects-*-asset", "default-objects-*-mp4-wrap"):
            selected_cases.update(path.name for path in (extra / "cases").glob(pattern))
        for path in extra.glob("*.json"):
            files["boundary_results/" + path.name] = path.read_bytes()
        for case in selected_cases:
            for path in (extra / "cases" / case).glob("*"):
                if path.is_file() and (path.name in ("result.json", "stdout.txt", "stderr.txt") or path.name.startswith("stream.")):
                    files["boundary_results/" + str(path.relative_to(extra))] = path.read_bytes()
        for pattern in ("derived/*/positions.json", "derived/*/positions.csv", "inputs/*/expected.json", "inputs/*/settings.plist"):
            for path in extra.glob(pattern):
                files["boundary_results/" + str(path.relative_to(extra))] = path.read_bytes()
        for path in (extra / "fixtures").glob("*"):
            if path.suffix in (".caf", ".mp4"):
                files["boundary_fixtures/" + path.name] = path.read_bytes()
    if args.mixed_results:
        mixed = args.mixed_results.resolve()
        checked = json.loads((mixed / "verification.json").read_text())
        if not checked["all_checks_passed"]:
            raise ValueError("mixed-path verification failed")
        names = [row["case"] for row in json.loads((mixed / "mixed.json").read_text())]
        selected = {name + suffix for name in names for suffix in ("-encode", "-decode", "-mp4-wrap", "-caf-import", "-mp4-import")}
        selected.add("lfe-only-total-rate-control")
        for path in mixed.glob("*.json"):
            files["mixed_results/" + path.name] = path.read_bytes()
        for name in selected:
            for path in (mixed / "cases" / name).glob("*"):
                if path.is_file() and (path.name in ("result.json", "stdout.txt", "stderr.txt") or path.name.startswith("stream.")):
                    files["mixed_results/" + str(path.relative_to(mixed))] = path.read_bytes()
        for name in names:
            for filename in ("expected.json", "settings.plist"):
                path = mixed / "inputs" / name / filename
                files["mixed_results/" + str(path.relative_to(mixed))] = path.read_bytes()
        for path in (mixed / "fixtures").glob("*"):
            if path.suffix in (".caf", ".mp4"):
                files["mixed_fixtures/" + path.name] = path.read_bytes()
    if args.capacity_results:
        capacity = args.capacity_results.resolve()
        checked = json.loads((capacity / "verification.json").read_text())
        if not checked["all_checks_passed"]:
            raise ValueError("mixed capacity verification failed")
        for path in capacity.glob("*.json"):
            files["capacity_results/" + path.name] = path.read_bytes()
        for path in (capacity / "cases").glob("*/*"):
            if path.is_file() and (path.name in ("result.json", "stdout.txt", "stderr.txt") or path.name.startswith("stream.")):
                files["capacity_results/" + str(path.relative_to(capacity))] = path.read_bytes()
        for pattern in ("inputs/*/expected.json", "inputs/*/settings.plist"):
            for path in capacity.glob(pattern):
                files["capacity_results/" + str(path.relative_to(capacity))] = path.read_bytes()
        for path in (capacity / "fixtures").glob("*"):
            if path.suffix in (".caf", ".mp4"):
                files["capacity_fixtures/" + path.name] = path.read_bytes()
    manifest = {
        "schema_version": 1,
        "scope": "private object entry; default and diagnostic profile ceilings distinguished in results",
        "excluded": ["system binaries", "IDA databases", "decompiled system code", "build products", "system logs"],
        "files": [{"path": name, "bytes": len(data), "sha256": hashlib.sha256(data).hexdigest()}
                  for name, data in sorted(files.items())],
    }
    files["MANIFEST.json"] = (json.dumps(manifest, indent=2) + "\n").encode()
    with ZipFile(args.output, "x", compression=ZIP_DEFLATED) as archive:
        for name, data in sorted(files.items()):
            archive.writestr(name, data)
    with ZipFile(args.output) as archive:
        if archive.testzip() is not None:
            raise ValueError("ZIP CRC verification failed")
        for entry in manifest["files"]:
            data = archive.read(entry["path"])
            if len(data) != entry["bytes"] or hashlib.sha256(data).hexdigest() != entry["sha256"]:
                raise ValueError("archive manifest verification failed")
    print(json.dumps({"archive": str(args.output.resolve()), "files": len(files),
                      "bytes": args.output.stat().st_size, "sha256": hashlib.sha256(args.output.read_bytes()).hexdigest()}))


if __name__ == "__main__":
    main()
