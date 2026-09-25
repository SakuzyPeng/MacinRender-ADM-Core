#!/usr/bin/env python3
"""Finish reproducible APAC bitrate comparisons and validate the stored evidence."""

import argparse
import hashlib
import json
import os
from pathlib import Path
import shlex

import numpy as np

from make_inputs import generate as generate_objects
from make_mixed_inputs import generate as generate_mixed
from run_bitrates import run
from run_case import run_case
from run_mixed_capacity import generate as generate_beds
from wrap_caf import wrap


def rows(root):
    return json.loads((root / "bitrates.json").read_text())


def extras(root):
    completed = {row["case"] for row in rows(root)}

    def once(name, *arguments):
        if name not in completed:
            run(root, name, *arguments)
            completed.add(name)

    noise = root / "inputs/noise-object"
    if not noise.exists():
        expected = generate_objects(noise, frames=1152000)
        pcm = np.fromfile(noise / "input.f32", dtype="<f4").reshape(-1, 2)
        pcm[:, 0] = .1 * np.random.default_rng(20260923).standard_normal(len(pcm))
        pcm.tofile(noise / "input.f32")
        expected.update(signal="deterministic broadband white noise, 24 seconds", noise_seed=20260923)
        (noise / "expected.json").write_text(json.dumps(expected, indent=2) + "\n")
    for global_rate, explicit, mode in [(32000, None, 1), (128000, None, 1), (256000, None, 1),
        (128000, [64000], 1), (128000, [128000], 1), (128000, [256000], 1),
        (128000, [128000], 0), (128000, [128000], 2), (128000, [128000], 3)]:
        name = f"noise-global{global_rate}-component{explicit[0] if explicit else 0}-mode{mode}"
        once(name, noise, 1, global_rate, explicit, mode)
    for rate in (100, 500, 1000, 2000, 3000):
        once(f"lfe-{rate}", root / "inputs/lfe-object", 2, 256000 + rate, [rate, 256000])
        once(f"object-{rate}", root / "inputs/object", 1, 256000, [rate])
    for rate in (1, 64000, 1000000):
        once(f"mixed-fixed-global-{rate}", root / "inputs/lfe-object", 2, rate, [16000, 256000])
    source = root / "inputs/objects2"
    if not source.exists():
        generate_objects(source, objects=2, trajectory="spread", frames=48000)
    for budget in (128000, 256000):
        once(f"objects2-component-{budget}", source, 2, budget, [budget])
    source = root / "inputs/lfe-objects7"
    if not source.exists():
        generate_mixed(source, objects=7, trajectory="spread", bed_kind="lfe_only", explicit_bitrates=True)
    once("mixed-7obj-lfe-256k", source, 8, 1808000, [16000, 1792000])
    source = root / "inputs/bed12-object"
    if not source.exists():
        generate_beds(source, [12], [1])
    once("bed12-object-128k", source, 13, 1552000, [1424000, 128000])


def observe(root):
    for key in list(os.environ):
        if key.startswith("APAC_"):
            os.environ.pop(key)
    scripts = Path(__file__).resolve().parent
    binary = root / "bin/bitrate_probe"

    def debug(script):
        return ["xcrun", "lldb", "--no-lldbinit", "--batch", "-o",
                "command script import " + shlex.quote(str(scripts / script)), "-o", "run", "--"]

    allocation = []
    for name in ("global-256000", "object-256000", "lfe-0", "lfe-16000"):
        row = next(item for item in rows(root) if item["case"] == name)
        source = Path(row["source"])
        expected = json.loads((source / "expected.json").read_text())
        channels = expected.get("input_channels", expected["objects"] + expected.get("metadata_channels", 1))
        output = root / "cases" / ("allocation-" + name)
        if not (output / "result.json").exists():
            run_case(debug("trace_bitrate.py") + [str(binary), "encode-rate", "-1", "-1",
                     str(root / "settings" / name / "settings.plist"), str(source / "input.f32"), str(channels),
                     str(row["audio_channels"]), str(output / "stream"), "0", str(row["requested_global_bps"])], output, 60)
        result = json.loads((output / "result.json").read_text())
        allocation.append({"case": name, "returncode": result["returncode"], "timed_out": result["timed_out"],
                           "observations": [event for event in result["events"] if event["stage"].startswith("component_rate")]})
    (root / "allocation.json").write_text(json.dumps(allocation, indent=2) + "\n")
    for name in ("object-6000", "mixed-7obj-lfe-256k"):
        row = next(item for item in rows(root) if item["case"] == name)
        output = root / "cases" / ("positions-" + name)
        if not (output / "result.json").exists():
            run_case(debug("trace_codec.py") + [str(binary), "decode", str(root / "cases" / (name + "-encode") / "stream"),
                     str(output / "decoded.f32"), str(row["audio_channels"])], output, 60)

    name = "mixed-7obj-lfe-256k"
    source = root / "cases" / (name + "-encode") / "stream"
    (root / "fixtures").mkdir(exist_ok=True)
    caf = root / "fixtures" / (name + ".caf")
    mp4 = caf.with_suffix(".mp4")
    if not caf.exists():
        wrap(source, caf)
    if not (root / "cases/mp4-wrap/result.json").exists():
        run_case(["afconvert", str(caf), str(mp4), "-f", "mp4f", "-d", "0"], root / "cases/mp4-wrap", 30)
    comparisons = []
    for file in (caf, mp4):
        output = root / "cases" / (file.suffix[1:] + "-import")
        if not (output / "result.json").exists():
            run_case([str(binary), "import", str(file), str(output / "stream")], output, 30)
        imported = json.loads((output / "result.json").read_text())
        comparisons.append({"container": file.suffix[1:], "returncode": imported["returncode"],
                            **{suffix: Path(str(source) + suffix).read_bytes() == (output / ("stream" + suffix)).read_bytes()
                               for suffix in (".packets", ".cookie", ".timing.json")}})
    (root / "containers.json").write_text(json.dumps(comparisons, indent=2) + "\n")


