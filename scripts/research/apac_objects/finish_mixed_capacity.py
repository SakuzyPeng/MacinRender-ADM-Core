#!/usr/bin/env python3
"""Verify mixed capacity evidence, controlled failures and lossless CAF/MP4 wrapping."""

import argparse
import json
import os
from pathlib import Path
import plistlib

from run_case import run_case
from run_mixed_capacity import CASES, LABELS, generate, verify_case
from wrap_caf import wrap


FAILURES = {
    "bed13-obj1": ("encode", "initialize", 0x21737474),
    "bed128split-obj1": ("encode", "initialize", 0x21646174),
    "bed127mono-obj1": ("encode", "cookie_info", 0x21646174),
    "bed24-obj69-default": ("encode", "initialize", 0x21737474),
    "bed65-obj1-ceiling2": ("encode", "initialize", 0x21737474),
    "bed90mono-obj1": ("decode", "decoder_initialize", 0x21646174),
    "bed1-obj70": ("decode", "decoder_produce", -50),
    "bed25split-obj69": ("decode", "decoder_produce", -50),
    "bed60split-obj68": ("decode", "decoder_produce", -50),
}
COMPONENT_COUNTS = (32, 63, 64, 126, 95, 79, 87, 91, 89, 90, 47, 55, 59, 61, 62)
CONTAINERS = ("bed24split-obj69", "bed127split-obj1", "bed253split-obj1-ceiling2",
              "bed128split-obj69-ceiling2")


def case(root, name):
    return json.loads((root / "cases" / name / "result.json").read_text())


def supplementary(root, reference):
    for key in list(os.environ):
        if key.startswith("APAC_"):
            os.environ.pop(key)
    binary = str(reference / "bin/codec_probe")
    rows = []
    for count in COMPONENT_COUNTS:
        name = f"component-count-{count}-native"
        source, output = root / "inputs" / name, root / "cases" / name
        if not (output / "result.json").exists():
            expected = generate(source, [1] * count, [1])
            run_case([binary, "encode", str(source / "settings.plist"), str(source / "input.f32"),
                      str(expected["input_channels"]), str(expected["audio_channels"]), str(output / "stream"),
                      "0", str(expected["total_bitrate"])], output, 30)
        result = case(root, name)
        row = {"case": name, "bed_components": count, "objects": 1, "returncode": result["returncode"],
               "timed_out": result["timed_out"], "last_event": result["events"][-1:]}
        if result["returncode"] == 0:
            decoded = root / "cases" / (name + "-decode")
            if not (decoded / "result.json").exists():
                run_case([binary, "decode", str(output / "stream"), str(decoded / "decoded.f32"), str(count + 1)], decoded, 30)
            observed = case(root, decoded.name)
            row.update(decode_returncode=observed["returncode"], decode_timed_out=observed["timed_out"],
                       decode_last_event=observed["events"][-1:])
        rows.append(row)
    (root / "component_counts.json").write_text(json.dumps(rows, indent=2) + "\n")

    # This controlled counterexample is rejected at set_acs, before PCM is read.
    name = "channel-map-index255"
    source, output = root / "inputs" / name, root / "cases" / name
    if not (output / "result.json").exists():
        base = root / "inputs/bed253split-obj1-ceiling2"
        config = plistlib.loads((base / "settings.plist").read_bytes())
        full_band = [pair for pair in LABELS if pair[1] not in (4, 37)]
        for parameter in config["parameters"][:2]:
            components = parameter["ASComponents"]
            bed = components[3]["Channel Bed"]
            bed[0]["current value"].append("kAudioChannelLabel_" + full_band[61 % len(full_band)][0])
            bed[1]["current maximum range"] += 1
            for field in bed:
                if field["key"] == "Bit Rate":
                    field["current value"] += 256000
            components[4]["Object"][1].update({"current minimum range": 254, "current maximum range": 254})
        config["parameters"][2]["Metadata"][0].update({"current minimum range": 255, "current maximum range": 255})
        source.mkdir(parents=True, exist_ok=False)
        (source / "settings.plist").write_bytes(plistlib.dumps(config))
        (source / "expected.json").write_text(json.dumps({"audio_channels": 255, "metadata_channels": 1,
            "input_channels": 256, "expected_failure_stage": "set_acs", "metadata_channel_index": 255}, indent=2) + "\n")
        run_case([binary, "encode", str(source / "settings.plist"), str(base / "input.f32"), "256", "255",
                  str(output / "stream"), "0", str(256000 * 255)], output, 30)

    (root / "fixtures").mkdir(exist_ok=True)
    containers = []
    for name in CONTAINERS:
        stream = root / "cases" / (name + "-encode") / "stream"
        caf = root / "fixtures" / (name + ".caf")
        mp4 = caf.with_suffix(".mp4")
        if not caf.exists():
            wrap(stream, caf)
        wrap_case = name + "-mp4-wrap"
        if not (root / "cases" / wrap_case / "result.json").exists():
            run_case(["afconvert", str(caf), str(mp4), "-f", "mp4f", "-d", "0"], root / "cases" / wrap_case, 30)
        assert case(root, wrap_case)["returncode"] == 0
        for file in (caf, mp4):
            output = root / "cases" / (name + "-" + file.suffix[1:] + "-import")
            if not (output / "result.json").exists():
                run_case([binary, "import", str(file), str(output / "stream")], output, 30)
            result = case(root, output.name)
            containers.append({"case": name, "container": file.suffix[1:], "returncode": result["returncode"],
                "compressed_packets_identical": Path(str(stream) + ".packets").read_bytes() == (output / "stream.packets").read_bytes(),
                "magic_cookie_identical": Path(str(stream) + ".cookie").read_bytes() == (output / "stream.cookie").read_bytes(),
                "timing_matches": json.loads(Path(str(stream) + ".timing.json").read_text()) == json.loads((output / "stream.timing.json").read_text())})
    (root / "containers.json").write_text(json.dumps(containers, indent=2) + "\n")


