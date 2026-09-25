#!/usr/bin/env python3
"""Check native object experiments against independent source expectations."""

import argparse
import json
from pathlib import Path

import numpy as np


def inspect(root, name, fixture, count):
    directory = root / "derived" / name
    expected = json.loads((root / "inputs" / fixture / "expected.json").read_text())
    actual = json.loads((directory / "positions.json").read_text())
    rows = actual["positions"]
    wanted = {item["sample"]: item["azimuths"] for item in expected["timeline"]}
    assert {(row["sample"], row["object_index"]) for row in rows} == {
        (sample, index) for sample in wanted for index in range(count)
    }, (name, "position timeline coverage")
    errors = []
    for row in rows:
        index = row["object_index"]
        assert row["group_id"] == index, (name, "group ID mismatch")
        azimuth, elevation, distance = row["position"]
        error = abs(azimuth - wanted[row["sample"]][index])
        tolerance = 180 / (1 << row["precision_bits"][0])
        assert error <= tolerance + 1e-5, (name, "position quantization", row, error)
        assert elevation == 0 and distance == 1, (name, "elevation/distance")
        errors.append(error)
    pcm = np.fromfile(directory / "audio.f32", dtype="<f4").reshape(-1, count).astype("float64")
    assert len(pcm) == expected["frames"] and np.isfinite(pcm).all(), (name, "PCM length/values")
    spectrum = np.abs(np.fft.rfft(pcm, axis=0)) ** 2
    frequency_bins = [round(frequency * len(pcm) / expected["sample_rate"]) for frequency in expected["audio_frequencies"]]
    fractions = [float(spectrum[bin_index, i] / spectrum[:, i].sum()) for i, bin_index in enumerate(frequency_bins)]
    assert np.argmax(spectrum, axis=0).tolist() == frequency_bins, (name, "audio channel mapping")
    assert min(fractions) >= .95, (name, "unexpected audio energy")
    return {"case": name, "objects": count, "valid_frames": len(pcm), "position_rows": len(rows),
            "max_azimuth_error_degrees": max(errors), "minimum_expected_tone_energy_fraction": min(fractions),
            "per_object_rms": np.sqrt(np.mean(pcm * pcm, axis=0)).tolist(), "passed": True}


def verify(root):
    rows = [inspect(root, name, name, 1) for name in ("left1", "right1", "moving1")]
    matrix = json.loads((root / "matrix.json").read_text())
    assert len(matrix) == 12, "incomplete matrix"
    for case in matrix:
        if case["diagnostic_ceiling"] is None and case["objects"] > 7:
            events = json.loads((root / "cases" / case["case"] / "result.json").read_text())["events"]
            assert case["encode_returncode"] != 0 and not case["encode_timed_out"]
            assert events[-1] == {"stage": "initialize", "status": 561214580}
            rows.append({"case": case["case"], "expected_rejection": "!stt at initialization", "passed": True})
        else:
            assert case["encode_returncode"] == 0 and case["decode_returncode"] == 0, case
            assert not case["encode_timed_out"] and not case["decode_timed_out"], case
            actual_profile = case["profile"]
            assert (actual_profile["profile"], actual_profile["level"]) == (5, 0 if case["objects"] <= 7 else 1)
            decoded = json.loads((root / "cases" / (case["case"] + "-decode") / "result.json").read_text())
            assert "APAC_PROFILE_CEILING" not in decoded["research_environment"], "decoder was modified"
            rows.append(inspect(root, case["case"], f"matrix-{case['objects']}", case["objects"]))
    rendering = json.loads((root / "single_object_summary.json").read_text())
    signs = {"left1": (1, 1), "right1": (-1, -1), "moving1": (1, -1)}
    for case in rendering:
        for window, sign in zip(("early", "late"), signs[case["case"]]):
            assert case["windows"][window]["left_minus_right_db"] * sign > 1, (case, "render direction")
    negatives = []
    for name in ("missing-metadata", "empty-metadata"):
        decoded = json.loads((root / "cases" / ("negative-" + name + "-decode") / "result.json").read_text())
        assert decoded["returncode"] == 0 and not decoded["timed_out"]
        positions = [event for event in decoded["events"] if "derived_spherical" in event and event.get("decode_metadata_index") is not None]
        if name == "missing-metadata":
            assert not positions
        else:
            assert positions and all(event["derived_spherical"] == [-180.0, -90.0, 0.0] for event in positions)
        negatives.append({"case": name, "encoder_can_produce_audio": True,
                          "required_object_position_validation": "rejected", "passed": True})
    invalid = json.loads((root / "cases/negative-wrong-object-range/result.json").read_text())
    assert invalid["returncode"] != 0 and invalid["events"][-1] == {"stage": "set_acs", "status": 560226676}
    containers = json.loads((root / "containers.json").read_text())
    assert len(containers) == 12 and all(case["compressed_packets_identical"] for case in containers)
    for case in containers:
        moving = case["case"] == "moving1"
        assert case["timing"] == {"valid_frames": 144000 if moving else 48000,
                                   "leading_frames": 2048, "trailing_frames": 384 if moving else 128}, case
    assets = json.loads((root / "asset_reads.json").read_text())
    assert len(assets) == 12
    for asset in assets:
        name = Path(asset["file"]).stem
        channels = int(name.split("-")[1]) if name.startswith("matrix-") else 1
        frames = 144000 if name == "moving1" else 48000
        assert asset["returncode"] == 0
        event = asset["events"][-1]
        assert event["status"] == 2 and event["channels"] == channels and event["frames"] == frames, asset
        assert event["values"] == frames * channels and event["rms"] > .001, asset
    supplements = []
    custom_path = root / "cases/custom-ancillary-control/result.json"
    if custom_path.exists():
        custom = json.loads(custom_path.read_text())
        assert custom["returncode"] != 0 and not custom["timed_out"]
        assert custom["events"][-1]["stage"] == "encoded" and custom["events"][-1]["packets"] == 0
        assert any("APACCustomModeEncoder::EncodeFrame" in event.get("function", "") for event in custom["events"])
        supplements.append({"case": "custom-ancillary-control", "no_audio_packets": True, "passed": True})
    binary_path = root / "binary_reader_summary.json"
    if binary_path.exists():
        binary = json.loads(binary_path.read_text())
        assert binary["returncode"] == 0 and binary["reader_observed"] and not binary["profile_table_modified"]
        supplements.append({"case": "binary-reader-mdpf-trace", "reader_selected": True,
                            "binary_payload_roundtrip_validated": False, "passed": True})
    return {"all_checks_passed": True, "audio_and_positions": rows, "rendering": rendering,
            "negative_cases": negatives + [{"case": "wrong-object-range", "rejected_at": "acs", "passed": True}],
            "containers": containers, "asset_reads": assets, "supplementary_checks": supplements}


if __name__ == "__main__":
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--output", type=Path, required=True)
    args = parser.parse_args()
    result = verify(args.output)
    (args.output / "verification.json").write_text(json.dumps(result, indent=2) + "\n")
    print(json.dumps({"all_checks_passed": True, "audio_and_position_cases": len(result["audio_and_positions"])}))
