#!/usr/bin/env python3
"""Measure per-component and combined BED/object capacity without changing the decoder."""

import argparse
import copy
import hashlib
import json
import os
from pathlib import Path
import plistlib
import shlex
import struct

import numpy as np

from make_inputs import Bits, position
from run_case import run_case
from run_experiments import environment


# A capacity fixture, not a proposed loudspeaker layout. Names may repeat across beds.
LABELS = [
    ("Left", 1), ("Right", 2), ("Center", 3), ("LFEScreen", 4),
    ("LeftSurround", 5), ("RightSurround", 6), ("RearSurroundLeft", 33),
    ("RearSurroundRight", 34), ("VerticalHeightLeft", 13),
    ("VerticalHeightRight", 15), ("LeftTopRear", 52), ("RightTopRear", 54),
    ("LeftWide", 35), ("RightWide", 36), ("LeftCenter", 7), ("RightCenter", 8),
    ("CenterSurround", 9), ("TopCenterSurround", 12), ("VerticalHeightCenter", 14),
    ("TopBackCenter", 17), ("LeftTopMiddle", 49), ("RightTopMiddle", 51),
    ("CenterTopRear", 53), ("LFE2", 37),
]

CASES = [
    ("bed12-obj1", [12], [1], None),
    ("bed13-obj1", [13], [1], None),
    ("bed24split-obj1", [12, 12], [1], None),
    ("bed1-obj69", [1], [7] * 9 + [6], None),
    ("bed1-obj70", [1], [7] * 10, None),
    ("bed12-obj69", [12], [7] * 9 + [6], None),
    ("bed127split-obj1", [12] * 10 + [7], [1], None),
    ("bed128split-obj1", [12] * 10 + [8], [1], None),
    ("bed24split-obj69", [12, 12], [7] * 9 + [6], None),
    ("bed25split-obj69", [12, 12, 1], [7] * 9 + [6], None),
    ("bed61split-obj67", [12] * 5 + [1], [7] * 9 + [4], None),
    ("bed60split-obj68", [12] * 5, [7] * 9 + [5], None),
    ("bed127mono-obj1", [1] * 127, [1], None),
    ("bed24-obj69-default", [24], [7] * 9 + [6], None),
    ("bed24-obj69-ceiling1", [24], [32, 32, 5], 1),
    ("bed64-obj69-ceiling2", [64], [32, 32, 5], 2),
    ("bed65-obj1-ceiling2", [65], [1], 2),
    ("bed253split-obj1-ceiling2", [64, 64, 64, 61], [1], 2),
    ("bed90mono-obj1", [1] * 90, [1], None),
    ("bed128split-obj69-ceiling2", [64, 64], [32, 32, 5], 2),
    ("bed62mono-obj1", [1] * 62, [1], None),
]


def metadata(beds, azimuths):
    data = Bits()
    data.put(beds + len(azimuths), 11)
    for _ in range(beds):
        data.put(0, 1)
    for index, azimuth in enumerate(azimuths, beds):
        data.put(1, 1)
        data.put(index, 11)
        data.put(0, 3)
        data.put(0, 1)
        data.put(0, 1)
        data.put(1, 11)
        data.put(0, 11)
        position(data, azimuth)
    payload = data.finish()
    body = b"\x00\x01apdd" + struct.pack(">H", len(payload)) + payload
    return b"\xff\xff" + struct.pack(">H", len(body) + 7) + b"\x01\x04\x00" + body


