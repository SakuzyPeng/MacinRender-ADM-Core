#!/usr/bin/env python3
"""Validate captured OMO gain/mix data against the actual ADM re-render PCM.

This checks the observed pipeline; it is not an independently initialized panner
or an implementation of the size renderer. No delay/gain fitting is performed.
"""

import argparse
import bisect
import collections
import gzip
import hashlib
import json
import math
import struct
from pathlib import Path

import numpy as np

from gain_trace_adm import inspect_adm
from measure_point_suite import decode_wav, file_sha256
from measure_static_bank import read_multichannel_adm
from run_gain_probe import decoded_pcm_hash


def regions(record):
    return {item["name"]: bytes.fromhex(item["hex"]) for item in record["regions"]}


def floats(data):
    value = np.frombuffer(data, dtype="<f4").astype(np.float64)
    if not np.isfinite(value).all():
        raise ValueError("nonfinite values in a confirmed float region")
    return value


def integer(data, offset=0, fmt="I"):
    return struct.unpack_from("<" + fmt, data, offset)[0]


def size_mix(raw, size, mode=0, threshold=1e-6):
    """Observed 0x101e6f0cc + 0x101e6e9b8; not a spatial model."""
    phase = min(size / 0.2, 1.0) * math.pi / 2
    dry, wet = math.cos(phase), math.sin(phase)
    if mode == 0:
        dry, wet = dry * dry, wet * wet
    result = np.asarray(raw, dtype=np.float64) * wet
    if mode == 0:
        front, rest = float(result[:3] @ result[:3]), float(result[3:] @ result[3:])
        if front + rest > 1e-6:
            attenuation = 10 ** (-size * 1.2 / 20)
            common = math.sqrt((front + rest) * attenuation ** 2 / (front + rest * attenuation ** 2))
            result[:3] *= common
            result[3:] *= common * attenuation
    result[result < threshold] = 0
    return dry, result


def predict_omo(record):
    q = regions(record)
    x = floats(q["input_pcm"])
    target_weight, previous_weight = floats(q["weight_target"]), floats(q["weight_previous"])
    target, previous = floats(q["object_gains_24"]), floats(q["object_gains_32"])
    other, other_previous = floats(q["object_gains_48"]), floats(q["object_gains_56"])
    state = q["object_state144"]
    current_dry, previous_dry = struct.unpack_from("<2f", state, 40)
    current_other, previous_other = struct.unpack_from("<2f", state, 64)
    dry = x * (current_dry * current_other * target_weight + previous_dry * previous_other * previous_weight)
    envelope = previous[:, None] * previous_weight + target[:, None] * target_weight
    other_envelope = other_previous[:, None] * previous_weight + other[:, None] * target_weight
    wet = envelope * x + other_envelope * x
    mode = integer(q["worker"], 72) & 15
    if "mix_coefficients" in q and np.any(envelope):
        coefficients = floats(q["mix_coefficients"])
        mapping = np.frombuffer(q["filter_channel_map"], dtype="<i4")
        signs = np.frombuffer(q["filter_channel_sign"], dtype="<i4")
        for channel, bank in enumerate(mapping):
            if bank < 0:
                continue
            key = f"filtered_pcm_{bank}"
            if key not in q:
                raise ValueError("missing live filtered PCM needed to validate nonzero size")
            wet[channel] = envelope[channel] * (coefficients[mode] * x +
                                                coefficients[mode + 4] * signs[channel] * floats(q[key]))
            wet[channel] += other_envelope[channel] * x
    group = integer(q["input_event72"], 24) + 4 * integer(q["input_event72"], 28)
    return dry, wet, group


