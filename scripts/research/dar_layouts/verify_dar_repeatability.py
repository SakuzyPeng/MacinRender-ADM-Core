#!/usr/bin/env python3
"""Verify that independent Renderer exports reproduce the same ADM PCM exactly."""

import argparse
import json
from pathlib import Path

from measure_static_bank import file_sha256


def main() -> None:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--run", type=Path, action="append", required=True)
    parser.add_argument("--layout", choices=("7.1.4", "9.1.6"),
                        help="Compare this layout when runs contain different layout sets")
    parser.add_argument("--output", type=Path, required=True)
    args = parser.parse_args()
    if len(args.run) < 2 or args.output.exists():
        parser.error("at least two runs and a new output path are required")
    reports = [json.loads(path.read_text()) for path in args.run]
    adm = Path(reports[0]["adm"])
    adm_hash = file_sha256(adm)
    layouts = {args.layout} if args.layout else {item["layout"] for item in reports[0]["outputs"]}
    results = []
    for path, report in zip(args.run, reports):
        if not report["success"] or report.get("restore_errors") or (
            report["settings_sha256_after"] != report["baseline_settings_sha256"]
        ):
            raise ValueError(f"Renderer export or restoration incomplete: {path}")
        if Path(report["adm"]).resolve() != adm.resolve() or (
            report.get("adm_sha256", adm_hash) != adm_hash
        ):
            raise ValueError(f"runs did not use identical ADM: {path}")
        if not layouts.issubset({item["layout"] for item in report["outputs"]}):
            raise ValueError(f"required layout missing: {path}")
        wavs = {}
        for item in report["outputs"]:
            if item["layout"] not in layouts:
                continue
            if file_sha256(Path(item["path"])) != item["sha256"]:
                raise ValueError(f"reference WAV changed: {item['path']}")
            wavs[item["layout"]] = item["sha256"]
        results.append({"run": str(path.resolve()), "wav_sha256": wavs,
                        "renderer_version": report.get("renderer_version")})
    identical = all(item["wav_sha256"] == results[0]["wav_sha256"] for item in results[1:])
    result = {"adm": str(adm.resolve()), "adm_sha256": adm_hash, "runs": results,
              "all_output_wav_bytes_identical": identical}
    args.output.write_text(json.dumps(result, indent=2) + "\n")
    if not identical:
        raise ValueError("Renderer repeated exports differ; see output report")
    print(args.output)


if __name__ == "__main__":
    main()
