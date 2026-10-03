#!/usr/bin/env python3
"""Generate or reuse one ADM suite and measure both offline speaker renders."""

import argparse
import json
import subprocess
import sys
from pathlib import Path

HERE = Path(__file__).resolve().parent
ROOT = HERE.parents[2]
RENDER = ROOT / "build/release/mradm"


def run(*arguments: object) -> None:
    subprocess.run([str(argument) for argument in arguments], check=True)


def read_json(path: Path) -> dict:
    return json.loads(path.read_text(encoding="utf-8"))


def validate_reference(path: Path, adm: Path, layouts: set[str]) -> None:
    from measure_static_bank import file_sha256

    report = read_json(path)
    if (not report["success"] or report.get("restore_errors") or Path(report["adm"]).resolve() != adm.resolve() or
            report.get("settings_sha256_after") != report.get("baseline_settings_sha256") or
            report.get("adm_sha256") != file_sha256(adm)):
        raise ValueError(f"reference did not use this ADM: {path}")
    if {item["layout"] for item in report["outputs"]} != layouts:
        raise ValueError(f"reference has wrong layouts: {path}")
    for item in report["outputs"]:
        if file_sha256(Path(item["path"])) != item["sha256"]:
            raise ValueError(f"reference PCM has changed: {item['path']}")


