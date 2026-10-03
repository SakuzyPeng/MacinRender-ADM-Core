#!/usr/bin/env python3
"""One-at-a-time direct-ADM semantic references and Release candidate checks."""

import argparse
import hashlib
import gzip
import json
import shutil
import subprocess
import sys
from pathlib import Path

import numpy as np

from make_semantic_suite import boundary_cases, final_cases, make_case, prepare_template
from measure_point_suite import decode_wav
from measure_size_field import canonical_pcm
from measure_static_bank import file_sha256, read_multichannel_adm
from run_compat_suite import validate_reference
from score_semantic_pcm import score

HERE = Path(__file__).resolve().parent
ROOT = HERE.parents[2]
MODEL_FILES = [ROOT / path for path in (
    "src/adm_render_triple_balance/semantics.cpp", "src/adm_render_triple_balance/semantics.h",
    "src/adm_render_triple_balance/panner.cpp", "src/adm_render_triple_balance/panner.h",
    "src/adm_render_triple_balance/size_panner.cpp", "src/adm_render_triple_balance/size_panner.h",
    "src/adm_render_triple_balance/size_processor.cpp", "src/adm_render_triple_balance/size_processor.h",
    "src/adm_render_triple_balance/triple_balance_renderer.cpp",
    "src/adm_render_common/speaker_pcm.cpp", "src/adm_render_common/speaker_pcm.h",
    "src/adm_core/semantic_policy.cpp", "src/adm_io/scene_importer.cpp", "src/adm_engine/render_service.cpp",
    "include/adm/scene.h", "include/adm/semantic_policy.h", "include/adm/render.h", "CMakeLists.txt")]
ANALYSIS_FILES = [HERE / name for name in ("score_semantic_pcm.py", "measure_size_field.py", "measure_size_motion.py",
                                          "gain_trace_adm.py", "make_semantic_suite.py", "run_semantic_suite.py",
                                          "measure_gain_trace.py", "renderer55_semantic_parsers.json")]
TRACE_CASES = {"object_start_s0p25": ("object", "7.1.4"),
               "block_start_s0p25": ("block", "9.1.6"),
               "object_gain_0_linear_s0": ("object", "9.1.6"),
               "mute_1_s0": ("object", "7.1.4"),
               "boundary_31_s0": ("object", "9.1.6")}
CONTROL_CASES = {"baseline_s0", "baseline_s0p25", "muted_zero_s0p25"}
WINDOW_CASES = {"block_start_s0p25", "boundary_31_s0", "two_object_handoff"}


def model_identity():
    return {str(path.relative_to(ROOT)): file_sha256(path) for path in MODEL_FILES}


def analysis_identity():
    return {path.name: file_sha256(path) for path in ANALYSIS_FILES}


def read_json(path):
    return json.loads(path.read_text())


def cached_reference(directory, output, adm_hash):
    """Recover only a losslessly retained, hash-verified canonical reference."""
    layout = output["layout"]
    for filename in ("reference-result.json", "candidate-result.json", "candidate-before-complete-scoring.json"):
        metadata = directory / filename
        if not metadata.exists():
            continue
        result = read_json(metadata)
        row = result.get("layouts", {}).get(layout, {})
        if result.get("adm_sha256") != adm_hash or not row.get("decoded_pcm_sha256"):
            continue
        own = directory / "reference-pcm.npz"
        baseline = directory.parent / ("baseline_s" + str(result["specification"].get("size", 0)).replace(".", "p")) / "reference-pcm.npz"
        paths = [own]
        if row.get("baseline_all_samples_exact"):
            paths.append(baseline)
        for archive in paths:
            if not archive.exists():
                continue
            with np.load(archive) as saved:
                if layout + "_pcm" not in saved:
                    continue
                pcm = saved[layout + "_pcm"]
            digest = hashlib.sha256(pcm.astype("<f4").tobytes()).hexdigest()
            if digest != row["decoded_pcm_sha256"]:
                raise ValueError(f"retained reference PCM hash mismatch: {archive}")
            return pcm
    return None


def execute(command, log):
    with log.open("w") as output:
        result = subprocess.run([str(value) for value in command], stdout=output, stderr=subprocess.STDOUT)
    return result.returncode


