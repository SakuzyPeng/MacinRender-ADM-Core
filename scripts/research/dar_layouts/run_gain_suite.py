#!/usr/bin/env python3
"""Generate one synthetic ADM, capture both layouts, validate and repeat."""

import argparse
import json
import subprocess
import sys
import wave
from pathlib import Path

from gain_trace_adm import inspect_adm
from measure_gain_trace import validate
from measure_point_suite import file_sha256
from run_gain_probe import build_batch

HERE = Path(__file__).resolve().parent
ROOT = HERE.parents[2]


def run(command, log):
    with log.open("w") as output:
        result = subprocess.run([str(value) for value in command], stdout=output, stderr=subprocess.STDOUT)
    if result.returncode:
        raise RuntimeError(f"command failed ({result.returncode}); see {log}")


def save_regression_audio(root, adm, layouts):
    destination = root / "regression-audio"
    destination.mkdir()
    windows = [{"event": event["id"], "source_start_sample": event["start_sample"] +
                max(0, event["duration_samples"] - 8192), "frames": min(512, event["duration_samples"]),
                "xyz": event["xyz"], "size": event["size"]}
               for obj in adm["objects"] for event in obj["events"]]
    inputs = {"input": Path(adm["path"])}
    for layout in layouts:
        baseline = json.loads((root / (layout + "-baseline") / "export/run.json").read_text())
        inputs[layout] = Path(baseline["outputs"][0]["path"])
    report = {"source_adm_sha256": adm["sha256"], "windows": windows, "files": []}
    for name, path in inputs.items():
        target = destination / (name + ".wav")
        with wave.open(str(path), "rb") as reader, wave.open(str(target), "wb") as writer:
            writer.setnchannels(reader.getnchannels())
            writer.setsampwidth(reader.getsampwidth())
            writer.setframerate(reader.getframerate())
            for window in windows:
                reader.setpos(window["source_start_sample"])
                writer.writeframes(reader.readframes(window["frames"]))
        report["files"].append({"path": str(target), "sha256": file_sha256(target), "bytes": target.stat().st_size})
    (destination / "manifest.json").write_text(json.dumps(report, indent=2) + "\n")


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--cases", type=Path, default=HERE / "gain_minimal_cases.json")
    parser.add_argument("--adm", type=Path, help="reuse a previously generated final ADM instead of regenerating")
    parser.add_argument("--profile", type=Path, default=HERE / "renderer55_gain_profile.json")
    parser.add_argument("--channel-map", type=Path, required=True)
    parser.add_argument("--output-dir", type=Path, required=True)
    parser.add_argument("--layouts", default="7.1.4,9.1.6")
    parser.add_argument("--repeat", type=int, default=2, help="fresh normal-export initialization per capture")
    parser.add_argument("--keep-intermediates", action="store_true", help="keep uncompressed traces and duplicate traced WAVs")
    args = parser.parse_args()
    layouts = args.layouts.split(",")
    if (not layouts or len(layouts) != len(set(layouts)) or
            any(item not in ("7.1.4", "9.1.6") for item in layouts) or not 2 <= args.repeat <= 3):
        parser.error("use unique supported layouts and 2 or 3 repeated captures")
    root = args.output_dir.resolve()
    root.mkdir(parents=True, exist_ok=False)
    report = {"entry_mode": "normal_render_capture", "independent_direct_entry": False, "layouts": {}, "success": False}
    try:
        if args.adm:
            adm, manifest = args.adm.resolve(), {"reused_final_adm": str(args.adm.resolve())}
        else:
            adm, manifest = build_batch(args.cases, root / "probe")
        report["adm"] = inspect_adm(adm)
        report["manifest"] = manifest
        cli = ROOT / "build/release/mradm"
        cache = (ROOT / "build/release/CMakeCache.txt").read_text()
        if "CMAKE_BUILD_TYPE:STRING=Release" not in cache:
            raise ValueError("comparison audio requires the Release build tree")
        run(["cmake", "--build", "--preset", "release", "--target", "mradm"], root / "release-build.log")
        report["release_binary_sha256"] = file_sha256(cli)
        for layout in layouts:
            print(f"{layout}: baseline, Release point regression, {args.repeat} independent captures", flush=True)
            base = root / (layout + "-baseline")
            common = [sys.executable, HERE / "run_gain_probe.py", "trace", "--adm", adm,
                      "--layout", layout, "--profile", args.profile]
            run([*common, "--without-breakpoints", "--output-dir", base], root / (layout + "-baseline.log"))
            point = root / (layout + "-point.wav")
            run([cli, "render", "-i", adm, "-o", point, "--renderer", "triple-balance",
                 "--speaker-spread-mode", "none", "--output-layout", layout, "--no-peak-limit",
                 "--output-bit-depth", "f32"], root / (layout + "-point.log"))
            results = []
            for index in range(args.repeat):
                destination = root / (layout + f"-trace-{index + 1}")
                run([*common, "--baseline-run", base / "export/run.json", "--output-dir", destination],
                    root / (layout + f"-trace-{index + 1}.log"))
                measured = validate(destination, args.channel_map, point)
                results.append({"root": str(destination), "passed": measured["passes_capture_validation"],
                                "max_errors": measured["max_errors"], "parameters": measured["parameters"],
                                "point_regression": measured.get("point_regression")})
            previous = results[0]["parameters"]
            # Ignore runtime addresses and scheduler order. The validated samples,
            # metadata, and actual gain vectors must agree after fresh initialization.
            repeatable = all(item["parameters"] == previous for item in results[1:])
            covered = {item["adm_event"] for item in previous}
            required = {item["id"] for obj in report["adm"]["objects"] for item in obj["events"]}
            report["layouts"][layout] = {"runs": results, "repeatable_parameters_and_gains": repeatable,
                                         "all_events_covered": covered == required,
                                         "passed": repeatable and covered == required and all(item["passed"] for item in results)}
            (root / "suite-report.json").write_text(json.dumps(report, indent=2) + "\n")
        report["success"] = all(item["passed"] for item in report["layouts"].values())
        if report["success"]:
            save_regression_audio(root, report["adm"], layouts)
        if report["success"] and not args.keep_intermediates:
            from compact_gain_artifacts import compact
            report["storage"] = {"manifest": str(root / "storage-manifest.json"),
                                 "saved_bytes": compact(root)["saved_bytes"]}
    except Exception as error:
        report["error"] = str(error)
        report["success"] = False
    (root / "suite-report.json").write_text(json.dumps(report, indent=2) + "\n")
    print(root / "suite-report.json")
    if not report["success"]:
        raise SystemExit(1)


if __name__ == "__main__":
    main()