def main() -> None:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--suite-root", type=Path, required=True,
                        help="Existing suite root, or a new directory for --profile")
    parser.add_argument("--profile", choices=("point-bank", "motion", "motion-holdout", "size-sequence",
                                             "size-impulse", "size-prbs-long", "size-transfer",
                                             "size-superposition", "size-motion", "size-motion-boundary",
                                             "size-route-scan", "size-warm-impulse", "size-warm-bank",
                                             "size-warm-bank-geometry", "size-spatial-train",
                                             "size-spatial-validation", "size-two-object",
                                             "gain-control"),
                        help="Generate this profile if --suite-root does not exist")
    parser.add_argument("--channel-map", type=Path, required=True)
    parser.add_argument("--reference-run", type=Path,
                        help="Existing run.json for a one-case suite; defaults to CASE/dar/run.json")
    parser.add_argument("--run-name", default="automated-compat",
                        help="New subdirectory in each case for candidate PCM and reports")
    parser.add_argument("--candidate-size", action="store_true",
                        help="Render native size for long PRBS or size-motion profiles and apply their acceptance score")
    parser.add_argument("--isolated-response", type=Path,
                        help="For size-warm-impulse, compare against a prior isolated impulse NPZ")
    parser.add_argument("--single-object-reference", type=Path,
                        help="For size-two-object, compare against the matching single-object WAV")
    args = parser.parse_args()
    root = args.suite_root.expanduser().resolve()
    run("cmake", "--build", "--preset", "release", "--target", "mradm")
    if not root.exists():
        if args.profile is None:
            parser.error("--profile is required to generate a new suite")
        run(sys.executable, HERE / "make_calibration_suite.py", "--output-root", root,
            "--profile", args.profile)
    manifest = read_json(root / "manifest.json")
    profile = manifest["profile"]
    if args.profile and args.profile != profile:
        parser.error(f"existing suite profile is {profile}, not {args.profile}")
    if profile not in ("point-bank", "motion", "motion-holdout", "size-sequence", "size-impulse",
                       "size-prbs-long", "size-transfer", "size-superposition", "size-motion",
                       "size-motion-boundary", "size-route-scan",
                       "size-warm-impulse", "size-warm-bank", "size-warm-bank-geometry",
                       "size-spatial-train", "size-spatial-validation", "size-two-object",
                       "gain-control"):
        parser.error(f"unsupported suite profile: {profile}")
    if args.candidate_size and profile not in ("size-prbs-long", "size-motion", "size-motion-boundary", "size-route-scan"):
        parser.error("--candidate-size requires long PRBS, size-motion, size-motion-boundary, or size-route-scan")
    if args.reference_run and len(manifest["cases"]) != 1:
        parser.error("--reference-run requires a one-case suite")
    if args.reference_run and not args.reference_run.is_file():
        parser.error(f"reference run does not exist: {args.reference_run}")
    mapping = read_json(args.channel_map)["layouts"]
    layouts = {"7.1.4", "9.1.6"}
    if set(mapping) != layouts or not all(mapping[layout]["all_samples_exact"] for layout in layouts):
        parser.error("7.1.4/9.1.6 sample-verified channel map required")
    if not RENDER.is_file():
        parser.error(f"Release renderer missing: {RENDER}")

    summary = {"profile": profile, "suite_manifest": str((root / "manifest.json").resolve()),
               "channel_map": str(args.channel_map.resolve()), "cases": []}
    for case in manifest["cases"]:
        case_dir = root / case["case_id"]
        work = case_dir / args.run_name
        if work.exists():
            parser.error(f"candidate run directory exists: {work}")
        work.mkdir()
        adm = Path(case["adm"])
        from measure_static_bank import file_sha256
        if file_sha256(adm) != case["sha256"]:
            raise ValueError(f"final ADM changed: {adm}")
        reference = args.reference_run.resolve() if args.reference_run else case_dir / "dar/run.json"
        reference_layouts = layouts
        if reference.exists() and profile in ("size-transfer", "size-superposition", "size-warm-impulse",
                                             "size-route-scan", "size-spatial-train",
                                             "size-spatial-validation", "size-two-object"):
            present = {item["layout"] for item in read_json(reference)["outputs"]}
            if not present or not present.issubset(layouts):
                raise ValueError(f"unexpected research layout set: {present}")
            reference_layouts = present
        if reference.exists():
            validate_reference(reference, adm, reference_layouts)
        else:
            run(sys.executable, HERE / "run_headless_rerender.py", "--adm", adm,
                "--output-dir", case_dir / "dar")
            validate_reference(reference, adm, layouts)
        result = {"case_id": case["case_id"], "adm_sha256": case["sha256"],
                  "reference": str(reference.resolve())}
        if profile in ("size-impulse", "size-prbs-long", "size-transfer", "size-superposition",
                       "size-motion", "size-motion-boundary", "size-route-scan", "size-warm-impulse",
                       "size-warm-bank", "size-warm-bank-geometry", "size-spatial-train",
                       "size-spatial-validation", "size-two-object"):
            scripts = {"size-impulse": ("measure_size_impulses.py", "impulse-report.json"),
                       "size-prbs-long": ("measure_size_field.py", "field-report.json"),
                       "size-transfer": ("measure_size_transfer.py", "transfer-report.json"),
                       "size-superposition": ("measure_size_superposition.py", "superposition-report.json"),
                       "size-motion": ("measure_size_motion.py", "motion-report.json"),
                       "size-motion-boundary": ("measure_size_motion.py", "motion-report.json"),
                       "size-route-scan": ("measure_size_motion.py", "motion-report.json"),
                       "size-warm-impulse": ("measure_size_warm_impulses.py", "warm-impulse-report.json"),
                       "size-warm-bank": ("measure_size_warm_bank.py", "warm-bank-report.json")}
            scripts["size-warm-bank-geometry"] = ("measure_size_warm_bank.py", "warm-bank-report.json")
            scripts["size-spatial-train"] = ("measure_size_spatial_scan.py", "spatial-report.json")
            scripts["size-spatial-validation"] = ("measure_size_spatial_scan.py", "spatial-report.json")
            scripts["size-two-object"] = ("measure_size_two_object.py", "two-object-report.json")
            script, filename = scripts[profile]
            report = work / filename
            measurement = [sys.executable, HERE / script, "--suite-manifest", root / "manifest.json",
                           "--case", case["case_id"], "--dar-run", reference,
                           "--channel-map", args.channel_map, "--output", report]
            if profile == "size-warm-impulse" and args.isolated_response is not None:
                measurement += ["--isolated-response", args.isolated_response]
            if profile == "size-two-object" and args.single_object_reference is not None:
                measurement += ["--single-object-reference", args.single_object_reference]
            if args.candidate_size:
                outputs = {}
                for layout, suffix in (("7.1.4", "714"), ("9.1.6", "916")):
                    wav = work / f"size-{suffix}.wav"
                    run(RENDER, "render", "--input", adm, "--output", wav, "--output-layout", layout,
                        "--renderer", "triple-balance", "--no-peak-limit",
                        "--output-bit-depth", "f32")
                    outputs[layout] = wav
                measurement += ["--candidate-714", outputs["7.1.4"],
                                "--candidate-916", outputs["9.1.6"]]
            run(*measurement)
            if args.candidate_size:
                if profile == "size-prbs-long":
                    score = work / "size-score.json"
                    run(sys.executable, HERE / "score_size_field.py", "--field-report", report, "--output", score)
                    passed = read_json(score)["passes_both_layouts"]
                else:
                    score = report
                    measured = read_json(report)["candidates"]
                    passed = set(measured) == reference_layouts and all(
                        value["passes_dynamic_thresholds"] for value in measured.values())
                result.update(status="pass" if passed else "fail", report=str(report.resolve()),
                              score=str(score.resolve()),
                              measured_layouts=sorted(reference_layouts),
                              candidate={layout: str(path.resolve()) for layout, path in outputs.items()})
            else:
                result.update(status="measured", report=str(report.resolve()),
                              reason="reference-only measurement; request --candidate-size for native comparison")
            summary["cases"].append(result)
            (root / f"{args.run_name}-summary.json").write_text(json.dumps(summary, indent=2) + "\n",
                                                                   encoding="utf-8")
            continue
        candidates = {}
        for layout, suffix in (("7.1.4", "714"), ("9.1.6", "916")):
            output = work / f"room-{suffix}.wav"
            run(RENDER, "render", "--input", adm, "--output", output, "--output-layout", layout,
                "--renderer", "triple-balance", "--speaker-spread-mode", "none",
                "--no-peak-limit", "--output-bit-depth", "f32")
            candidates[layout] = output
        if profile == "size-sequence":
            report = work / "size-energy.json"
            run(sys.executable, HERE / "measure_size_sequence.py", "--suite-manifest", root / "manifest.json",
                "--case", case["case_id"], "--dar-run", reference, "--channel-map", args.channel_map,
                "--candidate-714", candidates["7.1.4"], "--candidate-916", candidates["9.1.6"],
                "--output", report)
            result.update(status="point-baseline", reason="explicit spread=none diagnostic; use long PRBS for size acceptance",
                          report=str(report.resolve()),
                          candidate={layout: str(path.resolve()) for layout, path in candidates.items()})
        else:
            if profile in ("motion", "motion-holdout"):
                report = work / "motion-score.json"
                run(sys.executable, HERE / "compare_motion.py", "--suite-manifest", root / "manifest.json",
                    "--case", case["case_id"], "--dar-run", reference, "--channel-map", args.channel_map,
                    "--candidate-714", candidates["7.1.4"], "--candidate-916", candidates["9.1.6"],
                    "--output", report)
                metrics = read_json(report)["layouts"]
                passed = all(value["max_envelope_relative_rmse"] <= 0.02 and
                             (value["max_onset_error_samples"] is None or
                              value["max_onset_error_samples"] <= 48)
                             for value in metrics.values())
            else:
                gains = work / "signed-gains.json"
                run(sys.executable, HERE / "measure_static_bank.py", "--suite-manifest", root / "manifest.json",
                    "--case", case["case_id"], "--dar-run", reference, "--channel-map", args.channel_map,
                    "--candidate", f"7.1.4={candidates['7.1.4']}",
                    "--candidate", f"9.1.6={candidates['9.1.6']}", "--output", gains)
                report = work / "point-score.json"
                run(sys.executable, HERE / "score_point_results.py", "--reference", gains,
                    "--candidate", gains, "--channel-map", args.channel_map, "--output", report)
                metrics = read_json(report)["layouts"]
                passed = all(value["passes_point_thresholds"] for value in metrics.values())
            result.update(status="pass" if passed else "fail", report=str(report.resolve()),
                          candidate={layout: str(path.resolve()) for layout, path in candidates.items()})
        summary["cases"].append(result)
        (root / f"{args.run_name}-summary.json").write_text(json.dumps(summary, indent=2) + "\n",
                                                               encoding="utf-8")
    print(root / f"{args.run_name}-summary.json")


if __name__ == "__main__":
    main()