def export_reference(adm, directory, profile):
    renderer_log = Path.home() / "Library/Logs/Dolby/Dolby Atmos Renderer/RendererApp.log"
    log_offset = renderer_log.stat().st_size if renderer_log.exists() else 0
    config = {key: profile[key] for key in ("module_uuid", "binary_sha256", "image_base")}
    config.update({"capture": False, "trace_file": str(directory / "reference/control-trace.json"),
                   "export_arguments": ["--adm", str(adm), "--output-dir", str(directory / "reference")]})
    config_path = directory / "reference-config.json"
    config_path.write_text(json.dumps(config, indent=2) + "\n")
    status = execute([sys.executable, HERE / "run_gain_resident.py", config_path], directory / "export.log")
    if renderer_log.exists():
        with renderer_log.open("rb") as stream:
            stream.seek(log_offset)
            (directory / "renderer.log").write_bytes(stream.read())
    return status


def verified_trace(summary_path, adm_hash, layout):
    summary = read_json(summary_path)
    validation_path = summary_path.parent / "gain-validation.json"
    if (not summary.get("success") or summary.get("adm_sha256") != adm_hash or
            summary.get("layout") != layout or not validation_path.exists()):
        return None
    validation = read_json(validation_path)
    if (not validation.get("passes_capture_validation") or not validation.get("pcm_control_exact") or
            not any(item.get("records", 0) > 0 for item in validation.get("parser_hooks", []))):
        return None
    trace = summary_path.parent / "trace.json"
    data = trace.read_bytes() if trace.exists() else gzip.decompress(trace.with_suffix(".json.gz").read_bytes())
    if hashlib.sha256(data).hexdigest() != validation["trace_sha256"]:
        raise ValueError("retained trace hash changed")
    return {"summary": str(summary_path.resolve()), "validation": str(validation_path.resolve()),
            "validation_sha256": file_sha256(validation_path), "passes": True}


def capture_case(root, directory, case, profile):
    kind, layout = TRACE_CASES[case["case_id"]]
    candidates = list(directory.glob("trace-*/summary.json")) + list(root.parent.glob("traces*/**/summary.json"))
    for summary in candidates:
        verified = verified_trace(summary, case["sha256"], layout)
        if verified:
            return verified
    parser_profile = read_json(HERE / "renderer55_semantic_parsers.json")
    if any(parser_profile[key] != profile[key] for key in ("module_uuid", "binary_sha256")):
        raise ValueError("semantic parser and gain profiles use different binaries")
    selected = dict(profile)
    selected["hooks"] = [*profile["hooks"], parser_profile[kind]]
    selected["max_records"] = 256
    profile_path = directory / "semantic-trace-profile.json"
    profile_path.write_text(json.dumps(selected, indent=2) + "\n")
    generation = 1
    while (directory / f"trace-{generation}").exists():
        generation += 1
    output = directory / f"trace-{generation}"
    source = directory / f"trace-input-{generation}.wav"
    shutil.copyfile(case["adm"], source)
    control_run = directory / "reference/run.json"
    control = read_json(control_run)
    if any(not Path(item["path"]).exists() for item in control["outputs"]):
        # Re-capture a real control for trace validation. Previously pruned PCM
        # may be used for scoring, but is never disguised as an existing WAV.
        control_root = directory / f"trace-control-{generation}"
        control_root.mkdir()
        if export_reference(Path(case["adm"]), control_root, profile):
            raise RuntimeError("trace control export failed")
        control_run = control_root / "reference/run.json"
        validate_reference(control_run, Path(case["adm"]), {"7.1.4", "9.1.6"})
        control = read_json(control_run)
        mapping = read_json(root / "channel-map.json")["layouts"]
        for item in control["outputs"]:
            old = cached_reference(directory, item, case["sha256"])
            actual = canonical_pcm(decode_wav(Path(item["path"]), item["channels"], case["duration_samples"]),
                                   item["layout"], mapping[item["layout"]], False)
            if old is not None and not np.array_equal(old, actual):
                raise ValueError("reinitialized control differs from retained reference")
    if execute([sys.executable, HERE / "run_gain_probe.py", "trace", "--adm", source, "--layout", layout,
                "--profile", profile_path, "--output-dir", output,
                "--baseline-run", control_run], directory / "trace.log"):
        raise RuntimeError(f"semantic trace failed; see {directory / 'trace.log'}")
    if execute([sys.executable, HERE / "measure_gain_trace.py", "--trace-root", output,
                "--channel-map", root / "channel-map.json"], directory / "trace-validation.log"):
        raise RuntimeError("semantic trace numeric validation failed")
    verified = verified_trace(output / "summary.json", case["sha256"], layout)
    if not verified:
        raise RuntimeError("semantic parser did not hit or traced mixing was not verified")
    # A control WAV still exists here; compact only the proven duplicate trace PCM.
    from compact_gain_artifacts import compact
    compact(output)
    return verified


