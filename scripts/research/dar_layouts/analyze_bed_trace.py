#!/usr/bin/env python3
"""Validate audible bed PCM through OMO and the actual speaker OAR consumers."""

import argparse
import collections
import gzip
import hashlib
import json
import struct
from pathlib import Path

import numpy as np

from measure_gain_trace import floats, integer, regions
from measure_point_suite import decode_wav
from measure_size_field import canonical_pcm
from measure_static_bank import file_sha256, read_multichannel_adm
from run_compat_suite import validate_reference
from trace_bed_semantics import inspect_bed


def link_input_pcm(upstream_addresses, consumer_regions, count):
    regions_by_name = {part["name"]: part for part in consumer_regions}
    selection = []
    for index in range(count):
        address = regions_by_name[f"input_channel_{index}"]["address"]
        if address not in upstream_addresses:
            raise ValueError("OAR input does not alias a captured OMO output")
        selection.append(upstream_addresses[address])
    return selection


def analyze(root, channel_map):
    summary = json.loads((root / "summary.json").read_text())
    if not summary["success"] or not summary.get("pcm_comparison", {}).get("all_samples_exact"):
        raise ValueError("requires sample-identical traced/untraced output and successful state restoration")
    path = root / "trace.json"
    data = path.read_bytes() if path.exists() else gzip.decompress(path.with_suffix(".json.gz").read_bytes())
    trace = json.loads(data)
    if not trace["hook_restored"] or trace.get("size_dispatch_errors", 0):
        raise ValueError("capture did not restore its hooks or validate callback lifetime")
    adm = Path(summary["adm"])
    identity = inspect_bed(adm)
    if identity["sha256"] != summary["adm_sha256"]:
        raise ValueError("ADM changed after capture")
    run_path = Path(summary["export_run"])
    layout = summary["layout"]
    validate_reference(run_path, adm, {layout})
    run = json.loads(run_path.read_text())
    output = run["outputs"][0]
    source = read_multichannel_adm(adm, identity["channels"], identity["frames"])
    mapping = json.loads(channel_map.read_text())["layouts"][layout]
    reference = canonical_pcm(decode_wav(Path(output["path"]), output["channels"], identity["frames"]),
                              layout, mapping, False)
    labels = [item["label"] for item in sorted(mapping["interleaved_to_mono"], key=lambda item: item["mono_index"])]
    posts, oars = {}, collections.defaultdict(list)
    for record in trace["records"]:
        if record["kind"] == "omo_node":
            q = regions(record)
            count = len(q["input_events72"]) // 72
            x = np.stack([floats(q[f"input_pcm_{index}"]) for index in range(count)], axis=1)
            sample = record["sample_start"]
            if (x.shape[1] == source.shape[1] and sample + len(x) <= len(source) and np.any(x)
                    and np.array_equal(x, source[sample:sample + len(x)])):
                posts[sample] = record
        elif record["kind"] == "oar_process":
            oars[record["sample_start"]].append(record)
    checks, parameters = [], []
    for sample, record in sorted(posts.items()):
        q = regions(record)
        frames = len(q["input_pcm_0"]) // 4
        direct = np.stack([floats(q[f"direct_pcm_{index}"]) for index in range(identity["channels"])])
        branches = np.stack([floats(q[f"branches_pcm_{index}"])
                             for index in range(len(q["branches_events72"]) // 72)])
        combined = np.concatenate([direct, branches])
        # The adapter can reorder its input views (LFE first). A processor's
        # selection range describes the upstream container, not the order of
        # the post-call descriptor. Link actual PCM addresses, then verify every
        # sample; never infer bed source indices from that range alone.
        upstream_addresses = {}
        for region in record["regions"]:
            name = region["name"]
            if name.startswith("direct_pcm_"):
                upstream_addresses[region["address"]] = int(name.removeprefix("direct_pcm_"))
            elif name.startswith("branches_pcm_"):
                upstream_addresses[region["address"]] = identity["channels"] + int(name.removeprefix("branches_pcm_"))
        mixed = np.zeros((output["channels"], frames), dtype=float)
        input_error, gain_error = 0.0, 0.0
        observed_gains = {}
        consumer_bindings = []
        if not oars[sample]:
            raise ValueError("PCM-linked OMO block has no captured OAR consumer")
        for consumer in oars[sample]:
            c = regions(consumer)
            count = integer(c["input_descriptor"], fmt="Q")
            inp = np.stack([floats(c[f"input_channel_{index}"]) for index in range(count)])
            out = np.stack([floats(c[f"output_channel_{index}"]) for index in range(output["channels"])])
            if c["processor"][0]:
                begin, end = struct.unpack("<2Q", c["pcm_selection"])
                container_selection = list(range(begin, end))
            else:
                container_selection = list(struct.unpack(f"<{count}Q", c["pcm_selection"]))
            selection = link_input_pcm(upstream_addresses, consumer["regions"], count)
            consumer_bindings.append({"self": consumer["self"], "container_selection": container_selection,
                                      "post_call_pcm_source_indices": selection,
                                      "binding_basis": "identical captured PCM pointers and samples"})
            input_error = max(input_error, float(np.max(abs(inp - combined[selection]))))
            gains = np.stack([floats(c[f"gain_{index}_target"]) for index in range(count)])
            gain_error = max(gain_error, float(np.max(abs(gains.T.astype(float) @ inp - out))))
            for selected, gain in zip(selection, gains):
                if selected < 10:
                    observed_gains[selected] = gain.tolist()
            mixed += out
        checks.append({"sample_start": sample, "frames": frames, "oar_instances": len(oars[sample]),
                       "bed_direct_pcm_max_abs_error": float(np.max(abs(direct.T - source[sample:sample + frames]))),
                       "size_branch_max_abs_pcm": float(np.max(abs(branches))),
                       "omo_to_oar_max_abs_error": input_error, "constant_gain_max_abs_error": gain_error,
                       "mixed_output_max_abs_error": float(np.max(abs(mixed.T - reference[sample:sample + frames]))),
                       "consumer_bindings": consumer_bindings})
        if set(observed_gains) != set(range(10)):
            raise ValueError("did not link every audible bed input to an OAR gain vector")
        for bed in identity["bed"]:
            index = bed["input_channel"]
            event = q["input_events72"][index * 72:(index + 1) * 72]
            direct_event = q["direct_events72"][index * 72:(index + 1) * 72]
            parameters.append({"sample_start": sample, "input_channel": index,
                               "track_uid": bed["track_uid"], "source_labels": bed["blocks"][0]["labels"],
                               "event72_hex": event.hex(), "direct_event72_hex": direct_event.hex(),
                               "raw_u32_at_32": integer(event, 32),
                               "raw_u32_at_60": integer(event, 60),
                               "raw_u32_at_36": integer(event, 36),
                               "raw_size_at_52": integer(event, 52, "f"),
                               "oar_target_gains": observed_gains[index]})
    if not checks:
        raise ValueError("no PCM-linked bed blocks")
    metrics = {field: max(row[field] for row in checks) for field in checks[0]
               if field.endswith("error") or field.endswith("pcm")}
    passed = all(value <= 2e-7 for value in metrics.values())
    result = {"scope": "normal-render audible bed capture; static matrices only",
              "layout": layout, "output_labels": labels, "adm_sha256": identity["sha256"],
              "pcm_control_exact": True, "state_restored": True,
              "trace_sha256": hashlib.sha256(data).hexdigest(),
              "analyzer_sha256": file_sha256(Path(__file__)),
              "channel_map_sha256": file_sha256(channel_map), "verified_blocks": len(checks),
              "max_errors": metrics, "passes_capture_validation": passed,
              "hooks": trace["hooks"], "size_gain_calls": trace.get("size_dispatch_calls", 0),
              "checks": checks, "parameters": parameters}
    (root / "bed-validation.json").write_text(json.dumps(result, indent=2) + "\n")
    return result


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--trace-root", type=Path, required=True)
    parser.add_argument("--channel-map", type=Path, required=True)
    args = parser.parse_args()
    report = analyze(args.trace_root.resolve(), args.channel_map.resolve())
    print(json.dumps({key: report[key] for key in ("verified_blocks", "max_errors", "passes_capture_validation")}, indent=2))
    if not report["passes_capture_validation"]:
        raise SystemExit(1)


if __name__ == "__main__":
    main()