def verify(root):
    rows = json.loads((root / "capacity.json").read_text())
    assert len(rows) == len(CASES) and {row["case"] for row in rows} == {item[0] for item in CASES}
    positives, negatives = [], []
    for row in rows:
        name = row["case"]
        encoded = case(root, name + "-encode")
        assert not encoded["timed_out"]
        ceiling = encoded["research_environment"].get("APAC_PROFILE_CEILING")
        assert ceiling == (None if row["diagnostic_ceiling"] is None else str(row["diagnostic_ceiling"]))
        if row["profile"]:
            assert (row["profile"]["profile"], row["profile"]["level"]) == (5, row["diagnostic_ceiling"] or 0)
        if name in FAILURES:
            phase, stage, code = FAILURES[name]
            result = case(root, name + "-" + phase)
            assert result["returncode"] != 0 and not result["timed_out"]
            assert result["events"][-1] == {"stage": stage, "status": code}
            if phase == "decode":
                if stage == "decoder_produce":
                    assert row["metadata_capacity"]["required_bytes"] > row["metadata_capacity"]["capacity_bytes"] == 4096
                else:
                    assert row["metadata_capacity"] is None
                assert not any(key in result["research_environment"] for key in (
                    "APAC_PROFILE_CEILING", "APAC_DECODER_METADATA", "APAC_BINARY_METADATA"))
            negatives.append({"case": name, "phase": phase, "stage": stage, "status": code})
        else:
            checked = verify_case(root, name)
            assert checked == row["verification"]
            assert row["metadata_capacity"]["required_bytes"] <= row["metadata_capacity"]["capacity_bytes"] == 4096
            positives.append({"case": name, "bed_channels": row["bed_channels"], "objects": row["objects"],
                              "diagnostic_ceiling": row["diagnostic_ceiling"],
                              **{key: value for key, value in checked.items() if key != "channels"}})
    counts = json.loads((root / "component_counts.json").read_text())
    assert {row["bed_components"] for row in counts} == set(COMPONENT_COUNTS)
    for row in counts:
        result = case(root, row["case"])
        assert not result["timed_out"] and not result["research_environment"]
        if row["bed_components"] <= 90:
            assert result["returncode"] == 0 and result["events"][-1]["stage"] == "encoded"
            decoded = case(root, row["case"] + "-decode")
            assert not decoded["timed_out"] and not decoded["research_environment"]
            if row["bed_components"] <= 62:
                assert decoded["returncode"] == 0
                assert decoded["events"][-1]["frames"] == 10240
            else:
                assert decoded["returncode"] != 0
                assert decoded["events"][-1] == {"stage": "decoder_initialize", "status": 0x21646174}
        else:
            assert result["returncode"] != 0
            assert result["events"][-1] == {"stage": "cookie_info", "status": 0x21646174}
    control = case(root, "channel-map-index255")
    assert control["returncode"] != 0 and not control["timed_out"]
    assert control["events"][-1] == {"stage": "set_acs", "status": 0x21646174}
    containers = json.loads((root / "containers.json").read_text())
    assert len(containers) == len(CONTAINERS) * 2
    assert all(row["returncode"] == 0 and row["compressed_packets_identical"] and row["magic_cookie_identical"]
               and row["timing_matches"] for row in containers)
    return {"all_checks_passed": True, "decoder_policy": "system default, read-only metadata observation",
            "scope": "this component version, ASC dictionary and independently updated object metadata",
            "verified_cases": positives, "expected_failures": negatives, "component_count_encoding": counts,
            "channel_map_index255_rejected": True, "containers": containers,
            "not_validated": ["universal APAC format limits", "a physical layout for repeated BED labels",
                              "system player spatial rendering", "all BED component organizations"]}


if __name__ == "__main__":
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--output", required=True, type=Path)
    parser.add_argument("--reference", type=Path, help="omit to verify existing evidence without running tools")
    args = parser.parse_args()
    root = args.output.resolve()
    if args.reference:
        supplementary(root, args.reference.resolve())
    result = verify(root)
    (root / "verification.json").write_text(json.dumps(result, indent=2) + "\n")
    print(json.dumps({"all_checks_passed": result["all_checks_passed"], "positive_cases": len(result["verified_cases"]),
                      "negative_cases": len(result["expected_failures"]), "containers": len(result["containers"])}))
