#!/usr/bin/env python3
"""Check mixed APAC bed/object samples using native decoder outputs only."""

import argparse
import json
from pathlib import Path

import numpy as np


def check(root, name):
    expected = json.loads((root / "inputs" / name / "expected.json").read_text())
    encoded = json.loads((root / "cases" / (name + "-encode") / "result.json").read_text())
    decoded = json.loads((root / "cases" / (name + "-decode") / "result.json").read_text())
    assert encoded["returncode"] == decoded["returncode"] == 0
    assert not encoded["timed_out"] and not decoded["timed_out"]
    for result in (encoded, decoded):
        assert "APAC_PROFILE_CEILING" not in result["research_environment"]
        assert "APAC_DECODER_METADATA" not in result["research_environment"]
        profile = next(event for event in result["events"] if "profile" in event)
        assert (profile["profile"], profile["level"]) == (5, 0)
    layout = next(event for event in decoded["events"] if event["stage"] == "decoder_input_layout_value")
    labels = [channel["label"] for channel in layout["descriptions"]]
    has_lfe = expected["lfe_channel"] is not None
    bed_channels = expected["bed_channels"]
    assert labels[:bed_channels] == ([4] if bed_channels == 1 else [3, 4 if has_lfe else 2]), labels
    assert len(labels) == expected["audio_channels"]
    assert all(label >= 262144 for label in labels[bed_channels:]), labels
    encoded_lfe = any("::WriteLFE(" in event.get("function", "") for event in encoded["events"])
    decoded_lfe = any("APACLFEElement::Deserialize(" in event.get("function", "") for event in decoded["events"])
    assert encoded_lfe == decoded_lfe == has_lfe
    timing = json.loads((root / "cases" / (name + "-encode") / "stream.timing.json").read_text())
    audio = np.fromfile(root / "cases" / (name + "-decode") / "decoded.f32", dtype="<f4").reshape(-1, len(labels))
    assert len(audio) == sum(timing.values())
    first = timing["leading_frames"]
    audio = audio[first:first + timing["valid_frames"]].astype("float64")
    assert len(audio) == expected["frames"] and np.isfinite(audio).all()
    spectrum = np.abs(np.fft.rfft(audio, axis=0)) ** 2
    fractions = []
    for index, frequency in enumerate(expected["object_frequencies"]):
        channel = expected["bed_channels"] + index
        frequency_bin = round(frequency * len(audio) / 48000)
        assert np.argmax(spectrum[:, channel]) == frequency_bin
        fractions.append(float(spectrum[frequency_bin, channel] / spectrum[:, channel].sum()))
    assert min(fractions) > .95
    positions = {}
    for event in decoded["events"]:
        if "derived_spherical" not in event or event.get("decode_metadata_index") is None:
            continue
        group = event.get("object_index")
        if group is None or group < 1:
            continue
        sample = event["g_probe_pcm_output_frames"] - first
        if 0 <= sample < len(audio):
            positions[sample, group - 1] = event
    wanted = {row["sample"]: row["azimuths"] for row in expected["timeline"]}
    assert set(positions) == {(sample, index) for sample in wanted for index in range(expected["objects"])}
    errors = []
    for (sample, index), event in positions.items():
        azimuth, elevation, distance = event["derived_spherical"]
        assert event["group_id"] == index + 1 and elevation == 0 and distance == 1
        error = abs(azimuth - wanted[sample][index])
        assert error <= 180 / (1 << event["precision_bits"][0]) + 1e-5
        errors.append(error)
    stable = audio[9600:38400]
    amplitudes = np.abs(np.fft.rfft(stable, axis=0)) * 2 / len(stable)
    probe = expected["lowpass_probe_channel"]
    low = float(amplitudes[36, probe])  # 60 Hz in a 0.6-second analysis window
    high = float(amplitudes[600, probe])  # 1 kHz
    assert .045 < low < .055
    if has_lfe:
        assert high < .00005
    else:
        assert .045 < high < .055
    return {"case": name, "objects": expected["objects"], "audio_channels": len(labels), "valid_frames": len(audio),
            "native_layout_labels": labels, "dedicated_lfe_codec_observed": has_lfe,
            "object_position_rows": len(positions), "max_azimuth_error_degrees": max(errors),
            "minimum_object_tone_energy_fraction": min(fractions), "probe_60_hz_amplitude": low,
            "probe_1000_hz_amplitude": high, "probe_60_hz_gain_db": float(20 * np.log10(low / .05)),
            "probe_1000_hz_gain_db": float(20 * np.log10(max(high, 1e-30) / .05)), "passed": True}


def verify(root):
    names = ("mixed-left", "mixed-right", "mixed-moving", "mixed-control", "mixed-24ch",
             "mixed-lfe-explicit", "mixed-lfe-moving-explicit", "mixed-23obj-lfe")
    rows = [check(root, name) for name in names]
    negative = json.loads((root / "cases/mixed-lfe-only-encode/result.json").read_text())
    assert negative["returncode"] != 0 and not negative["timed_out"]
    assert negative["events"][-1] == {"stage": "produce", "status": -50}
    control = json.loads((root / "cases/lfe-only-total-rate-control/result.json").read_text())
    assert control["returncode"] != 0 and control["events"][-1] == {"stage": "produce", "status": -50}
    containers = json.loads((root / "containers.json").read_text())
    assert len(containers) == 12
    assert all(row["returncode"] == 0 and row["compressed_packets_identical"] and row["magic_cookie_identical"] and row["timing_matches"] for row in containers)
    return {"all_checks_passed": True, "encoder_policy": "default profile ceilings; private ASC settings",
            "decoder_policy": "system default capabilities and configuration", "verified_cases": rows, "containers": containers,
            "lfe_only_component": "auto allocation failed; explicit per-component bitrates are verified separately",
            "not_validated": ["all possible LFE component bitrate choices", "player spatial rendering and LFE calibration",
                              "a user-confirmed 17.1.6 coordinate map"]}


if __name__ == "__main__":
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--output", required=True, type=Path)
    args = parser.parse_args()
    result = verify(args.output)
    (args.output / "verification.json").write_text(json.dumps(result, indent=2) + "\n")
    print(json.dumps(result, indent=2))