def verify(root):
    tested = rows(root)
    assert len(tested) == 86 and len({row["case"] for row in tested}) == 86
    timeouts = {"object-2000000", "object-4294967295"}
    produce_errors = {f"object-{rate}" for rate in (1, 100, 500, 1000, 2000, 3000)}
    produce_errors.update(f"lfe-{rate}" for rate in (0, 1, 100, 500, 1000))
    positive, failures = [], []
    for row in tested:
        name = row["case"]
        if name in timeouts:
            assert row["encode_timed_out"] and row["encode_returncode"] != 0
            assert row["encode_last_event"] == [{"stage": "append", "status": 0}]
            failures.append({"case": name, "failure": "30-second timeout during first output production"})
            continue
        assert not row["encode_timed_out"]
        if name in produce_errors or name == "mode-4":
            stage, code = ("initialize", 0x21646174) if name == "mode-4" else ("produce", -50)
            assert row["encode_returncode"] != 0 and row["encode_last_event"] == [{"stage": stage, "status": code}]
            failures.append({"case": name, "failure": stage, "status": code})
            continue
        assert row["encode_returncode"] == row["decode_returncode"] == 0 and not row["decode_timed_out"]
        checked = row["audio_check"]
        assert checked["complete_frames"] and checked["finite"] and checked["valid_frames"] == row["valid_frames"]
        assert checked["non_silent_channels"] == row["audio_channels"]
        decoded = json.loads((root / "cases" / (name + "-decode") / "result.json").read_text())
        assert not decoded["research_environment"]
        if not name.startswith("noise-"):
            expected = json.loads((Path(row["source"]) / "expected.json").read_text())
            frequencies = expected.get("audio_frequencies", [60] + expected.get("object_frequencies", []))
            assert np.allclose(checked["peak_hz_per_channel"], frequencies, atol=1e-6)
        payload = root / "cases" / (name + "-encode") / "stream.packets"
        assert hashlib.sha256(payload.read_bytes()).hexdigest() == row["payload_sha256"]
        positive.append(name)
    allocation = json.loads((root / "allocation.json").read_text())
    expected_allocations = {"global-256000": [(0, 96000)], "object-256000": [(256000, 256000)],
                            "lfe-0": [(0, 0), (256000, 256000)], "lfe-16000": [(16000, 16000), (256000, 256000)]}
    for row in allocation:
        assert not row["timed_out"]
        pairs, before = [], None
        for event in row["observations"]:
            if event["stage"] == "component_rate_before_selection":
                before = event["bitrate_bps"]
            elif before is not None:
                pairs.append((before, event["bitrate_bps"]))
                before = None
        assert pairs == expected_allocations[row["case"]]
    positions = []
    for name in ("object-6000", "mixed-7obj-lfe-256k"):
        row = next(item for item in tested if item["case"] == name)
        expected = json.loads((Path(row["source"]) / "expected.json").read_text())
        decoded = json.loads((root / "cases" / ("positions-" + name) / "result.json").read_text())
        assert decoded["returncode"] == 0 and not decoded["timed_out"] and not decoded["research_environment"]
        timing = json.loads((root / "cases" / (name + "-encode") / "stream.timing.json").read_text())
        bed_groups = int("bed_channels" in expected)
        wanted = {(entry["sample"], index): azimuth for entry in expected["timeline"] for index, azimuth in enumerate(entry["azimuths"])}
        seen, errors = {}, []
        for event in decoded["events"]:
            if "derived_spherical" not in event or event.get("decode_metadata_index") is None:
                continue
            group = event.get("object_index")
            sample = event["g_probe_pcm_output_frames"] - timing["leading_frames"]
            if group is None or group < bed_groups or not 0 <= sample < expected["frames"]:
                continue
            key = (sample, group - bed_groups)
            assert event["group_id"] == group
            azimuth, elevation, distance = event["derived_spherical"]
            assert elevation == 0 and distance == 1
            error = abs(azimuth - wanted[key])
            assert error <= 180 / (1 << event["precision_bits"][0]) + 1e-5
            seen[key] = azimuth
            errors.append(error)
        assert set(seen) == set(wanted)
        positions.append({"case": name, "position_rows": len(seen), "maximum_azimuth_error_degrees": max(errors)})
    containers = json.loads((root / "containers.json").read_text())
    assert len(containers) == 2 and all(row["returncode"] == 0 and all(row[s] for s in (".packets", ".cookie", ".timing.json")) for row in containers)
    return {"all_checks_passed": True, "default_decoder": True, "encoder_profile_table_modified": False,
            "successful_cases": positive, "expected_failures": failures, "positions": positions, "containers": containers,
            "not_claimed": ["universal bitrate limits", "music quality recommendations", "lossless encoding", "strict container bandwidth ceiling"]}


if __name__ == "__main__":
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--output", type=Path, required=True)
    parser.add_argument("--verify-only", action="store_true")
    args = parser.parse_args()
    root = args.output.resolve()
    if not args.verify_only:
        extras(root)
        observe(root)
    checked = verify(root)
    (root / "verification.json").write_text(json.dumps(checked, indent=2) + "\n")
    print(json.dumps({"all_checks_passed": True, "successful_cases": len(checked["successful_cases"]),
                      "expected_failures": len(checked["expected_failures"]), "positions": checked["positions"]}))
