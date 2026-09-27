#!/usr/bin/env python3
"""Build deterministic Cartesian ADM probes from the AC-4 project's DAMF writer."""

import argparse
import copy
import hashlib
import json
import math
import random
import subprocess
import wave
from pathlib import Path
from xml.etree import ElementTree

import numpy as np

AC4_ROOT = Path("/Users/Sakuzy/code/rust/MacinDecode-AC4-Core")
CASE_TEMPLATE = AC4_ROOT / "vectors/probe_axes_single_object/case.json"
DAMF_WRITER = AC4_ROOT / "scripts/gen_damf.py"
NORMALIZER = Path("/Applications/Dolby/Dolby Atmos Conversion Tool/cmdline_atmos_conversion_tool")
RENDER_CLI = Path(__file__).resolve().parents[3] / "build/release/mradm"
FRAMES_PER_POINT = 24_000
BURST_FRAMES = 16_800
PULSE_SEGMENT_FRAMES = 96_000
LONG_PRBS_SEGMENT_FRAMES = 240_000


def file_sha256(path: Path) -> str:
    digest = hashlib.sha256()
    with path.open("rb") as source:
        for chunk in iter(lambda: source.read(1 << 20), b""):
            digest.update(chunk)
    return digest.hexdigest()


def timecode_samples(value: str) -> int:
    hours, minutes, seconds = value.split(":")
    return round((int(hours) * 3600 + int(minutes) * 60 + float(seconds)) * 48_000)


def verify_final_adm(path: Path, points: list) -> dict:
    result = subprocess.run([str(RENDER_CLI), "inspect", "--xml", str(path)],
                            check=True, capture_output=True, text=True)
    root = ElementTree.fromstring(result.stdout)
    channels = [element for element in root.iter()
                if element.tag.endswith("audioChannelFormat")
                and element.attrib.get("typeDefinition") == "Objects"]
    if len(channels) != 1:
        raise ValueError(f"expected one Objects channel in {path}, got {len(channels)}")
    blocks = [element for element in channels[0] if element.tag.endswith("audioBlockFormat")]
    if len(blocks) != len(points):
        raise ValueError(f"expected {len(points)} object blocks, got {len(blocks)}")
    effective_ramps = set()
    for index, (block, (_, expected)) in enumerate(zip(blocks, points)):
        coordinates = {element.attrib["coordinate"]: float(element.text)
                       for element in block if element.tag.endswith("position")}
        actual = tuple(coordinates.get(axis, 0.0) for axis in "XYZ")
        if any(abs(a - b) > 1e-7 for a, b in zip(actual, expected)):
            raise ValueError(f"ADM coordinate mismatch at {index}: {actual} != {expected}")
        if timecode_samples(block.attrib["rtime"]) != index * FRAMES_PER_POINT:
            raise ValueError(f"ADM event time mismatch at {index}")
        if timecode_samples(block.attrib["duration"]) != FRAMES_PER_POINT:
            raise ValueError(f"ADM block duration mismatch at {index}")
        for child in block:
            if child.tag.endswith("jumpPosition"):
                effective_ramps.add(child.attrib.get("interpolationLength", ""))
    return {"object_blocks": len(blocks), "effective_interpolation_lengths": sorted(effective_ramps)}


def point_train() -> list[tuple[str, tuple[float, float, float]]]:
    points = []
    for z in (0.0, 1.0):
        for y in (-1.0, -0.5, 0.0, 0.5, 1.0):
            for x in (-1.0, -0.5, 0.0, 0.5, 1.0):
                points.append((f"grid_{x:g}_{y:g}_{z:g}", (x, y, z)))
    for x, y in ((-1.0, 0.0), (-0.5, 0.0), (0.0, -1.0), (0.0, 0.0),
                 (0.0, 1.0), (0.5, 0.0), (1.0, 0.0)):
        points.append((f"mid_{x:g}_{y:g}", (x, y, 0.5)))
    for radius in (0.25, 0.75):
        points.append((f"radial_side_{radius:g}", (-radius, 0.0, 0.0)))
        points.append((f"radial_front_{radius:g}", (0.0, radius, 0.0)))
    return points


def point_holdout() -> list[tuple[str, tuple[float, float, float]]]:
    points = []
    for z in (0.25, 0.75):
        for y in (-0.75, -0.25, 0.25, 0.75):
            for x in (-0.75, -0.25, 0.25, 0.75):
                points.append((f"between_{x:g}_{y:g}_{z:g}", (x, y, z)))
    points.extend([
        ("near_zenith_l", (-0.05, 0.0, 1.0)),
        ("near_zenith_r", (0.05, 0.0, 1.0)),
        ("near_zenith_front", (0.0, 0.05, 1.0)),
        ("near_zenith_rear", (0.0, -0.05, 1.0)),
        ("near_origin_l", (-0.05, 0.0, 0.0)),
        ("near_origin_r", (0.05, 0.0, 0.0)),
        ("near_origin_front", (0.0, 0.05, 0.0)),
        ("near_origin_rear", (0.0, -0.05, 0.0)),
    ])
    return points


def point_train_extra() -> list[tuple[str, tuple[float, float, float]]]:
    points = []
    for z in (0.25, 0.75):
        for y in (-0.75, -0.25, 0.25, 0.75):
            for x in (-0.75, -0.25, 0.0, 0.25, 0.75):
                points.append((f"refine_{x:g}_{y:g}_{z:g}", (x, y, z)))
    for z in (0.1, 0.2, 0.3, 0.4, 0.5, 0.6, 0.7, 0.8, 0.9):
        points.append((f"vertical_{z:g}", (0.0, 0.0, z)))
    return points


def point_final_holdout() -> list[tuple[str, tuple[float, float, float]]]:
    generator = random.Random(0x5A19C4)
    points = []
    for index in range(48):
        xyz = (round(generator.uniform(-1.0, 1.0), 4),
               round(generator.uniform(-1.0, 1.0), 4),
               round(generator.uniform(0.0, 1.0), 4))
        points.append((f"blind_{index:02d}", xyz))
    points.extend([
        ("front_top_left_edge", (-0.52, 0.52, 0.9)),
        ("front_top_right_edge", (0.52, 0.52, 0.9)),
        ("rear_top_left_edge", (-0.52, -0.52, 0.9)),
        ("rear_top_right_edge", (0.52, -0.52, 0.9)),
        ("center_near_front", (0.01, 0.01, 0.1)),
        ("center_near_rear", (-0.01, -0.01, 0.9)),
    ])
    return points


def point_scan() -> list[tuple[str, tuple[float, float, float]]]:
    points = []
    for step in range(34):
        y = round(0.67 + step * 0.01, 4)
        points.append((f"scan_y_{step:02d}", (0.0, y, 0.5)))
    for step in range(61):
        x = round(-0.6 + step * 0.02, 4)
        points.append((f"scan_x_{step:02d}", (x, 0.0, 1.0)))
    return points


def size_train() -> list[tuple[str, tuple[float, float, float], float]]:
    positions = [
        ("front", (0.0, 1.0, 0.0)),
        ("origin", (0.0, 0.0, 0.0)),
        ("mid_height", (0.0, 0.0, 0.5)),
        ("zenith", (0.0, 0.0, 1.0)),
        ("left_side", (-1.0, 0.0, 0.0)),
        ("interior", (0.25, 0.25, 0.75)),
    ]
    return [(f"size_{size:g}_{name}", xyz, size)
            for size in (0.0, 0.25, 0.5, 1.0) for name, xyz in positions]


def size_holdout() -> list[tuple[str, tuple[float, float, float], float]]:
    positions = [
        ("forward_left", (-0.35, 0.65, 0.25)),
        ("rear_right", (0.55, -0.7, 0.4)),
        ("upper_middle", (0.1, 0.2, 0.8)),
        ("near_top", (-0.15, -0.1, 0.95)),
        ("near_origin", (0.02, -0.03, 0.05)),
    ]
    return [(f"size_{size:g}_{name}", xyz, size)
            for size in (0.125, 0.375, 0.75) for name, xyz in positions]


def size_impulse_points() -> list[tuple[str, tuple[float, float, float], float]]:
    return [
        ("point_front", (0.0, 1.0, 0.0), 0.0),
        ("size_quarter_front", (0.0, 1.0, 0.0), 0.25),
        ("size_quarter_origin", (0.0, 0.0, 0.0), 0.25),
        ("size_quarter_side", (-1.0, 0.0, 0.0), 0.25),
        ("size_full_front", (0.0, 1.0, 0.0), 1.0),
    ]


