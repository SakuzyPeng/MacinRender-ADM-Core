#!/usr/bin/env python3
"""Losslessly compress traces and remove PCM proven identical to retained controls."""

import argparse
import gzip
import hashlib
import json
from pathlib import Path

from measure_point_suite import file_sha256
from run_gain_probe import decoded_pcm_hash

LOCAL = Path(__file__).resolve().parents[3] / "local"


def compact(root: Path):
    root = root.resolve()
    if LOCAL.resolve() not in root.parents:
        raise ValueError("compaction is restricted to generated research directories under local/")
    manifest = root / "storage-manifest.json"
    report = json.loads(manifest.read_text()) if manifest.exists() else {"items": [], "saved_bytes": 0}

    def save():
        manifest.write_text(json.dumps(report, indent=2) + "\n")

    for summary_path in sorted(root.rglob("summary.json")):
        summary = json.loads(summary_path.read_text())
        comparison = summary.get("pcm_comparison", {})
        if (not comparison.get("all_samples_exact") or not summary.get("state_restored") or
                not summary.get("hook_restored")):
            continue
        run = json.loads(Path(summary["export_run"]).read_text())
        baseline = json.loads(Path(comparison["baseline_run"]).read_text())
        if not baseline["success"] or baseline["adm_sha256"] != run["adm_sha256"]:
            raise ValueError("invalid control identity in compactable capture")
        for item in run["outputs"]:
            path = Path(item["path"]).resolve()
            if not path.exists():
                continue
            expected = next(value for value in baseline["outputs"] if value["layout"] == item["layout"])
            control = Path(expected["path"]).resolve()
            if root not in path.parents or path == control or path.suffix != ".wav":
                raise ValueError("not an isolated generated trace WAV")
            if file_sha256(path) != item["sha256"] or file_sha256(control) != expected["sha256"]:
                raise ValueError("capture or retained control file changed")
            if decoded_pcm_hash(path) != decoded_pcm_hash(control):
                raise ValueError("capture differs from retained control")
            size = path.stat().st_size
            entry = {"path": str(path), "sha256": item["sha256"], "bytes": size,
                     "retained_pcm": str(control), "action": "verified_duplicate_pcm", "removed": False}
            report["items"].append(entry)
            save()
            path.unlink()
            entry["removed"] = True
            report["saved_bytes"] += size
            save()
    for path in sorted(root.rglob("trace.json")):
        data = path.read_bytes()
        output = path.with_suffix(".json.gz")
        packed = gzip.compress(data, compresslevel=6, mtime=0)
        digest = hashlib.sha256(data).hexdigest()
        if output.exists():
            if hashlib.sha256(gzip.decompress(output.read_bytes())).hexdigest() != digest:
                raise ValueError("existing compressed trace differs")
        else:
            output.write_bytes(packed)
        if hashlib.sha256(gzip.decompress(output.read_bytes())).hexdigest() != digest:
            raise ValueError("compressed trace failed readback verification")
        entry = {"path": str(path), "sha256": digest, "bytes": len(data),
                 "retained_trace": str(output), "action": "lossless_gzip", "removed": False}
        report["items"].append(entry)
        save()
        path.unlink()
        entry["removed"] = True
        report["saved_bytes"] += len(data) - output.stat().st_size
        save()
    return report


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--root", type=Path, required=True)
    args = parser.parse_args()
    result = compact(args.root)
    print(json.dumps({"saved_bytes": result["saved_bytes"], "manifest": str(args.root / "storage-manifest.json")}))


if __name__ == "__main__":
    main()
