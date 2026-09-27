#!/usr/bin/env python3
"""Version-locked, single-session tracing of Renderer ADM speaker gains."""

import argparse
import fcntl
import hashlib
import json
import math
import subprocess
import sys
from pathlib import Path

import run_headless_rerender as rerender
from gain_trace_adm import inspect_adm

HERE = Path(__file__).resolve().parent


def read_json(path):
    return json.loads(path.read_text(encoding="utf-8"))


def decoded_pcm_hash(path):
    result = subprocess.run(["ffmpeg", "-v", "error", "-i", str(path), "-map", "0:a:0",
                             "-c:a", "pcm_s24le", "-f", "hash", "-hash", "sha256", "-"],
                            check=True, capture_output=True, text=True)
    return result.stdout.strip()


def build_batch(path, destination):
    from make_calibration_suite import write_motion_case

    request = read_json(path)
    if request.get("sample_rate", 48_000) != 48_000:
        raise ValueError("gain batches support only 48 kHz")
    cases = request["cases"]
    if not cases or len({item["id"] for item in cases}) != len(cases):
        raise ValueError("batch IDs must be nonempty and unique")
    events = []
    previous = None
    for item in cases:
        xyz = item["xyz"]
        size = float(item["size"])
        sample = int(item["start_sample"])
        if (len(xyz) != 3 or not all(math.isfinite(float(v)) for v in xyz) or
            abs(xyz[0]) > 1 or abs(xyz[1]) > 1 or not 0 <= xyz[2] <= 1 or
            not math.isfinite(size) or not 0 <= size <= 1):
            raise ValueError(f"unsupported Cartesian room case: {item}")
        if (previous is None and sample != 0) or (previous is not None and sample - previous < 512):
            raise ValueError("batch starts at frame 0 and supports one event per 512-frame interval")
        previous = sample
        events.append({"start_samples": sample, "position": list(xyz), "size": size,
                       "ramp_samples": int(item.get("ramp_samples", 0)), "label": item["id"]})
    frames = int(request.get("duration_samples", cases[-1]["start_sample"] + 48_000))
    if frames <= cases[-1]["start_sample"]:
        raise ValueError("batch duration ends before its last event")
    destination.mkdir()
    manifest = write_motion_case(destination, "gain_batch", events, frames, 0x4761696E)
    manifest["batch_cases"] = cases
    manifest["mode"] = "normal_render_capture"
    (destination / "manifest.json").write_text(json.dumps(manifest, indent=2) + "\n")
    return Path(manifest["adm"]), manifest


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("mode", choices=("trace", "batch"))
    parser.add_argument("--adm", type=Path)
    parser.add_argument("--cases", type=Path)
    parser.add_argument("--layout", choices=("7.1.4", "9.1.6"), required=True)
    parser.add_argument("--profile", type=Path, required=True,
                        help="Version-locked module identity and discovered breakpoint definitions")
    parser.add_argument("--output-dir", type=Path, required=True)
    parser.add_argument("--without-breakpoints", action="store_true",
                        help="Produce the uninstrumented control through the same export transport")
    parser.add_argument("--baseline-run", type=Path, help="A prior normal export run.json for PCM identity checks")
    args = parser.parse_args()
    destination = args.output_dir.expanduser().resolve()
    if destination.exists():
        parser.error("output directory must be new")
    if args.mode == "trace" and (args.adm is None or args.cases is not None):
        parser.error("trace requires --adm and does not accept --cases")
    if args.mode == "batch" and (args.cases is None or args.adm is not None):
        parser.error("batch requires --cases and does not accept --adm")
    profile = read_json(args.profile)
    if rerender.file_sha256(rerender.APP) != profile["binary_sha256"]:
        raise ValueError("installed Renderer differs from the identified binary")
    destination.mkdir(parents=True)
    batch = None
    if args.mode == "batch":
        adm, batch = build_batch(args.cases, destination / "probe")
    else:
        adm = args.adm.expanduser().resolve()
    if not adm.is_file():
        raise ValueError(f"ADM does not exist: {adm}")
    adm_hash = rerender.file_sha256(adm)
    adm_identity = inspect_adm(adm)
    (destination / "adm-verification.json").write_text(json.dumps(adm_identity, indent=2) + "\n")
    resident = profile.get("transport") == "resident"
    config = {"module_uuid": profile["module_uuid"], "binary_sha256": profile["binary_sha256"],
              "image_base": profile["image_base"],
              "trace_file": str(destination / ("trace.json" if resident else "trace.jsonl")),
              "adm": str(adm), "adm_sha256": adm_hash,
              "capture": not args.without_breakpoints,
              "breakpoints": [] if args.without_breakpoints else profile.get("breakpoints", []),
              "export_arguments": ["--adm", str(adm), "--output-dir", str(destination / "export"),
                                   "--layouts", args.layout]}
    config["collector_sources_sha256"] = {name: rerender.file_sha256(HERE / name) for name in
        ("dar_gain_resident.cpp", "dar_gain_oar_capture.h", "dar_batch_bridge.cpp", "dar_resident_lldb.py")}
    if resident:
        config.update({key: profile[key] for key in ("vtable_slot", "gain_function", "hooks", "size_dispatch") if key in profile})
        config["max_records"] = profile.get("max_records", 2048)
        config["record_stride"] = profile.get("record_stride", 16)
        config["output_channels"] = 12 if args.layout == "7.1.4" else 16
    config_path = destination / "trace-config.json"
    config_path.write_text(json.dumps(config, indent=2) + "\n")
    pid = rerender.renderer_pid()
    command = ["xcrun", "lldb", "--batch"]
    if profile.get("ignore_mach_exceptions") and not resident:
        exceptions = profile["ignore_mach_exceptions"]
        if exceptions != "EXC_BREAKPOINT":
            raise ValueError("only the investigated Renderer EXC_BREAKPOINT pass-through is supported")
        command += ["-o", f"settings set platform.plugin.darwin.ignored-exceptions {exceptions}"]
        config["ignore_mach_exceptions"] = exceptions
        config_path.write_text(json.dumps(config, indent=2) + "\n")
    command += ["-o", f"process attach --pid {pid}",
                "-o", f"command script import {HERE / 'dar_gain_lldb.py'}",
                "-o", f"dar-gain-trace {config_path}"]
    if resident:
        command = [sys.executable, str(HERE / "run_gain_resident.py"), str(config_path)]
    with (destination / "lldb.log").open("w", encoding="utf-8") as output:
        result = subprocess.run(command, stdout=output, stderr=subprocess.STDOUT)
    report = {"mode": "resident_vtable_capture" if resident else "normal_render_capture", "requested_mode": args.mode,
              "adm": str(adm), "adm_sha256": adm_hash, "layout": args.layout,
              "adm_pcm_sha256": adm_identity["pcm_sha256"], "independent_direct_entry": False,
              "profile_sha256": rerender.file_sha256(args.profile), "validation_status": "capture_only",
              "profile": str(args.profile.resolve()), "lldb_exit_code": result.returncode,
              "success": False}
    if batch is not None:
        report["batch"] = batch
    run_path = destination / "export/run.json"
    if run_path.is_file():
        exported = read_json(run_path)
        report["export_run"] = str(run_path)
        report["state_restored"] = (not exported.get("restore_errors") and
                                     exported.get("settings_sha256_after") == exported.get("baseline_settings_sha256"))
        report["success"] = bool(exported.get("success") and report["state_restored"])
        if args.baseline_run and report["success"]:
            baseline = read_json(args.baseline_run)
            if (not baseline.get("success") or baseline.get("restore_errors") or
                    baseline.get("adm_sha256") != adm_hash or
                    baseline.get("settings_sha256_after") != baseline.get("baseline_settings_sha256")):
                raise ValueError("baseline is incomplete or used a different ADM")
            expected = next(item for item in baseline["outputs"] if item["layout"] == args.layout)
            actual = next(item for item in exported["outputs"] if item["layout"] == args.layout)
            first = decoded_pcm_hash(Path(expected["path"]))
            second = decoded_pcm_hash(Path(actual["path"]))
            report["pcm_comparison"] = {"baseline_run": str(args.baseline_run.resolve()),
                                        "baseline_hash": first, "trace_hash": second,
                                        "all_samples_exact": first == second}
            report["success"] &= first == second
    trace = Path(config["trace_file"])
    if trace.is_file():
        if resident:
            captured = read_json(trace)
            report["captured_records"] = len(captured["records"])
            report["total_hook_calls"] = captured["total_calls"]
            report["size_gain_calls"] = captured.get("size_dispatch_calls", 0)
            report["hook_restored"] = captured["hook_restored"]
            report["success"] &= bool(captured["hook_restored"] and not captured.get("error") and
                                       not captured.get("size_dispatch_errors", 0))
            if not args.without_breakpoints:
                report["success"] &= bool(captured["records"])
        else:
            records = [json.loads(line) for line in trace.read_text().splitlines()]
            report["hits"] = {name: sum(row.get("event") == "entry" and row.get("name") == name for row in records)
                              for name in {row["name"] for row in records if "name" in row}}
            report["trace_errors"] = [row for row in records if row["event"].endswith("error")]
            report["success"] &= not report["trace_errors"]
    report["success"] &= result.returncode == 0 and trace.is_file()
    (destination / "summary.json").write_text(json.dumps(report, indent=2) + "\n")
    print(destination / "summary.json")
    if not report["success"]:
        raise SystemExit(1)


if __name__ == "__main__":
    # One owner for the resident/debugger/export flow. Re-entry cannot compete
    # for a target thread or replace another run's scoped callbacks.
    with (rerender.ROOT / "local/dar-gain-capture.lock").open("w") as lock:
        try:
            fcntl.flock(lock, fcntl.LOCK_EX | fcntl.LOCK_NB)
        except BlockingIOError:
            raise SystemExit("another gain capture owns Renderer")
        main()
