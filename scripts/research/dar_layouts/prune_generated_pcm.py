#!/usr/bin/env python3
"""Remove only selected reproducible research PCM after hashing it for the record."""

import argparse
import json
from pathlib import Path

from measure_static_bank import file_sha256

ROOT = Path(__file__).resolve().parents[3]
LOCAL = (ROOT / "local").resolve()


def checked_path(path: Path, name: str) -> Path:
    resolved = path.resolve()
    if LOCAL not in resolved.parents or resolved.name != name:
        raise ValueError(f"not an expected generated local file: {resolved}")
    if not resolved.is_file():
        raise ValueError(f"generated file is missing: {resolved}")
    return resolved


def main() -> None:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--source-audio", type=Path, action="append", default=[],
                        help="Generated CASE/source/master.atmos.audio")
    parser.add_argument("--oracle-manifest", type=Path, action="append", default=[],
                        help="Research-only oracle PCM manifest with output hashes")
    parser.add_argument("--output", type=Path, required=True)
    args = parser.parse_args()
    if args.output.exists():
        parser.error("prune manifest exists")
    selected = {}
    for path in args.source_audio:
        source = checked_path(path, "master.atmos.audio")
        case = source.parent.parent / "case.json"
        final_adm = source.parent.parent / "normalized/output.wav"
        if not case.is_file() or not final_adm.is_file():
            raise ValueError(f"source has no retained case description or final ADM: {source}")
        selected[str(source)] = {"path": str(source), "sha256": file_sha256(source),
                                 "bytes": source.stat().st_size, "kind": "DAMF source PCM",
                                 "rebuild_from": str(case.resolve())}
    for manifest_path in args.oracle_manifest:
        manifest = json.loads(manifest_path.read_text())
        if not manifest.get("research_only"):
            raise ValueError(f"not a research-only oracle manifest: {manifest_path}")
        for item in manifest["outputs"].values():
            path = checked_path(Path(item["path"]), Path(item["path"]).name)
            if file_sha256(path) != item["sha256"]:
                raise ValueError(f"oracle PCM changed: {path}")
            selected[str(path)] = {"path": str(path), "sha256": item["sha256"],
                                   "bytes": path.stat().st_size, "kind": "research oracle PCM",
                                   "rebuild_from": str(manifest_path.resolve())}
    if not selected:
        parser.error("no generated files selected")
    report = {"status": "verified_before_deletion", "items": list(selected.values()),
              "total_bytes": sum(item["bytes"] for item in selected.values())}
    args.output.write_text(json.dumps(report, indent=2) + "\n")
    for item in selected.values():
        Path(item["path"]).unlink()
    report["status"] = "deleted_after_verification"
    args.output.write_text(json.dumps(report, indent=2) + "\n")
    print(args.output)


if __name__ == "__main__":
    main()