def validate(root: Path, channel_map: Path, candidate: Path | None = None):
    summary = json.loads((root / "summary.json").read_text())
    trace_path = root / "trace.json"
    trace_bytes = trace_path.read_bytes() if trace_path.exists() else gzip.decompress((root / "trace.json.gz").read_bytes())
    trace = json.loads(trace_bytes)
    run = json.loads(Path(summary["export_run"]).read_text())
    if not summary["success"] or not summary.get("pcm_comparison", {}).get("all_samples_exact"):
        raise ValueError("validation requires successful, restored export with an identical untraced control")
    if trace.get("size_dispatch_errors", 0) or not trace["hook_restored"]:
        raise ValueError("callback identity/lifecycle check failed")
    adm = inspect_adm(Path(summary["adm"]))
    if adm["sha256"] != summary["adm_sha256"] or run["adm_sha256"] != adm["sha256"]:
        raise ValueError("ADM changed after capture")
    artifact = next(item for item in run["outputs"] if item["layout"] == summary["layout"])
    if not Path(artifact["path"]).exists():
        # A compacted capture can reuse its bit-identical, retained control.
        baseline = json.loads(Path(summary["pcm_comparison"]["baseline_run"]).read_text())
        if baseline["adm_sha256"] != adm["sha256"] or not baseline["success"] or baseline.get("restore_errors"):
            raise ValueError("invalid retained PCM control")
        artifact = next(item for item in baseline["outputs"] if item["layout"] == summary["layout"])
        if decoded_pcm_hash(Path(artifact["path"])) != summary["pcm_comparison"]["trace_hash"]:
            raise ValueError("retained control is not the verified traced PCM")
    if file_sha256(Path(artifact["path"])) != artifact["sha256"]:
        raise ValueError("reference PCM file changed after export")
    source = read_multichannel_adm(Path(adm["path"]), adm["channels"], adm["frames"]).astype(np.float64)
    reference = decode_wav(Path(artifact["path"]), artifact["channels"], adm["frames"]).astype(np.float64)
    mapping = json.loads(channel_map.read_text())["layouts"][summary["layout"]]
    if not mapping["all_samples_exact"]:
        raise ValueError("unverified frozen channel map")
    canonical = np.zeros_like(reference)
    for item in mapping["interleaved_to_mono"]:
        canonical[:, item["mono_index"]] = reference[:, item["interleaved_index"]]
    callbacks, workers, posts, oars = collections.defaultdict(list), {}, {}, collections.defaultdict(list)
    excluded_posts = 0
    for record in trace["records"]:
        sample = record["sample_start"]
        if record.get("detail") == "size_gain_callback":
            callbacks[int(record["argument1"], 0)].append(record)
        elif record.get("detail") == "omo_mix_inputs":
            index = int(record["argument1"], 0)
            q = regions(record)
            x = floats(q["input_pcm"])
            if sample + len(x) <= len(source) and index < source.shape[1] and np.array_equal(x, source[sample:sample+len(x), index]):
                workers[(sample, index)] = record
        elif record["kind"] == "omo_node":
            q = regions(record)
            count = len(q["input_events72"]) // 72
            x = np.stack([floats(q[f"input_pcm_{i}"]) for i in range(count)], axis=1)
            if (sample + len(x) <= len(source) and x.shape[1] == source.shape[1] and
                    np.any(x) and np.array_equal(x, source[sample:sample+len(x)])):
                posts[sample] = record
            else:
                excluded_posts += 1
        elif record["kind"] == "oar_process":
            oars[sample].append(record)
    for values in callbacks.values():
        values.sort(key=lambda row: row["sample_start"])
    checks, parameter_rows = [], []
    for sample, post in sorted(posts.items()):
        q = regions(post)
        frames = len(q["input_pcm_0"]) // 4
        branch_count = len(q["branches_events72"]) // 72
        predicted_branches = np.zeros((branch_count, frames))
        predicted_dry = np.zeros((adm["channels"], frames))
        max_target_error, max_dry_error = 0.0, 0.0
        complete = True
        for obj in adm["objects"]:
            index = obj["input_channel"]
            worker = workers.get((sample, index))
            x = source[sample:sample + frames, index]
            if worker is None:
                if np.any(x):
                    complete = False
                continue
            state = q[f"object_{index}_state144"]
            size = integer(state, 124, "f")
            target = floats(q[f"object_{index}_size_target"])
            mode = integer(regions(worker)["worker"], 72) & 15
            callback = None
            if size > 0:
                values = callbacks[index]
                at = bisect.bisect_right([row["sample_start"] for row in values], sample) - 1
                if at < 0:
                    raise ValueError("size target has no preceding captured gain callback")
                callback = values[at]
                raw = floats(regions(callback)["size_gains"])
            else:
                raw = np.zeros(11)
            expected_dry, expected_target = size_mix(raw, size, mode)
            target_error = float(np.max(abs(expected_target - target)))
            dry_error = abs(expected_dry - integer(state, 40, "f"))
            max_target_error, max_dry_error = max(max_target_error, target_error), max(max_dry_error, dry_error)
            dry, wet, group = predict_omo(worker)
            predicted_dry[index] = dry
            predicted_branches[group * 11:(group + 1) * 11] += wet
            # Annotation only: numeric validation above uses captured buffers.
            # Direct-ADM consumes rtime on its file control clock, ignoring the
            # object's start. A late first block leaves a real default state.
            event = max((event for event in obj["events"]
                         if event.get("relative_start_sample", event["start_sample"]) // 512 * 512 <= sample),
                        key=lambda event: event.get("relative_start_sample", event["start_sample"]), default=None)
            parameter_rows.append({"sample_start": sample, "input_channel": index,
                                   "object_ids": obj["object_ids"], "track_uid": obj["track_uid"],
                                   "adm_event": event["id"] if event else None,
                                   "adm_xyz": event["xyz"] if event else None,
                                   "adm_size": event["size"] if event else None,
                                   "adm_gain": event["gain"] if event else None,
                                   "source_absolute_start_sample": event["start_sample"] if event else None,
                                   "relative_start_sample": event.get("relative_start_sample", event["start_sample"])
                                       if event else None,
                                   "omo_xyz": list(struct.unpack_from("<3f", q["input_events72"], index * 72 + 36)),
                                   "omo_size": size, "dry_gain": integer(state, 40, "f"),
                                   "size_raw_gains": raw.tolist(), "size_mix_gains": target.tolist(),
                                   "callback_sample_start": callback["sample_start"] if callback else None,
                                   "callback_metadata60_i32": np.frombuffer(regions(callback)["metadata60"], dtype="<i4").tolist() if callback else None})
        if not complete:
            checks.append({"sample_start": sample, "complete": False})
            continue
        direct = np.stack([floats(q[f"direct_pcm_{i}"]) for i in range(adm["channels"])])
        branches = np.stack([floats(q[f"branches_pcm_{i}"]) for i in range(branch_count)])
        omo_output = np.concatenate([direct, branches])
        oar_error, gain_error = 0.0, 0.0
        mixed = np.zeros((artifact["channels"], frames))
        for record in oars.get(sample, []):
            data = regions(record)
            count = integer(data["input_descriptor"], fmt="Q")
            inp = np.stack([floats(data[f"input_channel_{i}"]) for i in range(count)])
            output = np.stack([floats(data[f"output_channel_{i}"]) for i in range(artifact["channels"])])
            selection = data["pcm_selection"]
            if data["processor"][0]:
                start, end = struct.unpack("<2Q", selection)
                indices = list(range(start, end))
            else:
                indices = list(struct.unpack(f"<{count}Q", selection))
            oar_error = max(oar_error, float(np.max(abs(inp - omo_output[indices]))))
            gains = np.stack([floats(data[f"gain_{i}_target"]) for i in range(count)])
            gain_error = max(gain_error, float(np.max(abs(gains.T @ inp - output))))
            mixed += output
        checks.append({"sample_start": sample, "complete": True,
                       "size_gain_max_abs_error": max_target_error, "dry_gain_max_abs_error": max_dry_error,
                       "omo_direct_pcm_max_abs_error": float(np.max(abs(predicted_dry - direct))),
                       "omo_branch_pcm_max_abs_error": float(np.max(abs(predicted_branches - branches))),
                       "omo_to_oar_pcm_max_abs_error": oar_error,
                       "oar_constant_gain_pcm_max_abs_error": gain_error,
                       "summed_output_pcm_max_abs_error": float(np.max(abs(mixed.T - canonical[sample:sample + frames]))),
                       "oar_instances": len(oars.get(sample, []))})
    complete = [row for row in checks if row["complete"]]
    if not complete:
        raise ValueError("no complete PCM-linked samples in trace")
    metrics = {name: max(row[name] for row in complete) for name in complete[0] if name.endswith("error")}
    passed = (len(complete) == len(checks) and
              metrics["size_gain_max_abs_error"] < 2e-6 and metrics["dry_gain_max_abs_error"] < 2e-6 and
              metrics["omo_direct_pcm_max_abs_error"] < 2e-7 and metrics["omo_branch_pcm_max_abs_error"] < 2e-7 and
              metrics["omo_to_oar_pcm_max_abs_error"] < 2e-7 and metrics["summed_output_pcm_max_abs_error"] < 2e-7)
    report = {"scope": "verified normal-render capture; independent direct call is not implemented",
              "layout": summary["layout"], "adm": adm, "trace_sha256": hashlib.sha256(trace_bytes).hexdigest(),
              "channel_map_sha256": file_sha256(channel_map), "pcm_control_exact": True,
              "reference_state_restored": summary["state_restored"], "max_errors": metrics,
              "verified_blocks": len(complete), "excluded_nonmatching_background_posts": excluded_posts,
              "checks": checks, "parameters": parameter_rows, "passes_capture_validation": passed}
    report["parser_hooks"] = [item for item in trace.get("hooks", []) if item["name"].startswith("adm_")]
    report["parser_samples"] = [
        {"kind": item["detail"], "depth": item["argument3"],
         "fields": {part["name"]: bytes.fromhex(part["hex"]).decode("utf-8", errors="strict")
                    for part in item["regions"] if part["name"].endswith("_utf8")},
         "stack": item["stack"]}
        for item in trace["records"] if item.get("detail", "").startswith("adm_")]
    if candidate:
        pcm = decode_wav(candidate, artifact["channels"], adm["frames"]).astype(np.float64)
        # Project WAVE order follows its container mask, matching the frozen
        # interleaved order for these layouts. Convert to the same canonical order.
        candidate_canonical = np.zeros_like(pcm)
        for item in mapping["interleaved_to_mono"]:
            candidate_canonical[:, item["mono_index"]] = pcm[:, item["interleaved_index"]]
        scores = []
        for obj in adm["objects"]:
            for event in obj["events"]:
                if event["size"]:
                    continue
                end = min(event["start_sample"] + event["duration_samples"], len(source))
                begin = max(event["start_sample"] + 24000, end - 8192)
                x = source[begin:end, obj["input_channel"]]
                expected = x @ canonical[begin:end] / (x @ x)
                actual = x @ candidate_canonical[begin:end] / (x @ x)
                scores.append({"event": event["id"], "relative_l2": float(np.linalg.norm(actual - expected) / np.linalg.norm(expected)),
                               "max_abs": float(np.max(abs(actual - expected)))})
        report["point_regression"] = {"candidate_sha256": file_sha256(candidate), "build": "Release",
                                      "scores": scores, "passed": bool(scores) and all(row["relative_l2"] <= .01 and row["max_abs"] <= .02 for row in scores)}
        report["passes_capture_validation"] &= report["point_regression"]["passed"]
    (root / "gain-validation.json").write_text(json.dumps(report, indent=2) + "\n")
    return report


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--trace-root", type=Path, required=True)
    parser.add_argument("--channel-map", type=Path, required=True)
    parser.add_argument("--candidate", type=Path)
    args = parser.parse_args()
    report = validate(args.trace_root, args.channel_map, args.candidate)
    print(json.dumps({"passed": report["passes_capture_validation"], "max_errors": report["max_errors"]}, indent=2))
    if not report["passes_capture_validation"]:
        raise SystemExit(1)


if __name__ == "__main__":
    main()
