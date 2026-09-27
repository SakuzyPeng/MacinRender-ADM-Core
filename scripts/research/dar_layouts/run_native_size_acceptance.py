#!/usr/bin/env python3
"""Bounded same-ADM static size acceptance; each batch keeps reports and short PCM."""

import argparse
import hashlib
import json
import random
import subprocess
import sys
import wave
from pathlib import Path

from make_calibration_suite import write_size_prbs_long_case
from measure_point_suite import file_sha256
from render_native_size_candidate import render

HERE = Path(__file__).resolve().parent
ROOT = HERE.parents[2]
KERNEL_FILES = [ROOT / "src/adm_render_vbap" / name for name in
                ("room_compat_size_panner.cpp", "room_compat_size_panner.h",
                 "room_compat_size_processor.cpp", "room_compat_size_processor.h", "room_compat_panner.cpp")]


def kernel_identity():
    return {str(path.relative_to(ROOT)): file_sha256(path) for path in KERNEL_FILES}


def boundary_points():
    return [
        ("origin_zero", (0, 0, 0), 0), ("origin_tiny", (0, 0, 0), .0001),
        ("top_small", (0, 0, 1), .01), ("top_full", (0, 0, 1), 1),
        ("front_left_tiny", (-1, 1, 0), .0001), ("front_right_tiny", (1, 1, 0), .0001),
        ("upper_left", (-1, 1, 1), .25), ("upper_right", (1, 1, 1), .25),
        ("mirror_left", (-.37, .61, .28), .125), ("mirror_right", (.37, .61, .28), .125),
        ("cutoff_before", (.23, -.47, .61), .199), ("cutoff_exact", (.23, -.47, .61), .2),
        ("cutoff_after", (.23, -.47, .61), .201), ("origin_near", (.0001, -.0001, .0001), .5),
        ("rear_near", (-.999, -.999, .001), 1), ("top_near", (.001, .001, .999), .75),
    ]


def run(arguments, log):
    with log.open("w") as output:
        result = subprocess.run([str(x) for x in arguments], stdout=output, stderr=subprocess.STDOUT)
    if result.returncode:
        raise RuntimeError(f"command failed; see {log}")