def write_size_impulse_case(root: Path) -> dict:
    case_id = "size_impulse_identification"
    points = size_impulse_points()
    destination = root / case_id
    destination.mkdir()
    case = copy.deepcopy(json.loads(CASE_TEMPLATE.read_text(encoding="utf-8")))
    case["case_id"] = case_id
    case["intent"] = ["Separate fixed convolution, amplitude dependence and 512-sample phase effects"]
    case["duration_samples"] = len(points) * PULSE_SEGMENT_FRAMES
    prototype = case["objects"][0]
    case["objects"] = []
    manifest_objects = []
    for index, (label, xyz, size) in enumerate(points):
        obj = copy.deepcopy(prototype)
        obj["source_id"] = 10 + index
        obj["name"] = label
        obj["segments"] = [{"start_samples": 0, "position": list(xyz), "label": label}]
        obj.pop("segment_samples", None)
        obj.pop("burst_samples", None)
        base = index * PULSE_SEGMENT_FRAMES
        first = ((base + 4096 + 511) // 512) * 512
        phases = (0, 127, 255, 511)
        levels = (0.125, -0.125, 0.0625, 0.125)
        pulses = [{"sample": first + pulse_index * 12_288 + phase, "amplitude": level}
                  for pulse_index, (phase, level) in enumerate(zip(phases, levels))]
        obj["signal"] = {"kind": "impulses", "pulses": pulses}
        obj["static_fields"]["size"] = size
        case["objects"].append(obj)
        manifest_objects.append({"label": label, "xyz": list(xyz), "size": size,
                                 "input_channel": 10 + index, "pulses": pulses})
    for field in ("encodes", "dme_ac4", "dme_ims", "dee_ims"):
        case.pop(field, None)
    case_path = destination / "case.json"
    case_path.write_text(json.dumps(case, indent=2, ensure_ascii=False) + "\n", encoding="utf-8")
    subprocess.run(["python3", str(DAMF_WRITER), str(case_path)], check=True, capture_output=True, text=True)
    normalized = destination / "normalized"
    normalized.mkdir()
    subprocess.run([str(NORMALIZER), "-i", str(destination / "source/master.atmos"),
                    "-o", str(normalized), "-f", "wav", "--target_fps", "24"],
                   check=True, capture_output=True, text=True)
    adm = normalized / "output.wav"
    result = subprocess.run([str(RENDER_CLI), "inspect", "--xml", str(adm)],
                            check=True, capture_output=True, text=True)
    root_xml = ElementTree.fromstring(result.stdout)
    channels = [element for element in root_xml.iter()
                if element.tag.endswith("audioChannelFormat")
                and element.attrib.get("typeDefinition") == "Objects"]
    if len(channels) != len(points):
        raise ValueError("size impulse ADM object count changed")
    for index, (channel, item) in enumerate(zip(channels, manifest_objects)):
        blocks = [element for element in channel if element.tag.endswith("audioBlockFormat")]
        if len(blocks) != 1 or timecode_samples(blocks[0].attrib["rtime"]) != 0 or timecode_samples(
            blocks[0].attrib["duration"]
        ) != case["duration_samples"]:
            raise ValueError(f"size impulse object {index} timeline changed")
        coordinates = {element.attrib["coordinate"]: float(element.text)
                       for element in blocks[0] if element.tag.endswith("position")}
        extent = {name: next((float(element.text) for element in blocks[0] if element.tag.endswith(name)), 0.0)
                  for name in ("width", "height", "depth")}
        if any(abs(coordinates.get(axis, 0.0) - value) > 1e-7 for axis, value in zip("XYZ", item["xyz"])) or any(
            abs(value - item["size"]) > 1e-7 for value in extent.values()
        ):
            raise ValueError(f"size impulse object {index} position or extent changed")
    with wave.open(str(adm), "rb") as reader:
        if (reader.getnchannels(), reader.getframerate(), reader.getsampwidth(), reader.getnframes()) != (
            10 + len(points), 48_000, 3, case["duration_samples"]
        ):
            raise ValueError("size impulse final ADM PCM format changed")
        payload = reader.readframes(case["duration_samples"])
    bytes3 = np.frombuffer(payload, dtype=np.uint8).reshape(case["duration_samples"], 10 + len(points), 3)
    unsigned = bytes3[:, :, 0].astype(np.int32) | (bytes3[:, :, 1].astype(np.int32) << 8) | (
        bytes3[:, :, 2].astype(np.int32) << 16
    )
    pcm = (unsigned ^ 0x800000) - 0x800000
    if np.any(pcm[:, :10]):
        raise ValueError("size impulse bed must be silent")
    for item in manifest_objects:
        channel = item["input_channel"]
        samples = [pulse["sample"] for pulse in item["pulses"]]
        if not np.array_equal(np.flatnonzero(pcm[:, channel]), samples):
            raise ValueError(f"size impulse PCM timing changed: {item['label']}")
        for pulse in item["pulses"]:
            if abs(int(pcm[pulse["sample"], channel]) - round(pulse["amplitude"] * 8388607)) > 1:
                raise ValueError(f"size impulse PCM amplitude changed: {item['label']}")
    return {"case_id": case_id, "adm": str(adm.resolve()), "sha256": file_sha256(adm),
            "pcm_sha256": hashlib.sha256(payload).hexdigest(), "sample_rate": 48_000,
            "duration_samples": case["duration_samples"], "num_channels": 10 + len(points),
            "objects": manifest_objects,
            "verification": {"object_blocks": len(points), "pulse_count": sum(len(x["pulses"]) for x in manifest_objects)}}


def write_size_prbs_long_case(root: Path, points=None, case_id="size_prbs_identification", seed=0x53495A45) -> dict:
    points = points if points is not None else [
        ("quarter_front", (0.0, 1.0, 0.0), 0.25),
        ("quarter_origin", (0.0, 0.0, 0.0), 0.25),
        ("full_front", (0.0, 1.0, 0.0), 1.0),
    ]
    destination = root / case_id
    destination.mkdir()
    case = copy.deepcopy(json.loads(CASE_TEMPLATE.read_text(encoding="utf-8")))
    case["case_id"] = case_id
    case["intent"] = ["Four-second independent PRBS per static size object with measured silence tail"]
    case["duration_samples"] = len(points) * LONG_PRBS_SEGMENT_FRAMES
    prototype = case["objects"][0]
    case["objects"] = []
    manifest_objects = []
    for index, (label, xyz, size) in enumerate(points):
        obj = copy.deepcopy(prototype)
        obj["source_id"] = 10 + index
        obj["name"] = label
        obj["segments"] = [{"start_samples": 0, "position": list(xyz), "label": label}]
        start = index * LONG_PRBS_SEGMENT_FRAMES + 12_000
        stop = start + 192_000
        obj["signal_segments"] = [{"start_samples": start, "position": list(xyz), "label": label}]
        obj["burst_samples"] = 192_000
        obj["signal"] = {"kind": "prbs", "seed": (seed + index * 0x9E3779B9) & 0xFFFFFFFF,
                         "level_dbfs": -18.0, "fade_samples": 128}
        obj["static_fields"]["size"] = size
        case["objects"].append(obj)
        manifest_objects.append({"label": label, "xyz": list(xyz), "size": size,
                                 "input_channel": 10 + index, "signal_start_sample": start,
                                 "signal_stop_sample": stop})
    for field in ("encodes", "dme_ac4", "dme_ims", "dee_ims"):
        case.pop(field, None)
    case_path = destination / "case.json"
    case_path.write_text(json.dumps(case, indent=2, ensure_ascii=False) + "\n", encoding="utf-8")
    subprocess.run(["python3", str(DAMF_WRITER), str(case_path)], check=True, capture_output=True, text=True)
    normalized = destination / "normalized"
    normalized.mkdir()
    subprocess.run([str(NORMALIZER), "-i", str(destination / "source/master.atmos"),
                    "-o", str(normalized), "-f", "wav", "--target_fps", "24"],
                   check=True, capture_output=True, text=True)
    adm = normalized / "output.wav"
    result = subprocess.run([str(RENDER_CLI), "inspect", "--xml", str(adm)],
                            check=True, capture_output=True, text=True)
    root_xml = ElementTree.fromstring(result.stdout)
    channels = [element for element in root_xml.iter()
                if element.tag.endswith("audioChannelFormat")
                and element.attrib.get("typeDefinition") == "Objects"]
    if len(channels) != len(points):
        raise ValueError("long PRBS ADM object count changed")
    for channel, item in zip(channels, manifest_objects):
        blocks = [element for element in channel if element.tag.endswith("audioBlockFormat")]
        if len(blocks) != 1 or timecode_samples(blocks[0].attrib["rtime"]) != 0 or timecode_samples(
            blocks[0].attrib["duration"]
        ) != case["duration_samples"]:
            raise ValueError(f"long PRBS timeline changed: {item['label']}")
        coordinates = {element.attrib["coordinate"]: float(element.text)
                       for element in blocks[0] if element.tag.endswith("position")}
        extents = {name: next((float(element.text) for element in blocks[0] if element.tag.endswith(name)), 0.0)
                   for name in ("width", "height", "depth")}
        if any(abs(coordinates.get(axis, 0.0) - value) > 1e-7 for axis, value in zip("XYZ", item["xyz"])) or any(
            abs(value - item["size"]) > 1e-7 for value in extents.values()
        ):
            raise ValueError(f"long PRBS position or extent changed: {item['label']}")
    with wave.open(str(adm), "rb") as reader:
        if (reader.getnchannels(), reader.getframerate(), reader.getsampwidth(), reader.getnframes()) != (
            10 + len(points), 48_000, 3, case["duration_samples"]
        ):
            raise ValueError("long PRBS final ADM PCM format changed")
        payload = reader.readframes(case["duration_samples"])
    octets = np.frombuffer(payload, dtype=np.uint8).reshape(case["duration_samples"], 10 + len(points), 3)
    unsigned = octets[:, :, 0].astype(np.int32) | (octets[:, :, 1].astype(np.int32) << 8) | (
        octets[:, :, 2].astype(np.int32) << 16
    )
    pcm = (unsigned ^ 0x800000) - 0x800000
    if np.any(pcm[:, :10]):
        raise ValueError("long PRBS bed must be silent")
    for item in manifest_objects:
        channel = item["input_channel"]
        first, last = item["signal_start_sample"], item["signal_stop_sample"]
        if np.any(pcm[:first, channel]) or np.any(pcm[last:, channel]) or np.count_nonzero(pcm[first:last, channel]) < 180_000:
            raise ValueError(f"long PRBS source timing changed: {item['label']}")
    return {"case_id": case_id, "adm": str(adm.resolve()), "sha256": file_sha256(adm),
            "pcm_sha256": hashlib.sha256(payload).hexdigest(), "sample_rate": 48_000,
            "duration_samples": case["duration_samples"], "num_channels": 10 + len(points),
            "objects": manifest_objects,
            "verification": {"object_blocks": len(points), "signal_frames_per_object": 192_000}}


def write_size_transfer_case(root: Path) -> dict:
    case_id = "size_prbs_transfer"
    destination = root / case_id
    destination.mkdir()
    case = copy.deepcopy(json.loads(CASE_TEMPLATE.read_text(encoding="utf-8")))
    case["case_id"] = case_id
    case["intent"] = ["Same PRBS tests amplitude homogeneity and a 512-sample time shift"]
    case["duration_samples"] = 720_000
    obj = case["objects"][0]
    obj["name"] = case_id
    obj["segments"] = [{"start_samples": 0, "position": [0.0, 1.0, 0.0], "label": case_id}]
    obj["static_fields"]["size"] = 0.25
    segments = [
        {"start_samples": 12_000, "level_dbfs": -18.0, "seed": 0x6C6C5EED},
        {"start_samples": 252_000, "level_dbfs": -30.0, "seed": 0x6C6C5EED},
        {"start_samples": 492_512, "level_dbfs": -18.0, "seed": 0x6C6C5EED},
    ]
    obj["signal_segments"] = [{**segment, "position": [0.0, 1.0, 0.0], "label": f"repeat_{i}"}
                              for i, segment in enumerate(segments)]
    obj["burst_samples"] = 192_000
    obj["signal"] = {"kind": "prbs", "seed": 0x6C6C5EED,
                     "level_dbfs": -18.0, "fade_samples": 128}
    for field in ("encodes", "dme_ac4", "dme_ims", "dee_ims"):
        case.pop(field, None)
    case_path = destination / "case.json"
    case_path.write_text(json.dumps(case, indent=2, ensure_ascii=False) + "\n", encoding="utf-8")
    subprocess.run(["python3", str(DAMF_WRITER), str(case_path)], check=True, capture_output=True, text=True)
    normalized = destination / "normalized"
    normalized.mkdir()
    subprocess.run([str(NORMALIZER), "-i", str(destination / "source/master.atmos"),
                    "-o", str(normalized), "-f", "wav", "--target_fps", "24"],
                   check=True, capture_output=True, text=True)
    adm = normalized / "output.wav"
    result = subprocess.run([str(RENDER_CLI), "inspect", "--xml", str(adm)],
                            check=True, capture_output=True, text=True)
    root_xml = ElementTree.fromstring(result.stdout)
    channels = [element for element in root_xml.iter()
                if element.tag.endswith("audioChannelFormat")
                and element.attrib.get("typeDefinition") == "Objects"]
    if len(channels) != 1:
        raise ValueError("size transfer object count changed")
    blocks = [element for element in channels[0] if element.tag.endswith("audioBlockFormat")]
    if len(blocks) != 1 or timecode_samples(blocks[0].attrib["rtime"]) != 0 or timecode_samples(
        blocks[0].attrib["duration"]
    ) != case["duration_samples"]:
        raise ValueError("size transfer timeline changed")
    coordinates = {element.attrib["coordinate"]: float(element.text)
                   for element in blocks[0] if element.tag.endswith("position")}
    extent = {name: next((float(element.text) for element in blocks[0] if element.tag.endswith(name)), 0.0)
              for name in ("width", "height", "depth")}
    if any(abs(coordinates.get(axis, 0.0) - value) > 1e-7 for axis, value in zip("XYZ", (0.0, 1.0, 0.0))) or any(
        abs(value - 0.25) > 1e-7 for value in extent.values()
    ):
        raise ValueError("size transfer position or extent changed")
    with wave.open(str(adm), "rb") as reader:
        if (reader.getnchannels(), reader.getframerate(), reader.getsampwidth(), reader.getnframes()) != (
            11, 48_000, 3, case["duration_samples"]
        ):
            raise ValueError("size transfer final ADM PCM format changed")
        payload = reader.readframes(case["duration_samples"])
    octets = np.frombuffer(payload, dtype=np.uint8).reshape(case["duration_samples"], 11, 3)
    unsigned = octets[:, :, 0].astype(np.int32) | (octets[:, :, 1].astype(np.int32) << 8) | (
        octets[:, :, 2].astype(np.int32) << 16
    )
    pcm = (unsigned ^ 0x800000) - 0x800000
    if np.any(pcm[:, :10]):
        raise ValueError("size transfer bed must be silent")
    for segment in segments:
        first, last = segment["start_samples"], segment["start_samples"] + 192_000
        if np.count_nonzero(pcm[first:last, 10]) < 180_000:
            raise ValueError("size transfer PRBS is incomplete")
    lower = (pcm[segments[0]["start_samples"]:segments[0]["start_samples"] + 192_000, 10]
             * (10.0 ** (-12.0 / 20.0)))
    actual = pcm[segments[1]["start_samples"]:segments[1]["start_samples"] + 192_000, 10]
    if np.max(np.abs(lower - actual)) > 2.0:
        raise ValueError("size transfer lower-level source differs from the scaled PRBS")
    same = pcm[segments[2]["start_samples"]:segments[2]["start_samples"] + 192_000, 10]
    initial = pcm[segments[0]["start_samples"]:segments[0]["start_samples"] + 192_000, 10]
    if not np.array_equal(same, initial):
        raise ValueError("size transfer shifted source differs from the original PRBS")
    return {"case_id": case_id, "adm": str(adm.resolve()), "sha256": file_sha256(adm),
            "pcm_sha256": hashlib.sha256(payload).hexdigest(), "duration_samples": case["duration_samples"],
            "num_channels": 11, "input_object_channel": 10, "size": 0.25,
            "xyz": [0.0, 1.0, 0.0], "burst_samples": 192_000, "segments": segments,
            "verification": {"object_blocks": 1, "identical_source_segments": [0, 2],
                             "scaled_source_segment": 1}}


def write_size_superposition_case(root: Path) -> dict:
    case_id = "size_prbs_superposition"
    destination = root / case_id
    destination.mkdir()
    case = copy.deepcopy(json.loads(CASE_TEMPLATE.read_text(encoding="utf-8")))
    case["case_id"] = case_id
    case["intent"] = ["Test whether one sized object is linear for two independent PRBS signals"]
    case["duration_samples"] = 720_000
    obj = case["objects"][0]
    obj["name"] = case_id
    obj["segments"] = [{"start_samples": 0, "position": [0.0, 1.0, 0.0], "label": case_id}]
    obj["static_fields"]["size"] = 0.25
    segments = [
        {"start_samples": 12_000, "seed": 0x4561A1C0, "label": "A"},
        {"start_samples": 252_000, "seed": 0x9876C2A1, "label": "B"},
        {"start_samples": 492_000, "seed": 0x4561A1C0, "label": "A_plus_B"},
        {"start_samples": 492_000, "seed": 0x9876C2A1, "label": "A_plus_B_additive", "additive": True},
    ]
    obj["signal_segments"] = [{**segment, "position": [0.0, 1.0, 0.0]}
                              for segment in segments]
    obj["burst_samples"] = 192_000
    obj["signal"] = {"kind": "prbs", "seed": 0x4561A1C0, "level_dbfs": -24.0, "fade_samples": 128}
    for field in ("encodes", "dme_ac4", "dme_ims", "dee_ims"):
        case.pop(field, None)
    case_path = destination / "case.json"
    case_path.write_text(json.dumps(case, indent=2, ensure_ascii=False) + "\n", encoding="utf-8")
    subprocess.run(["python3", str(DAMF_WRITER), str(case_path)], check=True, capture_output=True, text=True)
    normalized = destination / "normalized"
    normalized.mkdir()
    subprocess.run([str(NORMALIZER), "-i", str(destination / "source/master.atmos"),
                    "-o", str(normalized), "-f", "wav", "--target_fps", "24"],
                   check=True, capture_output=True, text=True)
    adm = normalized / "output.wav"
    result = subprocess.run([str(RENDER_CLI), "inspect", "--xml", str(adm)],
                            check=True, capture_output=True, text=True)
    root_xml = ElementTree.fromstring(result.stdout)
    channels = [element for element in root_xml.iter()
                if element.tag.endswith("audioChannelFormat")
                and element.attrib.get("typeDefinition") == "Objects"]
    if len(channels) != 1:
        raise ValueError("superposition ADM object count changed")
    blocks = [element for element in channels[0] if element.tag.endswith("audioBlockFormat")]
    if len(blocks) != 1 or timecode_samples(blocks[0].attrib["rtime"]) != 0 or timecode_samples(
        blocks[0].attrib["duration"]
    ) != case["duration_samples"]:
        raise ValueError("superposition ADM timeline changed")
    coordinates = {element.attrib["coordinate"]: float(element.text)
                   for element in blocks[0] if element.tag.endswith("position")}
    extent = {name: next((float(element.text) for element in blocks[0] if element.tag.endswith(name)), 0.0)
              for name in ("width", "height", "depth")}
    if any(abs(coordinates.get(axis, 0.0) - value) > 1e-7 for axis, value in zip("XYZ", (0.0, 1.0, 0.0))) or any(
        abs(value - 0.25) > 1e-7 for value in extent.values()
    ):
        raise ValueError("superposition ADM position or extent changed")
    with wave.open(str(adm), "rb") as reader:
        if (reader.getnchannels(), reader.getframerate(), reader.getsampwidth(), reader.getnframes()) != (
            11, 48_000, 3, case["duration_samples"]
        ):
            raise ValueError("superposition final ADM PCM format changed")
        payload = reader.readframes(case["duration_samples"])
    octets = np.frombuffer(payload, dtype=np.uint8).reshape(case["duration_samples"], 11, 3)
    unsigned = octets[:, :, 0].astype(np.int32) | (octets[:, :, 1].astype(np.int32) << 8) | (
        octets[:, :, 2].astype(np.int32) << 16
    )
    pcm = (unsigned ^ 0x800000) - 0x800000
    if np.any(pcm[:, :10]):
        raise ValueError("superposition bed must be silent")
    first = pcm[12_000:204_000, 10]
    second = pcm[252_000:444_000, 10]
    combined = pcm[492_000:684_000, 10]
    if np.max(np.abs(first + second - combined)) > 2:
        raise ValueError("superposition final ADM PCM is not the sum of the two inputs")
    return {"case_id": case_id, "adm": str(adm.resolve()), "sha256": file_sha256(adm),
            "pcm_sha256": hashlib.sha256(payload).hexdigest(), "duration_samples": case["duration_samples"],
            "num_channels": 11, "input_object_channel": 10, "size": 0.25,
            "xyz": [0.0, 1.0, 0.0], "burst_samples": 192_000,
            "signal_start_samples": [12_000, 252_000, 492_000],
            "verification": {"object_blocks": 1, "source_sum_error_lsb": int(np.max(np.abs(first + second - combined)))}}


def write_size_two_object_case(root: Path) -> dict:
    case_id = "size_two_object_superposition"
    destination = root / case_id
    destination.mkdir()
    case = copy.deepcopy(json.loads(CASE_TEMPLATE.read_text(encoding="utf-8")))
    case["case_id"] = case_id
    case["intent"] = ["Check whether two sized objects share signal-processing state"]
    case["duration_samples"] = 720_000
    prototype = case["objects"][0]
    case["objects"] = []
    starts = [12_000, 252_000, 492_000]
    for index, (seed, active) in enumerate(((0x4561A1C0, (0, 2)), (0x9876C2A1, (1, 2)))):
        obj = copy.deepcopy(prototype)
        obj["source_id"] = 10 + index
        obj["name"] = f"size_source_{index}"
        obj["segments"] = [{"start_samples": 0, "position": [0.0, 1.0, 0.0], "label": obj["name"]}]
        obj["signal_segments"] = [{"start_samples": starts[i], "position": [0.0, 1.0, 0.0],
                                   "seed": seed, "label": f"{obj['name']}_{i}"}
                                  for i in active]
        obj["burst_samples"] = 192_000
        obj["signal"] = {"kind": "prbs", "seed": seed, "level_dbfs": -24.0, "fade_samples": 128}
        obj["static_fields"]["size"] = 0.25
        case["objects"].append(obj)
    for field in ("encodes", "dme_ac4", "dme_ims", "dee_ims"):
        case.pop(field, None)
    case_path = destination / "case.json"
    case_path.write_text(json.dumps(case, indent=2, ensure_ascii=False) + "\n", encoding="utf-8")
    subprocess.run(["python3", str(DAMF_WRITER), str(case_path)], check=True, capture_output=True, text=True)
    normalized = destination / "normalized"
    normalized.mkdir()
    subprocess.run([str(NORMALIZER), "-i", str(destination / "source/master.atmos"),
                    "-o", str(normalized), "-f", "wav", "--target_fps", "24"],
                   check=True, capture_output=True, text=True)
    adm = normalized / "output.wav"
    result = subprocess.run([str(RENDER_CLI), "inspect", "--xml", str(adm)],
                            check=True, capture_output=True, text=True)
    root_xml = ElementTree.fromstring(result.stdout)
    channels = [element for element in root_xml.iter()
                if element.tag.endswith("audioChannelFormat")
                and element.attrib.get("typeDefinition") == "Objects"]
    if len(channels) != 2:
        raise ValueError("two-object ADM lost an object")
    for channel in channels:
        blocks = [element for element in channel if element.tag.endswith("audioBlockFormat")]
        if len(blocks) != 1 or timecode_samples(blocks[0].attrib["rtime"]) != 0 or timecode_samples(
            blocks[0].attrib["duration"]
        ) != case["duration_samples"]:
            raise ValueError("two-object ADM timeline changed")
        coordinates = {element.attrib["coordinate"]: float(element.text)
                       for element in blocks[0] if element.tag.endswith("position")}
        extent = {name: next((float(element.text) for element in blocks[0] if element.tag.endswith(name)), 0.0)
                  for name in ("width", "height", "depth")}
        if any(abs(coordinates.get(axis, 0.0) - value) > 1e-7 for axis, value in zip("XYZ", (0.0, 1.0, 0.0))) or any(
            abs(value - 0.25) > 1e-7 for value in extent.values()
        ):
            raise ValueError("two-object position or extent changed")
    with wave.open(str(adm), "rb") as reader:
        if (reader.getnchannels(), reader.getframerate(), reader.getsampwidth(), reader.getnframes()) != (
            12, 48_000, 3, case["duration_samples"]
        ):
            raise ValueError("two-object final ADM PCM format changed")
        payload = reader.readframes(case["duration_samples"])
    octets = np.frombuffer(payload, dtype=np.uint8).reshape(case["duration_samples"], 12, 3)
    unsigned = octets[:, :, 0].astype(np.int32) | (octets[:, :, 1].astype(np.int32) << 8) | (
        octets[:, :, 2].astype(np.int32) << 16
    )
    pcm = (unsigned ^ 0x800000) - 0x800000
    if np.any(pcm[:, :10]):
        raise ValueError("two-object bed must be silent")
    for channel, first in ((10, starts[0]), (11, starts[1])):
        third = starts[2]
        if not np.array_equal(pcm[first:first + 192_000, channel], pcm[third:third + 192_000, channel]):
            raise ValueError("two-object repeated source changed")
    return {"case_id": case_id, "adm": str(adm.resolve()), "sha256": file_sha256(adm),
            "pcm_sha256": hashlib.sha256(payload).hexdigest(), "duration_samples": case["duration_samples"],
            "num_channels": 12, "input_object_channels": [10, 11], "size": 0.25,
            "xyz": [0.0, 1.0, 0.0], "burst_samples": 192_000,
            "signal_start_samples": starts,
            "verification": {"object_blocks": 2, "sources_repeat_in_joint_segment": True}}


def write_size_warm_impulse_case(root: Path) -> dict:
    case_id = "size_warm_impulses"
    destination = root / case_id
    destination.mkdir()
    case = copy.deepcopy(json.loads(CASE_TEMPLATE.read_text(encoding="utf-8")))
    case["case_id"] = case_id
    case["intent"] = ["Subtract identical PRBS bursts to reveal size impulse responses under active input"]
    case["duration_samples"] = 480_000
    obj = case["objects"][0]
    obj["name"] = case_id
    obj["segments"] = [{"start_samples": 0, "position": [0.0, 1.0, 0.0], "label": case_id}]
    obj["static_fields"]["size"] = 0.25
    starts = [12_000, 252_000]
    obj["signal_segments"] = [{"start_samples": start, "position": [0.0, 1.0, 0.0],
                               "seed": 0x751A91C0, "label": f"control_{index}"}
                              for index, start in enumerate(starts)]
    obj["burst_samples"] = 192_000
    pulses = [{"sample": starts[1] + offset, "amplitude": amplitude}
              for offset, amplitude in ((24_000, 0.0625), (72_000, -0.0625), (144_000, 0.03125))]
    obj["signal"] = {"kind": "prbs", "seed": 0x751A91C0, "level_dbfs": -24.0,
                     "fade_samples": 128, "impulses": pulses}
    for field in ("encodes", "dme_ac4", "dme_ims", "dee_ims"):
        case.pop(field, None)
    case_path = destination / "case.json"
    case_path.write_text(json.dumps(case, indent=2, ensure_ascii=False) + "\n", encoding="utf-8")
    subprocess.run(["python3", str(DAMF_WRITER), str(case_path)], check=True, capture_output=True, text=True)
    normalized = destination / "normalized"
    normalized.mkdir()
    subprocess.run([str(NORMALIZER), "-i", str(destination / "source/master.atmos"),
                    "-o", str(normalized), "-f", "wav", "--target_fps", "24"],
                   check=True, capture_output=True, text=True)
    adm = normalized / "output.wav"
    result = subprocess.run([str(RENDER_CLI), "inspect", "--xml", str(adm)],
                            check=True, capture_output=True, text=True)
    root_xml = ElementTree.fromstring(result.stdout)
    channels = [element for element in root_xml.iter()
                if element.tag.endswith("audioChannelFormat")
                and element.attrib.get("typeDefinition") == "Objects"]
    if len(channels) != 1:
        raise ValueError("warm impulse ADM object count changed")
    blocks = [element for element in channels[0] if element.tag.endswith("audioBlockFormat")]
    if len(blocks) != 1 or timecode_samples(blocks[0].attrib["rtime"]) != 0 or timecode_samples(
        blocks[0].attrib["duration"]
    ) != case["duration_samples"]:
        raise ValueError("warm impulse ADM timeline changed")
    coordinates = {element.attrib["coordinate"]: float(element.text)
                   for element in blocks[0] if element.tag.endswith("position")}
    extent = {name: next((float(element.text) for element in blocks[0] if element.tag.endswith(name)), 0.0)
              for name in ("width", "height", "depth")}
    if any(abs(coordinates.get(axis, 0.0) - value) > 1e-7 for axis, value in zip("XYZ", (0.0, 1.0, 0.0))) or any(
        abs(value - 0.25) > 1e-7 for value in extent.values()
    ):
        raise ValueError("warm impulse ADM position or extent changed")
    with wave.open(str(adm), "rb") as reader:
        if (reader.getnchannels(), reader.getframerate(), reader.getsampwidth(), reader.getnframes()) != (
            11, 48_000, 3, case["duration_samples"]
        ):
            raise ValueError("warm impulse final ADM PCM format changed")
        payload = reader.readframes(case["duration_samples"])
    octets = np.frombuffer(payload, dtype=np.uint8).reshape(case["duration_samples"], 11, 3)
    unsigned = octets[:, :, 0].astype(np.int32) | (octets[:, :, 1].astype(np.int32) << 8) | (
        octets[:, :, 2].astype(np.int32) << 16
    )
    pcm = (unsigned ^ 0x800000) - 0x800000
    if np.any(pcm[:, :10]):
        raise ValueError("warm impulse bed must be silent")
    control = pcm[starts[0]:starts[0] + 192_000, 10]
    probe = pcm[starts[1]:starts[1] + 192_000, 10]
    delta = probe - control
    active = np.flatnonzero(delta)
    expected = np.array([pulse["sample"] - starts[1] for pulse in pulses])
    if not np.array_equal(active, expected):
        raise ValueError("warm impulse final ADM differs outside the three pulses")
    for pulse in pulses:
        if abs(int(delta[pulse["sample"] - starts[1]]) - round(pulse["amplitude"] * 8388607)) > 2:
            raise ValueError("warm impulse PCM amplitude changed")
    return {"case_id": case_id, "adm": str(adm.resolve()), "sha256": file_sha256(adm),
            "pcm_sha256": hashlib.sha256(payload).hexdigest(), "duration_samples": case["duration_samples"],
            "num_channels": 11, "input_object_channel": 10, "size": 0.25,
            "xyz": [0.0, 1.0, 0.0], "burst_samples": 192_000,
            "signal_start_samples": starts, "pulses": pulses,
            "verification": {"object_blocks": 1, "pulse_count": len(pulses),
                             "source_difference_only_at_pulses": True}}


def write_size_warm_bank_case(root: Path, geometry: bool = False) -> dict:
    case_id = "size_warm_bank_geometry" if geometry else "size_warm_bank"
    destination = root / case_id
    destination.mkdir()
    core_points = [
        ("front_tiny", (0.0, 1.0, 0.0), 0.01),
        ("front_eighth", (0.0, 1.0, 0.0), 0.125),
        ("front_quarter", (0.0, 1.0, 0.0), 0.25),
        ("front_half", (0.0, 1.0, 0.0), 0.5),
        ("front_full", (0.0, 1.0, 0.0), 1.0),
        ("origin_quarter", (0.0, 0.0, 0.0), 0.25),
        ("left_side_quarter", (-1.0, 0.0, 0.0), 0.25),
        ("interior_three_quarters", (0.25, 0.25, 0.75), 0.75),
    ]
    geometry_points = [
        ("front_left", (-0.35, 0.65, 0.25), 0.25),
        ("front_right", (0.35, 0.65, 0.25), 0.25),
        ("rear_left", (-0.55, -0.7, 0.4), 0.25),
        ("rear_right", (0.55, -0.7, 0.4), 0.25),
        ("upper_middle", (0.1, 0.2, 0.8), 0.25),
        ("near_top", (-0.15, -0.1, 0.95), 0.25),
        ("rear_center", (0.0, -1.0, 0.0), 0.25),
        ("near_origin", (0.02, -0.03, 0.05), 0.25),
    ]
    points = geometry_points if geometry else core_points
    case = copy.deepcopy(json.loads(CASE_TEMPLATE.read_text(encoding="utf-8")))
    case["case_id"] = case_id
    case["intent"] = ["Recover stable FIRs at representative positions and isotropic sizes"]
    case["duration_samples"] = len(points) * LONG_PRBS_SEGMENT_FRAMES
    obj = case["objects"][0]
    obj["name"] = case_id
    obj["static_fields"]["size"] = 0.0
    obj["segments"] = []
    obj["signal_segments"] = []
    obj["burst_samples"] = 72_000
    pulses = []
    manifest_points = []
    for index, (label, xyz, size) in enumerate(points):
        base = index * LONG_PRBS_SEGMENT_FRAMES
        obj["segments"].append({"start_samples": base, "position": list(xyz),
                                "size": size, "ramp_samples": 0, "label": label})
        seed = (0x4279165D + index * 0x9E3779B9) & 0xFFFFFFFF
        control = base + 12_000
        probe = base + 132_000
        obj["signal_segments"].extend([
            {"start_samples": control, "position": list(xyz), "seed": seed, "label": f"{label}_control"},
            {"start_samples": probe, "position": list(xyz), "seed": seed, "label": f"{label}_probe"},
        ])
        case_pulses = [{"sample": probe + 24_000, "amplitude": 0.0625},
                       {"sample": probe + 48_000, "amplitude": -0.03125}]
        pulses.extend(case_pulses)
        manifest_points.append({"label": label, "xyz": list(xyz), "size": size,
                                "start_sample": base, "control_sample": control,
                                "probe_sample": probe, "pulses": case_pulses})
    obj["signal"] = {"kind": "prbs", "seed": 0x4279165D, "level_dbfs": -24.0,
                     "fade_samples": 128, "impulses": pulses}
    for field in ("encodes", "dme_ac4", "dme_ims", "dee_ims"):
        case.pop(field, None)
    case_path = destination / "case.json"
    case_path.write_text(json.dumps(case, indent=2, ensure_ascii=False) + "\n", encoding="utf-8")
    subprocess.run(["python3", str(DAMF_WRITER), str(case_path)], check=True, capture_output=True, text=True)
    normalized = destination / "normalized"
    normalized.mkdir()
    subprocess.run([str(NORMALIZER), "-i", str(destination / "source/master.atmos"),
                    "-o", str(normalized), "-f", "wav", "--target_fps", "24"],
                   check=True, capture_output=True, text=True)
    adm = normalized / "output.wav"
    result = subprocess.run([str(RENDER_CLI), "inspect", "--xml", str(adm)],
                            check=True, capture_output=True, text=True)
    root_xml = ElementTree.fromstring(result.stdout)
    channels = [element for element in root_xml.iter()
                if element.tag.endswith("audioChannelFormat")
                and element.attrib.get("typeDefinition") == "Objects"]
    if len(channels) != 1:
        raise ValueError("warm bank must have one ADM object channel")
    blocks = [element for element in channels[0] if element.tag.endswith("audioBlockFormat")]
    if len(blocks) != len(points):
        raise ValueError("warm bank ADM event count changed")
    for index, (block, item) in enumerate(zip(blocks, manifest_points)):
        if timecode_samples(block.attrib["rtime"]) != item["start_sample"] or timecode_samples(
            block.attrib["duration"]
        ) != LONG_PRBS_SEGMENT_FRAMES:
            raise ValueError(f"warm bank event {index} timeline changed")
        coordinates = {element.attrib["coordinate"]: float(element.text)
                       for element in block if element.tag.endswith("position")}
        extent = {name: next((float(element.text) for element in block if element.tag.endswith(name)), 0.0)
                  for name in ("width", "height", "depth")}
        if any(abs(coordinates.get(axis, 0.0) - value) > 1e-7 for axis, value in zip("XYZ", item["xyz"])) or any(
            abs(value - item["size"]) > 1e-7 for value in extent.values()
        ):
            raise ValueError(f"warm bank event {index} position or size changed")
    with wave.open(str(adm), "rb") as reader:
        if (reader.getnchannels(), reader.getframerate(), reader.getsampwidth(), reader.getnframes()) != (
            11, 48_000, 3, case["duration_samples"]
        ):
            raise ValueError("warm bank final ADM PCM format changed")
        payload = reader.readframes(case["duration_samples"])
    octets = np.frombuffer(payload, dtype=np.uint8).reshape(case["duration_samples"], 11, 3)
    unsigned = octets[:, :, 0].astype(np.int32) | (octets[:, :, 1].astype(np.int32) << 8) | (
        octets[:, :, 2].astype(np.int32) << 16
    )
    pcm = (unsigned ^ 0x800000) - 0x800000
    if np.any(pcm[:, :10]):
        raise ValueError("warm bank bed must be silent")
    for item in manifest_points:
        control = pcm[item["control_sample"]:item["control_sample"] + 72_000, 10]
        probe = pcm[item["probe_sample"]:item["probe_sample"] + 72_000, 10]
        delta = probe - control
        expected = np.array([pulse["sample"] - item["probe_sample"] for pulse in item["pulses"]])
        if not np.array_equal(np.flatnonzero(delta), expected):
            raise ValueError(f"warm bank source difference exceeds pulses: {item['label']}")
    return {"case_id": case_id, "adm": str(adm.resolve()), "sha256": file_sha256(adm),
            "pcm_sha256": hashlib.sha256(payload).hexdigest(), "duration_samples": case["duration_samples"],
            "num_channels": 11, "input_object_channel": 10, "burst_samples": 72_000,
            "points": manifest_points,
            "verification": {"object_blocks": len(points), "pulse_count": len(pulses),
                             "control_probe_sources_match_except_pulses": True}}


def gain_control() -> list[tuple[str, tuple[float, float, float], float, float]]:
    return [(f"gain_{gain:g}", (0.0, 0.0, 0.5), 0.0, gain)
            for gain in (0.0, -6.0, -12.0)]


def motion_cases() -> list[tuple[str, list[dict], int]]:
    steps = [
        (0, (0.0, 1.0, 0.0)),
        (48_000, (-1.0, 0.0, 0.0)),
        (96_000, (1.0, 0.0, 0.0)),
        (144_000, (0.0, 0.0, 1.0)),
        (192_000, (0.0, 0.0, 0.5)),
        (240_000, (0.0, 1.0, 0.0)),
    ]
    step_events = [{"start_samples": sample, "position": list(xyz), "ramp_samples": 0}
                   for sample, xyz in steps]
    sweep = [{"start_samples": 0, "position": [-1.0, 0.0, 0.5], "ramp_samples": 0}]
    sweep.extend({"start_samples": 48_000 + index * 4_800,
                  "position": [round(-1.0 + index * 0.1, 5), 0.0, 0.5],
                  "ramp_samples": 4_800}
                 for index in range(1, 21))
    gains = [{"start_samples": sample, "position": [0.0, 0.5, 0.5], "gain": gain,
              "ramp_samples": 0}
             for sample, gain in ((0, 0), (48_000, -6), (96_000, -12), (144_000, 0))]
    return [("motion_steps", step_events, 288_000),
            ("motion_sweep", sweep, 192_000),
            ("motion_gain", gains, 192_000)]


def motion_holdout() -> list[tuple[str, list[dict], int]]:
    # Fixed before measuring Renderer output; event times deliberately do not
    # coincide with the 512-sample panner control period.
    events = [
        {"start_samples": 0, "position": [0.6, -0.4, 0.3], "ramp_samples": 0},
        {"start_samples": 24_000, "position": [-0.3, 0.8, 0.6], "ramp_samples": 4_000},
        {"start_samples": 48_000, "position": [0.15, 0.1, 0.95], "ramp_samples": 0},
        {"start_samples": 70_000, "position": [-0.85, -0.1, 0.2], "ramp_samples": 10_000},
        {"start_samples": 90_000, "position": [0.5, -0.75, 0.7], "ramp_samples": 0},
        {"start_samples": 120_000, "position": [0.0, 0.5, 0.0], "ramp_samples": 8_000},
    ]
    return [("motion_blind_path", events, 144_000)]


def size_motion() -> list[tuple[str, list[dict], int]]:
    events = [
        {"start_samples": 0, "position": [0.0, 1.0, 0.0], "size": 0.0, "ramp_samples": 0},
        {"start_samples": 48_000, "position": [0.0, 1.0, 0.0], "size": 0.25, "ramp_samples": 0},
        {"start_samples": 96_000, "position": [0.0, 1.0, 0.0], "size": 1.0, "ramp_samples": 0},
        {"start_samples": 144_000, "position": [0.0, 1.0, 0.0], "size": 0.5, "ramp_samples": 4_000},
        {"start_samples": 192_000, "position": [0.0, 1.0, 0.0], "size": 0.0, "ramp_samples": 0},
        {"start_samples": 240_000, "position": [0.0, 0.0, 0.0], "size": 0.25, "ramp_samples": 0},
        {"start_samples": 288_000, "position": [0.25, 0.25, 0.75], "size": 0.75, "ramp_samples": 8_000},
    ]
    return [("size_motion_steps", events, 336_000)]


def size_motion_boundary() -> list[tuple[str, list[dict], int]]:
    events = [
        {"start_samples": 0, "position": [0.25, 0.25, 0.75], "size": 0.0, "ramp_samples": 0},
        {"start_samples": 48_000, "position": [0.25, 0.25, 0.75], "size": 0.01, "ramp_samples": 0},
        {"start_samples": 96_000, "position": [0.25, 0.25, 0.75], "size": 0.25, "ramp_samples": 0},
        {"start_samples": 144_000, "position": [0.25, 0.25, 0.75], "size": 0.0, "ramp_samples": 0},
        {"start_samples": 192_000, "position": [0.6, 0.7, 0.4], "size": 0.0, "ramp_samples": 0},
        {"start_samples": 240_000, "position": [0.6, 0.7, 0.4], "size": 0.01, "ramp_samples": 0},
        {"start_samples": 288_000, "position": [0.6, 0.7, 0.4], "size": 0.0, "ramp_samples": 0},
    ]
    return [("size_motion_routing_boundary", events, 336_000)]


def size_route_scan() -> list[tuple[str, list[dict], int]]:
    sizes = (0.0, 0.01, 0.02, 0.04, 0.06, 0.08, 0.10, 0.12,
             0.14, 0.16, 0.18, 0.20, 0.22, 0.24, 0.25)
    events = [{"start_samples": index * 48_000, "position": [0.25, 0.25, 0.75],
               "size": size, "ramp_samples": 0}
              for index, size in enumerate(sizes)]
    return [("size_route_scan", events, len(sizes) * 48_000)]


def size_spatial_train() -> list[tuple[str, list[dict], int]]:
    positions = [
        (-0.75, 0.75, 0.2), (0.75, 0.75, 0.2),
        (-0.75, -0.75, 0.2), (0.75, -0.75, 0.2),
        (0.0, 0.5, 0.5), (-0.5, 0.0, 0.5), (0.5, 0.0, 0.5),
        (0.0, -0.5, 0.5), (0.0, 0.0, 0.8), (0.1, 0.2, 0.8),
        (-0.3, 0.4, 0.75), (0.3, -0.4, 0.75),
    ]
    events = []
    for size in (0.25, 0.5, 1.0):
        for position in positions:
            events.append({"start_samples": len(events) * 72_000, "position": list(position),
                           "size": size, "ramp_samples": 0})
    return [("size_spatial_grid", events, len(events) * 72_000)]


def size_spatial_validation() -> list[tuple[str, list[dict], int]]:
    generator = random.Random(0x5A15C0DE)
    events = []
    for size in (0.125, 0.25, 0.375, 0.5, 0.75, 1.0):
        for _ in range(2):
            position = [round(generator.uniform(-1.0, 1.0), 4),
                        round(generator.uniform(-1.0, 1.0), 4),
                        round(generator.uniform(0.0, 1.0), 4)]
            for signed_x in (position[0], -position[0]):
                events.append({"start_samples": len(events) * 72_000,
                               "position": [signed_x, position[1], position[2]],
                               "size": size, "ramp_samples": 0})
    return [("size_spatial_interleaved_validation", events, len(events) * 72_000)]


def write_case(root: Path, case_id: str, points: list, seed: int) -> dict:
    if not points:
        raise ValueError("empty point set")
    destination = root / case_id
    if destination.exists():
        raise FileExistsError(destination)
    destination.mkdir(parents=True)
    case = copy.deepcopy(json.loads(CASE_TEMPLATE.read_text(encoding="utf-8")))
    case["case_id"] = case_id
    case["intent"] = ["Direct ADM-to-Dolby/SAF Cartesian point-gain calibration"]
    case["duration_samples"] = len(points) * FRAMES_PER_POINT
    case["objects"] = [case["objects"][0]]
    obj = case["objects"][0]
    obj["name"] = case_id
    obj["segments"] = [
        {"start_samples": index * FRAMES_PER_POINT, "position": list(position), "label": label}
        for index, (label, position) in enumerate(points)
    ]
    obj["segment_samples"] = FRAMES_PER_POINT
    obj["burst_samples"] = BURST_FRAMES
    obj["signal"] = {"kind": "prbs", "seed": seed, "level_dbfs": -12.0, "fade_samples": 128}
    obj["static_fields"]["size"] = 0.0
    for field in ("encodes", "dme_ac4", "dme_ims", "dee_ims"):
        case.pop(field, None)
    path = destination / "case.json"
    path.write_text(json.dumps(case, indent=2, ensure_ascii=False) + "\n", encoding="utf-8")
    subprocess.run(["python3", str(DAMF_WRITER), str(path)], check=True, capture_output=True, text=True)
    normalized = destination / "normalized"
    normalized.mkdir()
    subprocess.run(
        [str(NORMALIZER), "-i", str(destination / "source/master.atmos"),
         "-o", str(normalized), "-f", "wav", "--target_fps", "24"],
        check=True, capture_output=True, text=True,
    )
    adm = normalized / "output.wav"
    if not adm.is_file():
        raise RuntimeError(f"ADM normalizer did not create {adm}")
    verified = verify_final_adm(adm, points)
    return {
        "case_id": case_id,
        "adm": str(adm.resolve()),
        "sha256": file_sha256(adm),
        "duration_samples": case["duration_samples"],
        "segment_samples": FRAMES_PER_POINT,
        "burst_samples": BURST_FRAMES,
        "input_object_channel": 10,
        "signal_seed": seed,
        "points": [{"label": label, "xyz": list(position)} for label, position in points],
        "verification": verified,
    }


def write_bank_case(root: Path, case_id: str, points: list, seed: int) -> dict:
    if not points or len(points) > 118:
        raise ValueError("bank must contain between 1 and 118 objects")
    destination = root / case_id
    if destination.exists():
        raise FileExistsError(destination)
    destination.mkdir(parents=True)
    case = copy.deepcopy(json.loads(CASE_TEMPLATE.read_text(encoding="utf-8")))
    case["case_id"] = case_id
    case["intent"] = ["Simultaneous static Cartesian objects for direct gain-matrix recovery"]
    case["duration_samples"] = 96_000
    prototype = case["objects"][0]
    case["objects"] = []
    for index, point in enumerate(points):
        label, position = point[:2]
        size = point[2] if len(point) > 2 else 0.0
        gain_db = point[3] if len(point) > 3 else 0.0
        obj = copy.deepcopy(prototype)
        obj["source_id"] = 10 + index
        obj["name"] = label
        obj["segments"] = [{"start_samples": 0, "position": list(position), "label": label}]
        obj.pop("segment_samples", None)
        obj.pop("burst_samples", None)
        obj["signal"] = {"kind": "prbs", "seed": (seed + index * 0x9E3779B9) & 0xFFFFFFFF,
                         "level_dbfs": -30.0, "fade_samples": 128}
        obj["static_fields"]["size"] = size
        obj["static_fields"]["gain"] = gain_db
        case["objects"].append(obj)
    for field in ("encodes", "dme_ac4", "dme_ims", "dee_ims"):
        case.pop(field, None)
    path = destination / "case.json"
    path.write_text(json.dumps(case, indent=2, ensure_ascii=False) + "\n", encoding="utf-8")
    subprocess.run(["python3", str(DAMF_WRITER), str(path)], check=True, capture_output=True, text=True)
    normalized = destination / "normalized"
    normalized.mkdir()
    subprocess.run(
        [str(NORMALIZER), "-i", str(destination / "source/master.atmos"),
         "-o", str(normalized), "-f", "wav", "--target_fps", "24"],
        check=True, capture_output=True, text=True,
    )
    adm = normalized / "output.wav"
    if not adm.is_file():
        raise RuntimeError(f"ADM normalizer did not create {adm}")
    verified = verify_final_bank(adm, points)
    return {"case_id": case_id, "adm": str(adm.resolve()), "sha256": file_sha256(adm),
            "duration_samples": case["duration_samples"], "input_object_first_channel": 10,
            "objects": [{"label": point[0], "xyz": list(point[1]),
                         "size": point[2] if len(point) > 2 else 0.0,
                         "gain_db": point[3] if len(point) > 3 else 0.0,
                         "input_channel": 10 + index}
                        for index, point in enumerate(points)],
            "verification": verified}


def verify_final_bank(path: Path, points: list) -> dict:
    result = subprocess.run([str(RENDER_CLI), "inspect", "--xml", str(path)],
                            check=True, capture_output=True, text=True)
    root = ElementTree.fromstring(result.stdout)
    channels = [element for element in root.iter()
                if element.tag.endswith("audioChannelFormat")
                and element.attrib.get("typeDefinition") == "Objects"]
    if len(channels) != len(points):
        raise ValueError(f"expected {len(points)} Objects channels, got {len(channels)}")
    actual_by_name = {}
    extents = []
    gains = []
    for channel in channels:
        name = channel.attrib["audioChannelFormatName"]
        blocks = [element for element in channel if element.tag.endswith("audioBlockFormat")]
        if len(blocks) != 1:
            raise ValueError(f"{name}: expected one static ADM block")
        block = blocks[0]
        if timecode_samples(block.attrib["rtime"]) != 0 or timecode_samples(block.attrib["duration"]) != 96_000:
            raise ValueError(f"{name}: wrong static block timeline")
        coordinates = {element.attrib["coordinate"]: float(element.text)
                       for element in block if element.tag.endswith("position")}
        actual_by_name[name] = tuple(coordinates.get(axis, 0.0) for axis in "XYZ")
        extents.append({kind: next((float(item.text) for item in block if item.tag.endswith(kind)), 0.0)
                        for kind in ("width", "height", "depth")})
        gains.append(next((float(item.text) for item in block if item.tag.endswith("gain")), 1.0))
    # The normalizer may replace channel names, but it keeps each object's ID order.
    actual_positions = list(actual_by_name.values())
    expected_positions = [point[1] for point in points]
    if len(actual_positions) != len(expected_positions) or any(
        any(abs(a - b) > 1e-7 for a, b in zip(actual, expected))
        for actual, expected in zip(actual_positions, expected_positions)
    ):
        raise ValueError("ADM bank coordinates or channel order changed")
    for index, (extent, gain) in enumerate(zip(extents, gains)):
        expected_size = points[index][2] if len(points[index]) > 2 else 0.0
        expected_gain = 10.0 ** ((points[index][3] if len(points[index]) > 3 else 0.0) / 20.0)
        if any(abs(value - expected_size) > 1e-7 for value in extent.values()) or not math.isclose(
            gain, expected_gain, abs_tol=1e-7
        ):
            raise ValueError(f"ADM bank size/gain changed at object {index}")
    return {"static_object_blocks": len(actual_positions), "duration_samples": 96_000,
            "effective_extents": extents, "effective_gains": gains}


def write_size_sequence(root: Path, case_id: str, points: list, seed: int) -> dict:
    destination = root / case_id
    if destination.exists():
        raise FileExistsError(destination)
    destination.mkdir(parents=True)
    case = copy.deepcopy(json.loads(CASE_TEMPLATE.read_text(encoding="utf-8")))
    case["case_id"] = case_id
    case["intent"] = ["One active PRBS object per segment for size-energy measurement"]
    case["duration_samples"] = len(points) * FRAMES_PER_POINT
    prototype = case["objects"][0]
    case["objects"] = []
    for index, (label, xyz, size) in enumerate(points):
        obj = copy.deepcopy(prototype)
        obj["source_id"] = 10 + index
        obj["name"] = label
        obj["segments"] = [{"start_samples": 0, "position": list(xyz), "label": label}]
        obj["signal_segments"] = [{"start_samples": index * FRAMES_PER_POINT,
                                   "position": list(xyz), "label": label}]
        obj["segment_samples"] = FRAMES_PER_POINT
        obj["burst_samples"] = BURST_FRAMES
        obj["signal"] = {"kind": "prbs", "seed": (seed + index * 0x9E3779B9) & 0xFFFFFFFF,
                         "level_dbfs": -18.0, "fade_samples": 128}
        obj["static_fields"]["size"] = size
        case["objects"].append(obj)
    for field in ("encodes", "dme_ac4", "dme_ims", "dee_ims"):
        case.pop(field, None)
    path = destination / "case.json"
    path.write_text(json.dumps(case, indent=2, ensure_ascii=False) + "\n", encoding="utf-8")
    subprocess.run(["python3", str(DAMF_WRITER), str(path)], check=True, capture_output=True, text=True)
    normalized = destination / "normalized"
    normalized.mkdir()
    subprocess.run(
        [str(NORMALIZER), "-i", str(destination / "source/master.atmos"),
         "-o", str(normalized), "-f", "wav", "--target_fps", "24"],
        check=True, capture_output=True, text=True,
    )
    adm = normalized / "output.wav"
    if not adm.is_file():
        raise RuntimeError(f"ADM normalizer did not create {adm}")
    result = subprocess.run([str(RENDER_CLI), "inspect", "--xml", str(adm)],
                            check=True, capture_output=True, text=True)
    root_xml = ElementTree.fromstring(result.stdout)
    channels = [element for element in root_xml.iter()
                if element.tag.endswith("audioChannelFormat")
                and element.attrib.get("typeDefinition") == "Objects"]
    if len(channels) != len(points):
        raise ValueError("size sequence lost an object channel")
    effective = []
    for index, (channel, (_, expected_xyz, expected_size)) in enumerate(zip(channels, points)):
        blocks = [element for element in channel if element.tag.endswith("audioBlockFormat")]
        matches = [block for block in blocks if timecode_samples(block.attrib["rtime"]) == 0]
        if not matches:
            raise ValueError(f"size sequence event {index} changed")
        block = matches[-1]
        coordinates = {item.attrib["coordinate"]: float(item.text)
                       for item in block if item.tag.endswith("position")}
        actual_xyz = tuple(coordinates.get(axis, 0.0) for axis in "XYZ")
        if any(abs(a - b) > 1e-7 for a, b in zip(actual_xyz, expected_xyz)):
            raise ValueError(f"size sequence position {index} changed")
        extent = {kind: next((float(item.text) for item in block if item.tag.endswith(kind)), 0.0)
                  for kind in ("width", "height", "depth")}
        if any(abs(value - expected_size) > 1e-7 for value in extent.values()):
            raise ValueError(f"size sequence extent {index} changed")
        effective.append(extent)
    return {"case_id": case_id, "adm": str(adm.resolve()), "sha256": file_sha256(adm),
            "duration_samples": case["duration_samples"], "segment_samples": FRAMES_PER_POINT,
            "burst_samples": BURST_FRAMES, "num_channels": 10 + len(points),
            "segments": [{"label": label, "xyz": list(xyz), "size": size,
                          "input_channel": 10 + index, "start_sample": index * FRAMES_PER_POINT}
                         for index, (label, xyz, size) in enumerate(points)],
            "verification": {"object_blocks": len(effective), "effective_extents": effective}}


def write_motion_case(root: Path, case_id: str, events: list[dict], frames: int, seed: int) -> dict:
    destination = root / case_id
    if destination.exists():
        raise FileExistsError(destination)
    destination.mkdir(parents=True)
    case = copy.deepcopy(json.loads(CASE_TEMPLATE.read_text(encoding="utf-8")))
    case["case_id"] = case_id
    case["intent"] = ["Continuous deterministic PRBS for ADM motion and gain-envelope measurement"]
    case["duration_samples"] = frames
    case["objects"] = [case["objects"][0]]
    obj = case["objects"][0]
    obj["name"] = case_id
    obj["segments"] = events
    obj.pop("segment_samples", None)
    obj.pop("burst_samples", None)
    obj["signal"] = {"kind": "prbs", "seed": seed, "level_dbfs": -18.0, "fade_samples": 128}
    obj["static_fields"]["size"] = 0.0
    for field in ("encodes", "dme_ac4", "dme_ims", "dee_ims"):
        case.pop(field, None)
    path = destination / "case.json"
    path.write_text(json.dumps(case, indent=2, ensure_ascii=False) + "\n", encoding="utf-8")
    subprocess.run(["python3", str(DAMF_WRITER), str(path)], check=True, capture_output=True, text=True)
    normalized = destination / "normalized"
    normalized.mkdir()
    subprocess.run(
        [str(NORMALIZER), "-i", str(destination / "source/master.atmos"),
         "-o", str(normalized), "-f", "wav", "--target_fps", "24"],
        check=True, capture_output=True, text=True,
    )
    adm = normalized / "output.wav"
    if not adm.is_file():
        raise RuntimeError(f"ADM normalizer did not create {adm}")
    result = subprocess.run([str(RENDER_CLI), "inspect", "--xml", str(adm)],
                            check=True, capture_output=True, text=True)
    root_xml = ElementTree.fromstring(result.stdout)
    channels = [element for element in root_xml.iter()
                if element.tag.endswith("audioChannelFormat")
                and element.attrib.get("typeDefinition") == "Objects"]
    if len(channels) != 1:
        raise ValueError("motion ADM must have one Objects channel")
    blocks = [element for element in channels[0] if element.tag.endswith("audioBlockFormat")]
    if len(blocks) != len(events):
        raise ValueError("motion event count changed")
    effective = []
    for index, (block, event) in enumerate(zip(blocks, events)):
        if timecode_samples(block.attrib["rtime"]) != event["start_samples"]:
            raise ValueError(f"motion event time {index} changed")
        coordinates = {item.attrib["coordinate"]: float(item.text)
                       for item in block if item.tag.endswith("position")}
        actual_xyz = tuple(coordinates.get(axis, 0.0) for axis in "XYZ")
        if any(abs(a - b) > 1e-7 for a, b in zip(actual_xyz, event["position"])):
            raise ValueError(f"motion position {index} changed")
        size = {kind: next((float(item.text) for item in block if item.tag.endswith(kind)), 0.0)
                for kind in ("width", "height", "depth")}
        if any(abs(value - event.get("size", 0.0)) > 1e-7 for value in size.values()):
            raise ValueError(f"motion size {index} changed")
        jump = next((item for item in block if item.tag.endswith("jumpPosition")), None)
        gain = next((float(item.text) for item in block if item.tag.endswith("gain")), 1.0)
        expected_gain = 10.0 ** (event.get("gain", 0.0) / 20.0)
        if not math.isclose(gain, expected_gain, abs_tol=1e-7):
            raise ValueError(f"motion event gain {index} changed")
        effective.append({"start_sample": event["start_samples"], "xyz": list(actual_xyz),
                          "gain": gain, "extent": size,
                          "jump_position": jump.text if jump is not None else None,
                          "interpolation_length": jump.attrib.get("interpolationLength") if jump is not None else None})
    return {"case_id": case_id, "adm": str(adm.resolve()), "sha256": file_sha256(adm),
            "duration_samples": frames, "input_object_channel": 10,
            "events": events, "verification": {"effective_events": effective}}


def main() -> None:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--output-root", type=Path, required=True)
    parser.add_argument("--profile", choices=("base", "point-refine", "point-bank", "point-scan", "size-bank",
                                             "size-sequence", "size-impulse", "size-prbs-long", "size-transfer",
                                             "size-superposition",
                                             "size-two-object",
                                             "size-warm-impulse",
                                             "size-warm-bank", "size-warm-bank-geometry",
                                             "motion", "motion-holdout", "size-motion", "size-motion-boundary",
                                             "size-route-scan", "size-spatial-train", "size-spatial-validation",
                                             "gain-control"),
                        default="base")
    args = parser.parse_args()
    root = args.output_root.expanduser().resolve()
    if root.exists():
        parser.error("output root must not already exist")
    for required in (CASE_TEMPLATE, DAMF_WRITER, NORMALIZER, RENDER_CLI):
        if not required.exists():
            parser.error(f"required tool or case missing: {required}")
    root.mkdir(parents=True)
    if args.profile == "base":
        cases = [
            write_case(root, "point_train", point_train(), 0x1A2B3C4D),
            write_case(root, "point_holdout", point_holdout(), 0x5E6F7788),
        ]
    elif args.profile == "point-refine":
        cases = [
            write_case(root, "point_train_extra", point_train_extra(), 0x3451BCDE),
            write_case(root, "point_final_holdout", point_final_holdout(), 0x7AD03123),
        ]
    elif args.profile == "point-bank":
        cases = [
            write_bank_case(root, "bank_train_base", point_train(), 0x4311AC01),
            write_bank_case(root, "bank_train_extra", point_train_extra(), 0x4311AC02),
            write_bank_case(root, "bank_final_holdout", point_final_holdout(), 0x4311AC03),
        ]
    elif args.profile == "point-scan":
        cases = [write_bank_case(root, "bank_scan_xy", point_scan(), 0x4311AC04)]
    elif args.profile == "size-bank":
        cases = [
            write_bank_case(root, "size_train", size_train(), 0x4311AC05),
            write_bank_case(root, "size_holdout", size_holdout(), 0x4311AC06),
        ]
    elif args.profile == "size-sequence":
        cases = [
            write_size_sequence(root, "size_train_sequential", size_train(), 0x4311AC07),
            write_size_sequence(root, "size_holdout_sequential", size_holdout(), 0x4311AC08),
        ]
    elif args.profile == "size-impulse":
        cases = [write_size_impulse_case(root)]
    elif args.profile == "size-prbs-long":
        cases = [write_size_prbs_long_case(root)]
    elif args.profile == "size-transfer":
        cases = [write_size_transfer_case(root)]
    elif args.profile == "size-superposition":
        cases = [write_size_superposition_case(root)]
    elif args.profile == "size-two-object":
        cases = [write_size_two_object_case(root)]
    elif args.profile == "size-warm-impulse":
        cases = [write_size_warm_impulse_case(root)]
    elif args.profile == "size-warm-bank":
        cases = [write_size_warm_bank_case(root)]
    elif args.profile == "size-warm-bank-geometry":
        cases = [write_size_warm_bank_case(root, geometry=True)]
    elif args.profile == "motion":
        cases = [write_motion_case(root, name, events, frames, 0x4311AC09 + index)
                 for index, (name, events, frames) in enumerate(motion_cases())]
    elif args.profile == "motion-holdout":
        cases = [write_motion_case(root, name, events, frames, 0x4311AC0E + index)
                 for index, (name, events, frames) in enumerate(motion_holdout())]
    elif args.profile == "size-motion":
        cases = [write_motion_case(root, name, events, frames, 0x4311AC20 + index)
                 for index, (name, events, frames) in enumerate(size_motion())]
    elif args.profile == "size-motion-boundary":
        cases = [write_motion_case(root, name, events, frames, 0x4311AC30 + index)
                 for index, (name, events, frames) in enumerate(size_motion_boundary())]
    elif args.profile == "size-route-scan":
        cases = [write_motion_case(root, name, events, frames, 0x4311AC40 + index)
                 for index, (name, events, frames) in enumerate(size_route_scan())]
    elif args.profile == "size-spatial-train":
        cases = [write_motion_case(root, name, events, frames, 0x4311AC50 + index)
                 for index, (name, events, frames) in enumerate(size_spatial_train())]
    elif args.profile == "size-spatial-validation":
        cases = [write_motion_case(root, name, events, frames, 0x4311AC60 + index)
                 for index, (name, events, frames) in enumerate(size_spatial_validation())]
    else:
        cases = [write_bank_case(root, "bank_gain_control", gain_control(), 0x4311AC0D)]
    manifest = {"profile": args.profile, "sample_rate": 48_000, "cases": cases}
    (root / "manifest.json").write_text(json.dumps(manifest, indent=2) + "\n", encoding="utf-8")
    print(root / "manifest.json")


if __name__ == "__main__":
    main()