def generate(output, bed_sizes, object_groups, frames=8192):
    if not bed_sizes or min(bed_sizes) < 1 or max(bed_sizes) > 65:
        raise ValueError("each BED fixture supports 1..65 channels, including the Level 2 negative control")
    if not object_groups or min(object_groups) < 1:
        raise ValueError("a mixed case needs at least one object")
    if frames % 1024:
        raise ValueError("use whole metadata frames")
    objects, bed_channels = sum(object_groups), sum(bed_sizes)
    audio_channels = bed_channels + objects
    components, labels, rates = [], [], []
    offset = 0
    for size in bed_sizes:
        # Beyond 24 this is deliberately a repeated-label capacity stress test,
        # not a claim that 64 distinct physical speaker positions are defined.
        full_band = [pair for pair in LABELS if pair[1] not in (4, 37)]
        bed = LABELS[:size] if size <= 24 else [full_band[i % len(full_band)] for i in range(size)]
        labels.extend(label for _, label in bed)
        components.append({"key": "Channel Bed", "Channel Bed": [
            {"key": "ChannelLayoutLabel", "current value": ["kAudioChannelLabel_" + name for name, _ in bed]},
            {"key": "Channel Map", "current minimum range": offset, "current maximum range": offset + size - 1},
        ]})
        rates.append(sum(16000 if label in (4, 37) else 256000 for _, label in bed))
        offset += size
    for size in object_groups:
        components.append({"key": "Object", "Object": [
            {"key": "Object Count", "current value": size},
            {"key": "Channel Map", "current minimum range": offset, "current maximum range": offset + size - 1},
        ]})
        rates.append(size * 256000)
        offset += size
    azimuths = [-150 + 300 * (i + .5) / objects for i in range(objects)]
    blob = metadata(len(bed_sizes), azimuths)
    metadata_channels = (len(blob) + 2047) // 2048
    if audio_channels + metadata_channels > 255:
        raise ValueError("channel map would exceed tested dictionary range")
    codec = copy.deepcopy(components)
    for component, rate in zip(codec, rates):
        component[component["key"]].append({"key": "Bit Rate", "current value": rate})
    settings = {"version": 1, "sub version": 2, "parameters": [
        {"key": "Audio Scene Components", "ASComponents": components},
        {"key": "Codec Configurations", "ASComponents": codec},
        {"key": "APAC Metadata", "Metadata": [{"key": "Channel Map", "current minimum range": audio_channels,
                                                "current maximum range": audio_channels + metadata_channels - 1}]},
    ]}
    pcm = np.zeros((frames, audio_channels + metadata_channels), dtype="<f4")
    frequencies, lfe_index = [], 0
    for channel in range(audio_channels):
        if channel < bed_channels and labels[channel] in (4, 37):
            # Unique integer bins below the LFE passband, even with many BEDs.
            frequency_bin = 6 + lfe_index
            lfe_index += 1
        else:
            frequency_bin = 80 + channel * 7
        frequency = frequency_bin * 48000 / frames
        frequencies.append(frequency)
        pcm[:, channel] = .08 * np.sin(2 * np.pi * frequency_bin * np.arange(frames) / frames)
    words = np.frombuffer(blob + b"\x00" * (len(blob) % 2), dtype="<i2").astype("<f4") / 32768
    for start in range(0, frames, 1024):
        for channel in range(metadata_channels):
            chunk = words[channel * 1024:(channel + 1) * 1024]
            pcm[start:start + len(chunk), audio_channels + metadata_channels - 1 - channel] = chunk
    expected = {"bed_sizes": bed_sizes, "bed_components": len(bed_sizes), "bed_channels": bed_channels,
                "bed_labels": labels, "object_groups": object_groups, "objects": objects,
                "audio_channels": audio_channels, "metadata_channels": metadata_channels,
                "input_channels": audio_channels + metadata_channels, "total_bitrate": sum(rates),
                "component_bitrates": rates, "sample_rate": 48000, "frames": frames,
                "repeated_labels_within_large_bed": any(size > 24 for size in bed_sizes),
                "audio_frequencies": frequencies, "azimuths": azimuths, "metadata_hex": blob.hex()}
    output.mkdir(parents=True, exist_ok=False)
    pcm.tofile(output / "input.f32")
    (output / "settings.plist").write_bytes(plistlib.dumps(settings))
    (output / "expected.json").write_text(json.dumps(expected, indent=2) + "\n")
    return expected