def retain_audio_and_prune(case, paths, destination):
    """Keep late signal plus the complete explicit tail from each 5 s segment."""
    retained = []
    for name, path in paths.items():
        output = destination / (name + "-regression.wav")
        windows = []
        with wave.open(str(path), "rb") as source:
            # Dolby references and ADM are PCM24. Candidate float WAVs are
            # already represented by the measured archive; retain reference audio.
            with wave.open(str(output), "wb") as writer:
                writer.setparams(source.getparams())
                for obj in case["objects"]:
                    start = obj["signal_stop_sample"] - 512
                    frames = min(48000, source.getnframes() - start)
                    source.setpos(start)
                    writer.writeframes(source.readframes(frames))
                    windows.append({"start_sample": start, "frames": frames, "object": obj["label"]})
        retained.append({"source": str(path), "source_sha256": file_sha256(path),
                         "retained": str(output), "sha256": file_sha256(output), "windows": windows})
    return retained


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--phase", choices=("boundary", "final"), required=True)
    parser.add_argument("--output-dir", type=Path, required=True)
    parser.add_argument("--channel-map", type=Path, required=True)
    parser.add_argument("--freeze", type=Path, help="required for final: successful boundary report")
    parser.add_argument("--seed", type=lambda value: int(value, 0), default=0x26092651)
    parser.add_argument("--keep-pcm", action="store_true")
    parser.add_argument("--resume", action="store_true", help="continue after an export/measurement interruption")
    args = parser.parse_args()
    root = args.output_dir.resolve()
    root.mkdir(parents=True, exist_ok=args.resume)
    run(["cmake", "--build", "--preset", "release", "--target", "mr_adm_room_compat_size_probe"],
        root / "release-build.log")
    identity = kernel_identity()
    if args.phase == "final":
        if not args.freeze:
            parser.error("final set requires --freeze with successful boundary report")
        frozen = json.loads(args.freeze.read_text())
        if not frozen["success"] or frozen["kernel_sha256"] != identity:
            raise ValueError("kernel changed or boundary acceptance is incomplete")
        random_source = random.Random(args.seed)
        # DAMF metadata text uses six significant digits. Four decimal room
        # inputs survive its affine coordinate mapping and the strict final
        # BWF checks; do not relax those checks to accept shifted test points.
        room_value = lambda low, high: round(random_source.uniform(low, high), 4)
        points = [(f"final_{i:02d}", (room_value(-1, 1), room_value(-1, 1), room_value(0, 1)),
                   max(.0001, room_value(0, 1))) for i in range(32)]
    else:
        points = boundary_points()
    report = {"phase": args.phase, "kernel_sha256": identity, "seed": args.seed,
              "release_driver_sha256": file_sha256(ROOT / "build/release/mr_adm_room_compat_size_probe"),
              "source_coordinate_step": .0001 if args.phase == "final" else None,
              "points": points, "batches": [], "success": False}
    if args.resume:
        saved = json.loads((root / "acceptance.json").read_text())
        if saved["kernel_sha256"] != identity or saved["phase"] != args.phase or saved["seed"] != args.seed:
            raise ValueError("cannot resume with changed kernel or test set")
        report = saved
        if "error" in report:
            report.setdefault("interruption_history", []).append(report.pop("error"))
    (root / "acceptance.json").write_text(json.dumps(report, indent=2) + "\n")
    try:
        for begin in range(0, len(points), 4):
            if kernel_identity() != identity:
                raise ValueError("kernel changed while an acceptance set was running")
            case_id = f"batch_{begin // 4:02d}"
            if any(item["case_id"] == case_id for item in report["batches"]):
                continue
            print(f"{args.phase}: cases {begin + 1}–{min(begin + 4, len(points))}/{len(points)}", flush=True)
            saved_manifest = root / case_id / "suite.json"
            if saved_manifest.exists():
                case = json.loads(saved_manifest.read_text())["cases"][0]
                if file_sha256(Path(case["adm"])) != case["sha256"]:
                    raise ValueError("unfinished batch ADM changed")
            else:
                case = write_size_prbs_long_case(root, points[begin:begin + 4], case_id, args.seed + begin)
            directory = Path(case["adm"]).parent.parent
            manifest = directory / "suite.json"
            manifest.write_text(json.dumps({"profile": "size-prbs-long", "cases": [case]}, indent=2) + "\n")
            outputs = {}
            candidates = {}
            combined = None
            for layout in ("7.1.4", "9.1.6"):
                reference = directory / (layout + "-reference")
                exported = None
                attempt = 0
                while reference.exists():
                    saved_run = reference / "export/run.json"
                    if saved_run.exists():
                        value = json.loads(saved_run.read_text())
                        if value.get("success") and not value.get("restore_errors") and value["adm_sha256"] == case["sha256"]:
                            if all(file_sha256(Path(item["path"])) == item["sha256"] for item in value["outputs"]):
                                exported = value
                                break
                    attempt += 1
                    reference = directory / (layout + f"-reference-retry-{attempt}")
                if exported is None:
                    run([sys.executable, HERE / "run_gain_probe.py", "trace", "--adm", case["adm"],
                         "--layout", layout, "--profile", HERE / "renderer55_gain_profile.json",
                         "--without-breakpoints", "--output-dir", reference], directory / (layout + f"-export-{attempt}.log"))
                    exported = json.loads((reference / "export/run.json").read_text())
                if not exported["success"] or exported.get("restore_errors"):
                    raise ValueError("reference failed restoration")
                outputs[layout] = exported["outputs"][0]
                if combined is None:
                    combined = dict(exported)
                candidate = directory / (layout + "-candidate.wav")
                revision = 0
                while candidate.exists():
                    revision += 1
                    candidate = directory / (layout + f"-candidate-{revision}.wav")
                render(Path(case["adm"]), layout, candidate)
                candidates[layout] = candidate
            combined["outputs"] = list(outputs.values())
            combined_path = directory / "reference.json"
            combined_path.write_text(json.dumps(combined, indent=2) + "\n")
            revision = 0
            field = directory / "field.json"
            while field.exists() or field.with_suffix(".npz").exists():
                revision += 1
                field = directory / f"field-{revision}.json"
            run([sys.executable, HERE / "measure_size_field.py", "--suite-manifest", manifest, "--case", case["case_id"],
                 "--dar-run", combined_path, "--channel-map", args.channel_map,
                 "--candidate-714", candidates["7.1.4"], "--candidate-916", candidates["9.1.6"],
                 "--output", field], directory / "measure.log")
            score_path = directory / ("score.json" if revision == 0 else f"score-{revision}.json")
            run([sys.executable, HERE / "score_size_field.py", "--field-report", field, "--output", score_path],
                directory / "score.log")
            score = json.loads(score_path.read_text())
            report["batches"].append({"case_id": case["case_id"], "adm_sha256": case["sha256"],
                                      "score": str(score_path), "passed": score["passes_both_layouts"]})
            retained = retain_audio_and_prune(case, {key: Path(value["path"]) for key, value in outputs.items()}, directory)
            if not args.keep_pcm:
                removal = [directory / "source/master.atmos.audio", Path(case["adm"]),
                           *(Path(value["path"]) for value in outputs.values()),
                           *candidates.values()]
                storage = {"retained": retained, "removed": [{"path": str(path), "bytes": path.stat().st_size,
                                                               "sha256": file_sha256(path)} for path in removal]}
                (directory / "storage.json").write_text(json.dumps(storage, indent=2) + "\n")
                for path in removal:
                    path.unlink()
            (root / "acceptance.json").write_text(json.dumps(report, indent=2) + "\n")
        report["success"] = all(item["passed"] for item in report["batches"])
    except Exception as error:
        report["error"] = str(error)
    (root / "acceptance.json").write_text(json.dumps(report, indent=2) + "\n")
    print(root / "acceptance.json")
    if not report["success"]:
        raise SystemExit(1)


if __name__ == "__main__":
    main()
