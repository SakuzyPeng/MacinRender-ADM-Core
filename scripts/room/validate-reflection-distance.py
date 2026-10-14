#!/usr/bin/env python3
"""Capture before/after Release renders using one immutable near-wall scene.

Run --stage baseline before changing reflection lookup, then --stage candidate
after rebuilding Release. No HRTF data is copied and no source room is edited.
"""
import argparse
import hashlib
import json
from pathlib import Path
import re
import runpy
import shutil
import subprocess
import sys
import time

import numpy as np

HELPERS = runpy.run_path(str(Path(__file__).with_name("validate-distance.py")))


def sha(path):
    return hashlib.sha256(path.read_bytes()).hexdigest()


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--stage", choices=["baseline", "candidate"], required=True)
    parser.add_argument("--mradm", type=Path, required=True)
    parser.add_argument("--room", type=Path, required=True)
    parser.add_argument("--collection", type=Path, required=True)
    parser.add_argument("--output", type=Path, required=True)
    args = parser.parse_args()
    executable, collection = args.mradm.resolve(), args.collection.resolve()
    root, original_room = args.output.resolve(), args.room.resolve()
    if "debug" in executable.parts:
        parser.error("Audition renders require Release")
    if args.stage == "baseline":
        root.mkdir(parents=True, exist_ok=False)
        room = json.loads(original_room.read_text())
        room.update(id=room["id"] + "-reflection-distance-validation", name="Near-wall distance validation",
                    listener=[.4, 3., 1.2], minimum_distance=.2)
        room["absorption"][0] = [.2] * 8
        for asset in room["assets"].values():
            source, target = original_room.parent / asset["file"], root / asset["file"]
            if sys.platform != "darwin" or subprocess.run(
                    ["/bin/cp", "-c", str(source), str(target)], capture_output=True).returncode:
                shutil.copyfile(source, target)
        (root / "room.json").write_text(json.dumps(room, indent=2) + "\n")
        timeline = HELPERS["source"](root / "source.wav")
        report = dict(schema="mradm.reflection-distance-audition", version=1, profile="release",
                      sample_rate_hz=48000, material_seconds=14, timeline=timeline,
                      source_sha256=sha(root / "source.wav"), room_sha256=sha(root / "room.json"),
                      original_room_sha256=sha(original_room), collection_sha256=sha(collection),
                      room_assets={asset["file"]: sha(root / asset["file"]) for asset in room["assets"].values()},
                      peak_limiter=False, loudness_normalization=False, stages={})
    else:
        report = json.loads((root / "report.json").read_text())
        if "baseline" not in report["stages"]:
            raise ValueError("A completed pre-change baseline is required")
        for key, path in [("source_sha256", root / "source.wav"), ("room_sha256", root / "room.json"),
                          ("original_room_sha256", original_room), ("collection_sha256", collection)]:
            if sha(path) != report[key]:
                raise ValueError(f"Comparison input changed: {key}")
        for name, digest in report["room_assets"].items():
            if sha(root / name) != digest:
                raise ValueError(f"Room asset changed: {name}")
    output = root / f"{args.stage}.wav"
    if output.exists():
        raise ValueError("Stage output already exists")
    prepared = json.loads(subprocess.check_output(
        [str(executable), "room-check", str(root / "room.json"), "--hrir", str(collection)], text=True))
    policy = prepared.get("hrtf_collection", {}).get("early_interpolation")
    if args.stage == "baseline" and policy == "delay-aligned-image-path-radius":
        raise ValueError("Baseline must precede the reflection-distance change")
    if args.stage == "candidate" and policy != "delay-aligned-image-path-radius":
        raise ValueError("Candidate does not enable reflection-distance lookup")
    command = [str(executable), "render", "-i", str(root / "source.wav"), "-o", str(output),
               "--renderer", "room-binaural", "--room", str(root / "room.json"), "--room-hrir", str(collection),
               "--room-metres-per-unit", "1", "--output-bit-depth", "f32", "--no-peak-limit", "--interp-ms", "0",
               "--write-semantic-report", str(root / f"{args.stage}-semantics.json")]
    started = time.perf_counter()
    measured = ["/usr/bin/time", "-l", *command] if sys.platform == "darwin" else command
    result = subprocess.run(measured, text=True, capture_output=True)
    elapsed = time.perf_counter() - started
    (root / f"{args.stage}.log").write_text(result.stdout + result.stderr)
    result.check_returncode()
    pcm = HELPERS["samples"](output)
    if len(pcm) != 14 * 48000 + prepared["tail_frames"] or not np.isfinite(pcm).all() or not np.any(pcm):
        raise ValueError("Invalid PCM or incomplete tail")
    rss = re.search(r"(\d+)\s+maximum resident set size", result.stderr)
    artifacts = [executable, executable.parent / "libmr_adm_render_room.dylib"]
    report["stages"][args.stage] = dict(frames=len(pcm), elapsed_seconds=elapsed,
        maximum_rss_bytes=int(rss[1]) if rss else None, peak=float(np.max(np.abs(pcm))),
        hrir_guard_frames=prepared["hrir_interpolation_guard_frames"] + prepared["hrir_preprocessing_guard_frames"],
        output_sha256=sha(output), prepared=prepared,
        head=subprocess.check_output(["git", "rev-parse", "HEAD"], text=True).strip(),
        release_artifact_sha256={path.name: sha(path) for path in artifacts if path.exists()})
    if args.stage == "candidate":
        stages = report["stages"]
        guard = max(stage["hrir_guard_frames"] for stage in stages.values())
        length = max(stage["frames"] + guard - stage["hrir_guard_frames"] for stage in stages.values())
        aligned, padding = {}, {}
        for name, stage in stages.items():
            leading = guard - stage["hrir_guard_frames"]
            trailing = length - leading - stage["frames"]
            aligned[name] = np.pad(HELPERS["samples"](root / f"{name}.wav").astype(float),
                                   ((leading, trailing), (0, 0)))
            padding[name] = dict(leading_frames=leading, trailing_frames=trailing)
        semantics = [json.loads((root / f"{name}-semantics.json").read_text()) for name in stages]
        if semantics[0] != semantics[1]:
            raise ValueError("Effective metadata differs")
        report["comparison"] = dict(alignment="declared-hrir-guards", frames=length, zero_padding=padding,
            effective_metadata_identical=True,
            relative_pcm_difference=float(np.linalg.norm(aligned["candidate"] - aligned["baseline"]) /
                                          np.linalg.norm(aligned["baseline"])))
    report["scope"] = "Same input and room; only reflection HRTF lookup changes. Numerical comparison is not a listening score."
    (root / "report.json").write_text(json.dumps(report, indent=2) + "\n")
    print(json.dumps(report["stages"][args.stage], indent=2))


if __name__ == "__main__":
    main()