def verify_case(root, name):
    expected = json.loads((root / "inputs" / name / "expected.json").read_text())
    encode = root / "cases" / (name + "-encode")
    decode = root / "cases" / (name + "-decode")
    encoded = json.loads((encode / "result.json").read_text())
    decoded = json.loads((decode / "result.json").read_text())
    assert encoded["returncode"] == decoded["returncode"] == 0
    assert not encoded["timed_out"] and not decoded["timed_out"]
    assert not any(key in decoded["research_environment"] for key in (
        "APAC_PROFILE_CEILING", "APAC_BINARY_METADATA", "APAC_DECODER_METADATA"))
    timing = json.loads((encode / "stream.timing.json").read_text())
    audio = np.fromfile(decode / "decoded.f32", dtype="<f4").reshape(-1, expected["audio_channels"])
    assert len(audio) == sum(timing.values()), (len(audio), timing)
    first = timing["leading_frames"]
    audio = audio[first:first + timing["valid_frames"]].astype("float64")
    assert len(audio) == expected["frames"] and np.isfinite(audio).all()
    layout = next(row for row in decoded["events"] if row["stage"] == "decoder_input_layout_value")
    labels = [row["label"] for row in layout["descriptions"]]
    wanted_labels = expected["bed_labels"] + [262144 + i for count in expected["object_groups"] for i in range(count)]
    assert labels == wanted_labels, (labels, wanted_labels)
    spectrum = np.abs(np.fft.rfft(audio, axis=0)) ** 2
    channels = []
    for channel, frequency in enumerate(expected["audio_frequencies"]):
        frequency_bin = round(frequency * len(audio) / 48000)
        fraction = float(spectrum[frequency_bin, channel] / spectrum[:, channel].sum())
        rms = float(np.sqrt(np.mean(audio[:, channel] ** 2)))
        assert np.argmax(spectrum[:, channel]) == frequency_bin, (channel, frequency)
        assert fraction > .95 and rms > .02, (channel, fraction, rms)
        channels.append({"channel": channel, "label": labels[channel], "frequency": frequency,
                         "tone_energy_fraction": fraction, "rms": rms, "valid_frames": len(audio)})
    positions = {}
    for row in decoded["events"]:
        if "derived_spherical" not in row or row.get("decode_metadata_index") is None:
            continue
        group = row.get("object_index")
        if group is None or group < expected["bed_components"]:
            continue
        sample = row["g_probe_pcm_output_frames"] - first
        if 0 <= sample < len(audio):
            positions[sample, group - expected["bed_components"]] = row
    wanted = {(sample, index) for sample in range(0, len(audio), 1024) for index in range(expected["objects"])}
    assert set(positions) == wanted, (len(positions), len(wanted))
    errors = []
    for (sample, index), row in positions.items():
        azimuth, elevation, distance = row["derived_spherical"]
        assert row["group_id"] == expected["bed_components"] + index and elevation == 0 and distance == 1
        error = abs(azimuth - expected["azimuths"][index])
        assert error <= 180 / (1 << row["precision_bits"][0]) + 1e-5
        errors.append(error)
    return {"passed": True, "valid_frames": len(audio), "channels": channels,
            "position_rows": len(positions), "max_azimuth_error_degrees": max(errors),
            "minimum_tone_energy_fraction": min(row["tone_energy_fraction"] for row in channels)}