def diagnostic(x, y):
    energy = np.sum(y.astype(np.float64) ** 2, axis=0)
    gain = x.astype(np.float64) @ y / max(float(x.astype(np.float64) @ x), 1e-30)
    blocks = []
    for first in range(0, len(x), 512):
        xx = x[first:first + 512].astype(np.float64)
        yy = y[first:first + 512].astype(np.float64)
        blocks.append(np.sum(yy * yy, axis=0) / max(float(xx @ xx), 1e-12))
    active = np.flatnonzero(np.any(np.abs(y) > 2 ** -23, axis=1))
    return {"signed_gain": gain.tolist(), "channel_energy": energy.tolist(),
            "first_nonzero_sample": int(active[0]) if len(active) else None,
            "last_nonzero_sample": int(active[-1]) if len(active) else None,
            "decoded_pcm_sha256": hashlib.sha256(y.astype("<f4").tobytes()).hexdigest(),
            "finite": bool(np.isfinite(y).all())}, np.asarray(blocks)


def render_check(directory, case, layout, mapping, channels, frames, label, extra):
    output = directory / f"check-{label}-{layout}.wav"
    status = execute([ROOT / "build/release/mradm", "render", "-i", case["adm"], "-o", output,
                      "--renderer", "triple-balance", "--output-layout", layout,
                      "--no-peak-limit", "--output-bit-depth", "f32", *extra],
                     directory / f"check-{label}-{layout}.log")
    if status:
        return None
    value = canonical_pcm(decode_wav(output, channels, frames), layout, mapping, True)
    output.unlink()
    return value


def user_and_window_checks(directory, case, layout, mapping, baseline):
    checks = {}
    channels = baseline.shape[1]
    if case["case_id"] in CONTROL_CASES:
        owner = case["identity"]["objects"][0]["object_ids"][0]
        policies = {"half": ({"global": {"gain": {"scale": .5, "mute": False}}}, .5),
                    "mute": ({"global": {"gain": {"mute": True}}}, 0),
                    "precedence": ({"global": {"gain": {"scale": 2}},
                                    "objects": [{"id": owner, "gain": {"scale": .25}}]}, .25)}
        for label, (policy, scale) in policies.items():
            policy_path = directory / f"check-{label}.json"
            policy_path.write_text(json.dumps({"schema": "mradm.semantic-policy.v1", **policy}) + "\n")
            observed = render_check(directory, case, layout, mapping, channels, len(baseline), label,
                                    ["--semantic-policy", policy_path])
            error = float(np.max(np.abs(observed - baseline * scale))) if observed is not None else None
            checks[label] = {"max_error": error, "passes": error is not None and error <= 2e-7}
    if case["case_id"] in WINDOW_CASES:
        first, last = (47001, 102001)
        crop = render_check(directory, case, layout, mapping, channels, last - first, "crop",
                            ["--start", f"{first / 48000:.12f}", "--end", f"{last / 48000:.12f}"])
        repeat = render_check(directory, case, layout, mapping, channels, len(baseline), "repeat", [])
        checks["crop"] = {"passes": crop is not None and bool(np.array_equal(crop, baseline[first:last]))}
        checks["repeat"] = {"passes": repeat is not None and bool(np.array_equal(repeat, baseline))}
    return {"checks": checks, "passes": all(item["passes"] for item in checks.values())}


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--output-dir", type=Path, required=True)
    parser.add_argument("--channel-map", type=Path, required=True)
    parser.add_argument("--only", help="Comma-separated case IDs; size baselines are included automatically")
    parser.add_argument("--resume", action="store_true")
    parser.add_argument("--candidate", action="store_true")
    parser.add_argument("--keep-audio", action="store_true")
    parser.add_argument("--phase", choices=("boundary", "final"), default="boundary")
    parser.add_argument("--freeze", type=Path, help="Passed boundary acceptance.json; required for final phase")
    parser.add_argument("--seed", type=lambda value: int(value, 0), default=0x26092653)
    parser.add_argument("--skip-trace", action="store_true", help="PCM-only diagnostic run; not full boundary acceptance")
    args = parser.parse_args()
    root = args.output_dir.resolve()
    root.mkdir(parents=True, exist_ok=True)
    configuration = {"phase": args.phase, "seed": args.seed,
                     "signal_seed": args.seed if args.phase == "final" else 0x53454D41}
    configuration_path = root / "suite-configuration.json"
    if configuration_path.exists() and read_json(configuration_path) != configuration:
        parser.error("suite phase or seed changed; use a new output directory")
    configuration_path.write_text(json.dumps(configuration, indent=2) + "\n")
    template = prepare_template(root, configuration["signal_seed"])
    profile = read_json(HERE / "renderer55_gain_profile.json")
    mapping = read_json(args.channel_map)["layouts"]
    mapping_copy = root / "channel-map.json"
    if mapping_copy.exists() and file_sha256(mapping_copy) != file_sha256(args.channel_map):
        parser.error("suite channel mapping changed")
    if mapping_copy.resolve() != args.channel_map.resolve():
        shutil.copyfile(args.channel_map, mapping_copy)
    frozen_model = model_identity()
    frozen_analysis = analysis_identity()
    if args.phase == "final":
        if args.freeze is None or not args.candidate or args.only:
            parser.error("final requires --freeze and --candidate, without --only")
        freeze = read_json(args.freeze)
        if (not freeze.get("passes") or not freeze.get("scope_complete") or freeze.get("phase") != "boundary" or
                freeze["model_identity"] != frozen_model or freeze.get("analysis_identity") != frozen_analysis):
            parser.error("boundary acceptance did not pass for this exact model")
    specifications = final_cases(args.seed) if args.phase == "final" else boundary_cases()
    if args.only:
        wanted = set(args.only.split(","))
        if wanted - {item["id"] for item in specifications}:
            parser.error("unknown case ID")
        for item in specifications:
            if item["id"] in wanted:
                wanted.add("baseline_s" + str(item.get("size", 0)).replace(".", "p"))
        specifications = [item for item in specifications if item["id"] in wanted]
    specifications.sort(key=lambda item: (not item["id"].startswith("baseline"), item["id"]))
    (root / "specifications.json").write_text(json.dumps(specifications, indent=2) + "\n")
    if args.candidate:
        if execute(["cmake", "--build", "--preset", "release", "--target", "mradm"], root / "build.log"):
            raise RuntimeError("Release build failed")
    candidate_build = file_sha256(ROOT / "build/release/mradm") if args.candidate else None
    summary = {"reference_profile": profile, "channel_map_sha256": file_sha256(args.channel_map),
               "fixed_delay_samples": 0, "global_level": 1, "cases": [], "phase": args.phase,
               "model_identity": frozen_model, "analysis_identity": frozen_analysis,
               "seed": args.seed, "expected_cases": len(specifications), "scope_complete": args.only is None}
    for specification in specifications:
        if (root / "STOP").exists():
            print("stopped between cases; Renderer state already restored", flush=True)
            break
        identity = specification["id"]
        directory = root / identity
        directory.mkdir(exist_ok=True)
        result_path = directory / ("candidate-result.json" if args.candidate else "reference-result.json")
        if result_path.exists():
            if not args.resume:
                raise FileExistsError(result_path)
            result = read_json(result_path)
            if result.get("complete"):
                if result["specification"] != specification:
                    raise ValueError(f"cannot resume changed case: {identity}")
                needs_trace = args.candidate and not args.skip_trace and identity in TRACE_CASES
                if not args.candidate or ("passes_both_layouts" in result and
                                           result.get("candidate_build_sha256") == candidate_build and
                                           result.get("model_identity") == frozen_model and
                                           result.get("analysis_identity") == frozen_analysis and
                                           (not needs_trace or result.get("trace", {}).get("passes"))):
                    summary["cases"].append(result)
                    continue
        print(f"语义探针 {identity}", flush=True)
        case = make_case(template, directory, specification)
        run_path = directory / "reference/run.json"
        if run_path.exists():
            prior = read_json(run_path)
            trace_write_failure = (not prior.get("success") and
                                   any("trace_written': False" in text for text in prior.get("restore_errors", [])) and
                                   prior["baseline_settings_sha256"] == prior["settings_sha256_after"] and
                                   prior.get("initial", {}).get("master") == prior.get("final", {}).get("master") and
                                   prior.get("initial", {}).get("config") == prior.get("final", {}).get("config"))
            if trace_write_failure or (prior.get("success") and
                                       any(not Path(item["path"]).exists() and
                                           cached_reference(directory, item, case["sha256"]) is None
                                           for item in prior["outputs"])):
                generation = 1
                while (directory / f"reference-pruned-{generation}").exists():
                    generation += 1
                (directory / "reference").rename(directory / f"reference-pruned-{generation}")
        if not run_path.exists():
            export_reference(Path(case["adm"]), directory, profile)
        reference = read_json(run_path)
        result = {"id": identity, "specification": specification, "adm_sha256": case["sha256"],
                  "export_success": reference["success"], "layouts": {}, "complete": False,
                  "candidate_build_sha256": candidate_build,
                  "model_identity": frozen_model if args.candidate else None,
                  "analysis_identity": frozen_analysis if args.candidate else None}
        if not reference["success"]:
            result["reference_failure"] = reference
            result["complete"] = not reference.get("restore_errors") and (
                reference["baseline_settings_sha256"] == reference["settings_sha256_after"])
            result_path.write_text(json.dumps(result, indent=2) + "\n")
            summary["cases"].append(result)
            if not result["complete"]:
                raise RuntimeError("reference state restoration failed; stop before next case")
            if identity.startswith("baseline"):
                raise RuntimeError("baseline export failed; cannot classify semantic probes")
            continue
        if (reference["adm_sha256"] != case["sha256"] or
                reference.get("restore_errors") or
                reference["baseline_settings_sha256"] != reference["settings_sha256_after"] or
                {item["layout"] for item in reference["outputs"]} != set(mapping)):
            raise ValueError("reference identity, layout or restoration mismatch")
        if args.candidate and not args.skip_trace and identity in TRACE_CASES:
            result["trace"] = capture_case(root, directory, case, profile)
        source = read_multichannel_adm(Path(case["adm"]), case["identity"]["channels"], case["duration_samples"])
        object_channels = [item["input_channel"] for item in case["identity"]["objects"]]
        x = source[:, object_channels].sum(axis=1)
        arrays = {}
        prunable = [Path(case["adm"])]
        for output in reference["outputs"]:
            layout = output["layout"]
            if Path(output["path"]).exists():
                if file_sha256(Path(output["path"])) != output["sha256"]:
                    raise ValueError("reference WAV changed")
                y = canonical_pcm(decode_wav(Path(output["path"]), output["channels"], len(x)),
                                  layout, mapping[layout], False)
            else:
                y = cached_reference(directory, output, case["sha256"])
                if y is None:
                    raise ValueError("reference PCM was pruned without a verified retained copy")
            metrics, envelope = diagnostic(x, y)
            result["layouts"][layout] = metrics
            arrays[layout + "_reference_power512"] = envelope
            baseline = root / ("baseline_s" + str(specification.get("size", 0)).replace(".", "p")) / "reference-pcm.npz"
            if identity.startswith("baseline"):
                arrays[layout + "_pcm"] = y
            elif baseline.exists():
                with np.load(baseline) as data:
                    original = data[layout + "_pcm"]
                    metrics["baseline_all_samples_exact"] = bool(np.array_equal(y, original))
                    metrics["baseline_relative_pcm_l2"] = float(np.linalg.norm(y - original) /
                                                               max(np.linalg.norm(original), 1e-12))
            if not metrics.get("baseline_all_samples_exact"):
                arrays[layout + "_pcm"] = y
            if args.candidate:
                candidate = directory / f"candidate-{layout}.wav"
                report = directory / f"semantic-{layout}.json"
                status = execute([ROOT / "build/release/mradm", "render", "-i", case["adm"], "-o", candidate,
                                  "--renderer", "triple-balance", "--output-layout", layout,
                                  "--no-peak-limit", "--output-bit-depth", "f32", "--write-semantic-report", report],
                                 directory / f"candidate-{layout}.log")
                metrics["candidate_exit_code"] = status
                if status == 0:
                    observed = canonical_pcm(decode_wav(candidate, output["channels"], len(x)),
                                             layout, mapping[layout], True)
                    test_metrics, test_envelope = diagnostic(x, observed)
                    metrics["candidate"] = test_metrics
                    metrics["candidate_relative_pcm_l2"] = float(np.linalg.norm(observed - y) /
                                                                 max(np.linalg.norm(y), 1e-12))
                    metrics["candidate_max_pcm_error"] = float(np.max(np.abs(observed - y)))
                    metrics["acceptance"], measurements = score(x, y, observed, case)
                    arrays.update({layout + "_" + key: value for key, value in measurements.items()})
                    metrics["integration"] = user_and_window_checks(directory, case, layout, mapping[layout], observed)
                    arrays[layout + "_candidate_power512"] = test_envelope
                    prunable.append(candidate)
            # Preserve precise transition excerpts while keeping large audio bounded.
            interesting = {0, case["signal_stop_sample"]}
            source_obj = case["identity"]["objects"][0]["source_objects"][0]
            interesting.add(source_obj["start"]["samples"])
            if source_obj["duration"]["samples"] is not None:
                interesting.add(source_obj["start"]["samples"] + source_obj["duration"]["samples"])
            for event in case["identity"]["objects"][0]["events"]:
                interesting.add(event["start_sample"])
                if event["duration_samples"] is not None:
                    interesting.add(event["start_sample"] + event["duration_samples"])
            for sample in sorted(interesting):
                first, last = max(0, sample - 512), min(len(y), sample + 4096)
                if first < last:
                    arrays[f"{layout}_at_{sample}"] = y[first:last]
            if Path(output["path"]).exists():
                prunable.append(Path(output["path"]))
        if any(key.endswith("_pcm") for key in arrays):
            np.savez_compressed(directory / "reference-pcm.npz", **{key: value for key, value in arrays.items()
                                                                  if key.endswith("_pcm")})
            arrays = {key: value for key, value in arrays.items() if not key.endswith("_pcm")}
        np.savez_compressed(directory / "measurements.npz", **arrays)
        result["complete"] = True
        if args.candidate:
            result["passes_both_layouts"] = all(item.get("acceptance", {}).get("passes", False) and
                                                item.get("integration", {}).get("passes", False)
                                                for item in result["layouts"].values())
        result_path.write_text(json.dumps(result, indent=2) + "\n")
        if not args.keep_audio and not identity.startswith("baseline"):
            pruned = []
            for path in prunable:
                pruned.append({"path": str(path), "sha256": file_sha256(path), "bytes": path.stat().st_size})
                path.unlink()
            (directory / "pruned.json").write_text(json.dumps(pruned, indent=2) + "\n")
        summary["cases"].append(result)
        (root / "summary.json").write_text(json.dumps(summary, indent=2) + "\n")
    (root / "summary.json").write_text(json.dumps(summary, indent=2) + "\n")
    if args.candidate:
        summary["model_unchanged"] = (model_identity() == frozen_model and analysis_identity() == frozen_analysis and
                                      file_sha256(ROOT / "build/release/mradm") == candidate_build)
        summary["passes"] = (len(summary["cases"]) == len(specifications) and summary["model_unchanged"] and
                             all(item.get("passes_both_layouts", False) for item in summary["cases"]))
        summary["trace_complete"] = all(item.get("trace", {}).get("passes", False)
                                        for item in summary["cases"] if item["id"] in TRACE_CASES)
        summary["passes"] &= summary["trace_complete"] and not args.skip_trace and summary["scope_complete"]
        summary["candidate_sha256"] = file_sha256(ROOT / "build/release/mradm")
        (root / "acceptance.json").write_text(json.dumps(summary, indent=2) + "\n")
    print(root / "summary.json")


if __name__ == "__main__":
    main()