def run(root, reference, name, beds, groups, ceiling=None):
    if not root.exists():
        root.mkdir(parents=True)
        facts = environment()
        facts["codec_probe_sha256"] = hashlib.sha256((reference / "bin/codec_probe").read_bytes()).hexdigest()
        (root / "environment.json").write_text(json.dumps(facts, indent=2) + "\n")
    expected = generate(root / "inputs" / name, beds, groups)
    for key in ("APAC_PROFILE_CEILING", "APAC_BINARY_METADATA", "APAC_DECODER_METADATA", "APAC_TRACE_LFE"):
        os.environ.pop(key, None)
    trace = Path(__file__).resolve().parent / "trace_codec.py"
    debug = ["xcrun", "lldb", "--no-lldbinit", "--batch", "-o",
             f"command script import {shlex.quote(str(trace))}", "-o", "run", "--"]
    binary = str(reference / "bin/codec_probe")
    os.environ["APAC_TRACE_ERRORS"] = "1"
    os.environ["APAC_TRACE_POSITIONS"] = "0"
    if ceiling is not None:
        os.environ["APAC_PROFILE_CEILING"] = str(ceiling)
    encoded_dir = root / "cases" / (name + "-encode")
    encoded = run_case(debug + [binary, "encode", str(root / "inputs" / name / "settings.plist"),
                               str(root / "inputs" / name / "input.f32"), str(expected["input_channels"]),
                               str(expected["audio_channels"]), str(encoded_dir / "stream"), "0",
                               str(expected["total_bitrate"])], encoded_dir, 60)
    row = {"case": name, "bed_sizes": beds, "bed_channels": sum(beds), "object_groups": groups,
           "objects": sum(groups), "audio_channels": expected["audio_channels"], "diagnostic_ceiling": ceiling,
           "encode_returncode": encoded["returncode"], "encode_timed_out": encoded["timed_out"],
           "encode_last_event": encoded["events"][-1:]}
    row["profile"] = next((event for event in encoded["events"] if "profile" in event), None)
    os.environ.pop("APAC_PROFILE_CEILING", None)
    os.environ.pop("APAC_TRACE_POSITIONS", None)
    if encoded["returncode"] == 0:
        decoded_dir = root / "cases" / (name + "-decode")
        decoded = run_case(debug + [binary, "decode", str(encoded_dir / "stream"), str(decoded_dir / "decoded.f32"),
                                   str(expected["audio_channels"])], decoded_dir, 60)
        row.update(decode_returncode=decoded["returncode"], decode_timed_out=decoded["timed_out"],
                   decode_last_event=decoded["events"][-1:],
                   metadata_capacity=next((event for event in decoded["events"] if event["stage"] == "metadata_sink_capacity"), None))
        if decoded["returncode"] == 0:
            try:
                row["verification"] = verify_case(root, name)
            except (ValueError, AssertionError, StopIteration) as error:
                row["verification_error"] = str(error)
    path = root / "capacity.json"
    rows = json.loads(path.read_text()) if path.exists() else []
    rows.append(row)
    path.write_text(json.dumps(rows, indent=2) + "\n")
    compact = {key: value for key, value in row.items() if key != "verification"}
    if "verification" in row:
        compact["verification"] = {key: value for key, value in row["verification"].items() if key != "channels"}
    print(json.dumps(compact), flush=True)
    return row


if __name__ == "__main__":
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--output", type=Path, required=True)
    parser.add_argument("--reference", type=Path, required=True)
    mode = parser.add_mutually_exclusive_group(required=True)
    mode.add_argument("--name")
    mode.add_argument("--matrix", action="store_true")
    parser.add_argument("--beds", type=int, nargs="+")
    parser.add_argument("--object-groups", type=int, nargs="+")
    parser.add_argument("--ceiling", type=int, choices=(1, 2))
    args = parser.parse_args()
    if args.matrix:
        if args.output.exists():
            parser.error("a full matrix needs a new output directory")
        for name, beds, groups, ceiling in CASES:
            run(args.output.resolve(), args.reference.resolve(), name, beds, groups, ceiling)
    else:
        if not args.beds or not args.object_groups:
            parser.error("--name requires --beds and --object-groups")
        run(args.output.resolve(), args.reference.resolve(), args.name, args.beds, args.object_groups, args.ceiling)
