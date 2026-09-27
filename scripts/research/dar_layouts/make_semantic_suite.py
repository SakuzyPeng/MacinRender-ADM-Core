#!/usr/bin/env python3
"""Small, final-BWF field-isolation probes. Never normalizes away omitted fields."""

import argparse
import copy
import hashlib
import json
import math
import random
import struct
import subprocess
import tempfile
from pathlib import Path
from xml.etree import ElementTree as ET

import numpy as np

from gain_trace_adm import inspect_adm
from make_calibration_suite import CASE_TEMPLATE, DAMF_WRITER, NORMALIZER, file_sha256, write_motion_case

FRAMES = 240_000
SIGNAL_STOP = 192_000
POSITION = [0.23, 0.51, 0.37]


def chunks(path):
    data = path.read_bytes()
    if data[:4] != b"RIFF" or data[8:12] != b"WAVE":
        raise ValueError("small synthetic RIFF WAVE required")
    result = []
    offset = 12
    while offset + 8 <= len(data):
        kind, length = struct.unpack_from("<4sI", data, offset)
        payload = data[offset + 8:offset + 8 + length]
        if len(payload) != length:
            raise ValueError("truncated RIFF")
        result.append((kind, payload))
        offset += 8 + length + (length & 1)
    if offset != len(data):
        raise ValueError("invalid RIFF length")
    return result


def write_riff(path, parts):
    body = b"WAVE" + b"".join(struct.pack("<4sI", kind, len(payload)) + payload +
                              (b"\0" if len(payload) & 1 else b"") for kind, payload in parts)
    path.write_bytes(b"RIFF" + struct.pack("<I", len(body)) + body)


def named(root, name):
    return [item for item in root.iter() if item.tag.split("}")[-1] == name]


def child(parent, name, value=None, attributes=None):
    for item in list(parent):
        if item.tag.split("}")[-1] == name:
            parent.remove(item)
    if value is not None:
        prefix = parent.tag.rsplit("}", 1)[0] + "}" if "}" in parent.tag else ""
        item = ET.SubElement(parent, prefix + name, attributes or {})
        item.text = str(value)


def timecode(samples):
    seconds, fraction = divmod(int(samples), 48_000)
    hours, seconds = divmod(seconds, 3600)
    minutes, seconds = divmod(seconds, 60)
    # DAR 5.5's ADM parser accepts the Conversion Tool's five fractional digits,
    # but rejects nine digits. At 48 kHz this still round-trips every sample.
    return f"{hours:02}:{minutes:02}:{seconds + fraction / 48_000:08.5f}"


def set_time(element, name, value):
    element.attrib.pop(name, None)
    if value is not None:
        element.set(name, timecode(value))


def set_gain(element, value):
    if value is None:
        child(element, "gain")
    else:
        unit = value.get("unit", "linear")
        child(element, "gain", value["value"], {"gainUnit": unit})


def prepare_template(root, seed=0x53454D41):
    path = root / "template.wav"
    if path.exists():
        return path
    case = write_motion_case(root, "template-source", [{"start_samples": 0, "position": POSITION,
                              "size": 0.0, "ramp_samples": 0}], FRAMES, seed)
    parts = chunks(Path(case["adm"]))
    parts = [(kind, payload[:SIGNAL_STOP * 33] + bytes((FRAMES - SIGNAL_STOP) * 33)
              if kind == b"data" else payload) for kind, payload in parts]
    write_riff(path, parts)
    # These files were just generated here and can be recreated from case.json.
    pruned = []
    for item in (root / "template-source").rglob("*"):
        if item.is_file() and item.suffix in (".wav", ".audio", ".metadata", ".atmos"):
            pruned.append({"path": str(item), "sha256": file_sha256(item), "bytes": item.stat().st_size})
            item.unlink()
    (root / "template-prune.json").write_text(json.dumps(pruned, indent=2) + "\n")
    return path


def two_object_dbmd(root):
    path = root / "dbmd-12.bin"
    provenance = root / "dbmd-12-provenance.json"
    if path.exists() and provenance.exists():
        identity = json.loads(provenance.read_text())
        payload = path.read_bytes()
        if (identity.get("channels") != 12 or identity.get("object_channels") != [10, 11] or
                hashlib.sha256(payload).hexdigest() != identity.get("dbmd_sha256")):
            raise ValueError("cached DBMD does not match its 12-channel source template")
        return payload
    # Rebuild an unproven cache from a complete two-object master. Do not infer
    # or patch offsets in this private chunk from its filename or byte length.
    with tempfile.TemporaryDirectory(prefix="dbmd-12-source-", dir=root) as temporary:
        payload, identity = generate_two_object_dbmd(Path(temporary))
    if path.exists() and path.read_bytes() != payload:
        raise ValueError("unproven cached DBMD differs from the regenerated two-object master")
    path.write_bytes(payload)
    provenance.write_text(json.dumps(identity, indent=2) + "\n")
    return payload


def generate_two_object_dbmd(directory):
    case = json.loads(CASE_TEMPLATE.read_text())
    case["case_id"] = "semantic-two-object-template"
    case["duration_samples"] = FRAMES
    first = case["objects"][0]
    first["segments"] = [{"start_samples": 0, "position": POSITION, "size": 0}]
    first.pop("segment_samples", None)
    first.pop("burst_samples", None)
    first["signal"] = {"kind": "silence"}
    second = copy.deepcopy(first)
    second["source_id"] = 11
    second["name"] = "second-object"
    case["objects"] = [first, second]
    for key in ("encodes", "dme_ac4", "dme_ims", "dee_ims"):
        case.pop(key, None)
    specification = directory / "case.json"
    specification.write_text(json.dumps(case, indent=2) + "\n")
    subprocess.run(["python3", str(DAMF_WRITER), str(specification)], check=True, capture_output=True)
    normalized = directory / "normalized"
    normalized.mkdir()
    subprocess.run([str(NORMALIZER), "-i", str(directory / "source/master.atmos"), "-o", str(normalized),
                    "-f", "wav", "--target_fps", "24"], check=True, capture_output=True)
    master = normalized / "output.wav"
    source = inspect_adm(master)
    object_channels = [item["input_channel"] for item in source["objects"]]
    if source["channels"] != 12 or object_channels != [10, 11] or source["frames"] != FRAMES:
        raise ValueError("two-object master topology changed during normalization")
    raw = next(payload for kind, payload in chunks(master) if kind == b"dbmd")
    identity = {"channels": source["channels"], "object_channels": object_channels,
                "dbmd_sha256": hashlib.sha256(raw).hexdigest(), "source_bwf_sha256": source["sha256"],
                "source_pcm_sha256": source["pcm_sha256"], "source_chna_sha256": source["chna_sha256"],
                "producer_case": case, "normalizer_sha256": file_sha256(NORMALIZER),
                "verification": "exact DBMD from a complete normalized master of this topology"}
    return raw, identity


def validate_probe_topology(parts, expected_channels, expected_dbmd):
    """Bind opaque DBMD to its verified source topology; never guess its fields."""
    selected = {}
    for kind, payload in parts:
        if kind in (b"fmt ", b"data", b"chna", b"dbmd"):
            if kind in selected:
                raise ValueError("duplicate topology chunk")
            selected[kind] = payload
    if set(selected) != {b"fmt ", b"data", b"chna", b"dbmd"}:
        raise ValueError("generated probe lacks a required topology chunk")
    if selected[b"dbmd"] != expected_dbmd:
        raise ValueError("DBMD does not match the verified template for this PCM topology")
    fmt = selected[b"fmt "]
    if len(fmt) < 16:
        raise ValueError("truncated PCM format")
    _, channels, rate, byte_rate, alignment, bits = struct.unpack_from("<HHIIHH", fmt)
    if (channels, rate, bits, alignment, byte_rate) != (
            expected_channels, 48000, 24, expected_channels * 3, expected_channels * 3 * 48000):
        raise ValueError("PCM format disagrees with generated topology")
    if len(selected[b"data"]) != FRAMES * alignment:
        raise ValueError("PCM byte count disagrees with generated topology")
    chna = selected[b"chna"]
    if len(chna) != 4 + expected_channels * 40 or struct.unpack_from("<HH", chna) != (channels, channels):
        raise ValueError("CHNA count disagrees with generated topology")
    indices = [struct.unpack_from("<H", chna, 4 + index * 40)[0] for index in range(channels)]
    if sorted(indices) != list(range(1, channels + 1)):
        raise ValueError("CHNA does not bind each PCM channel exactly once")


def add_second_object(parts, specification, dbmd):
    """Two independent PCM bindings; only used by explicit multi-object cases."""
    xml = ET.fromstring(next(payload for kind, payload in parts if kind == b"axml"))
    if "}" in xml.tag:
        ET.register_namespace("", xml.tag.split("}")[0][1:])
    identifiers = {"AO_100b": "AO_100c", "ATU_0000000b": "ATU_0000000c",
                   "AC_00031001": "AC_00031002", "AP_00031001": "AP_00031002",
                   "AS_00031001": "AS_00031002", "AT_00031001_01": "AT_00031002_01"}
    for parent in list(xml.iter()):
        for item in list(parent):
            if not any(value in identifiers for key, value in item.attrib.items() if key.endswith("ID")):
                continue
            clone = copy.deepcopy(item)
            for element in clone.iter():
                for key, value in list(element.attrib.items()):
                    if value in identifiers:
                        element.set(key, identifiers[value])
                    if key == "audioBlockFormatID":
                        element.set(key, value.replace("00031001", "00031002"))
                if element.text in identifiers:
                    element.text = identifiers[element.text]
            if clone.tag.split("}")[-1] == "audioObject":
                clone.set("audioObjectName", specification["id"] + "_second")
                set_time(clone, "start", 96000 if specification["two_objects"] == "handoff" else 0)
                set_time(clone, "duration", 96000)
                set_gain(clone, {"value": 0})
                child(clone, "mute", "1")
            if clone.tag.split("}")[-1] == "audioChannelFormat":
                for block in named(clone, "audioBlockFormat"):
                    for pos in named(block, "position"):
                        pos.text = str({"X": -.4, "Y": -.6, "Z": .7}[pos.attrib["coordinate"]])
            parent.append(clone)
    content = named(xml, "audioContent")[0]
    prefix = content.tag.rsplit("}", 1)[0] + "}" if "}" in content.tag else ""
    ET.SubElement(content, prefix + "audioObjectIDRef").text = "AO_100c"
    encoded = ET.tostring(xml, encoding="utf-8", xml_declaration=True)
    output = []
    for kind, payload in parts:
        if kind == b"axml":
            payload = encoded
        elif kind == b"dbmd":
            payload = dbmd
        elif kind == b"fmt ":
            payload = bytearray(payload)
            struct.pack_into("<H", payload, 2, 12)
            struct.pack_into("<I", payload, 8, 48000 * 36)
            struct.pack_into("<H", payload, 12, 36)
            payload = bytes(payload)
        elif kind == b"chna":
            channels, uids = struct.unpack_from("<HH", payload)
            if channels != 11 or uids != 11:
                raise ValueError("unexpected template channel binding")
            payload = struct.pack("<HH", 12, 12) + payload[4:] + struct.pack(
                "<H12s14s11sc", 12, b"ATU_0000000c", b"AT_00031002_01", b"AP_00031002", b"\0")
        elif kind == b"data":
            original = np.frombuffer(payload, dtype=np.uint8).reshape(FRAMES, 11, 3)
            audio = np.zeros((FRAMES, 12, 3), dtype=np.uint8)
            audio[:, :11] = original
            samples = (np.random.default_rng(0x53454D42).integers(0, 2, FRAMES) * 2 - 1) * 700000
            samples[SIGNAL_STOP:] = 0
            if specification["two_objects"] == "handoff":
                audio[96000:, 10] = 0
                samples[:96000] = 0
            for byte in range(3):
                audio[:, 11, byte] = (samples >> (8 * byte)) & 255
            payload = audio.tobytes()
        output.append((kind, payload))
    return output


def make_case(template, destination, specification):
    destination.mkdir(parents=True, exist_ok=True)
    parts = chunks(template)
    xml = ET.fromstring(next(payload for kind, payload in parts if kind == b"axml"))
    if "}" in xml.tag:
        ET.register_namespace("", xml.tag.split("}")[0][1:])
    formats = [item for item in named(xml, "audioChannelFormat")
               if item.attrib.get("typeDefinition") == "Objects"]
    if len(formats) != 1:
        raise ValueError("expected one Objects channel")
    objects = [item for item in named(xml, "audioObject")
               if any(ref.text == "ATU_0000000b" for ref in named(item, "audioTrackUIDRef"))]
    if not objects:
        identity = inspect_adm(template)
        object_id = identity["objects"][0]["object_ids"][0]
        objects = [item for item in named(xml, "audioObject") if item.attrib["audioObjectID"] == object_id]
    if len(objects) != 1:
        raise ValueError("ambiguous object binding")
    obj = objects[0]
    obj.set("audioObjectName", specification["id"])
    set_gain(obj, specification.get("object_gain"))
    mute = specification.get("mute")
    child(obj, "mute", None if mute is None else ("1" if mute else "0"))
    set_time(obj, "start", specification.get("object_start", 0))
    set_time(obj, "duration", specification.get("object_duration", FRAMES))
    channel = formats[0]
    original = copy.deepcopy(named(channel, "audioBlockFormat")[0])
    for block in named(channel, "audioBlockFormat"):
        channel.remove(block)
    blocks = specification.get("blocks", [{"rtime": 0, "duration": FRAMES}])
    for index, event in enumerate(blocks):
        block = copy.deepcopy(original)
        identifier = original.attrib["audioBlockFormatID"].rsplit("_", 1)[0]
        block.set("audioBlockFormatID", identifier + f"_{index + 1:08x}")
        set_time(block, "rtime", event.get("rtime"))
        set_time(block, "duration", event.get("duration"))
        set_gain(block, event.get("gain", specification.get("block_gain")))
        xyz = event.get("xyz", specification.get("xyz", POSITION))
        for item in named(block, "position"):
            item.text = str(xyz["XYZ".index(item.attrib["coordinate"])])
        size = event.get("size", specification.get("size", 0.0))
        for key in ("width", "height", "depth"):
            child(block, key, size)
        child(block, "diffuse", 0)
        child(block, "jumpPosition", "1", {"interpolationLength": "0.00000"})
        channel.append(block)
    raw_xml = ET.tostring(xml, encoding="utf-8", xml_declaration=True)
    path = destination / "input.wav"
    final_parts = [(kind, raw_xml if kind == b"axml" else payload) for kind, payload in parts]
    expected_channels = 11
    expected_dbmd = next(payload for kind, payload in parts if kind == b"dbmd")
    if specification.get("two_objects"):
        expected_channels = 12
        expected_dbmd = two_object_dbmd(template.parent)
        final_parts = add_second_object(final_parts, specification, expected_dbmd)
    validate_probe_topology(final_parts, expected_channels, expected_dbmd)
    write_riff(path, final_parts)
    identity = inspect_adm(path)
    if [item["input_channel"] for item in identity["objects"]] != list(range(10, expected_channels)):
        raise ValueError("ADM object bindings disagree with generated PCM/DBMD topology")
    template_identity = inspect_adm(template)
    if not specification.get("two_objects") and any(
            identity[key] != template_identity[key] for key in ("pcm_sha256", "chna_sha256", "frames")):
        raise ValueError("field isolation changed PCM/binding/length")
    manifest = {"case_id": specification["id"], "specification": specification, "adm": str(path.resolve()),
                "sha256": identity["sha256"], "identity": identity, "duration_samples": FRAMES,
                "signal_stop_sample": SIGNAL_STOP, "input_channel": 10,
                "topology_verification": {"channels": expected_channels, "dbmd_sha256": identity["dbmd_sha256"],
                                          "exact_verified_template_dbmd": True}}
    if specification.get("two_objects"):
        manifest["signal_event_samples"] = [96000] if specification["two_objects"] == "handoff" else []
    (destination / "case.json").write_text(json.dumps(manifest, indent=2) + "\n")
    return manifest


def boundary_cases():
    result = []
    for size in (0, .01, .25, 1):
        tag = str(size).replace(".", "p")
        result.append({"id": f"baseline_s{tag}", "size": size})
        for field in ("object_gain", "block_gain"):
            for gain in (0, .25, .5, 1, 2):
                for unit in ("linear", "dB"):
                    # dB -infinity is probed explicitly; its raw spelling is retained.
                    value = gain if unit == "linear" else ("-INF" if gain == 0 else 20 * math.log10(gain))
                    result.append({"id": f"{field}_{gain}_{unit}_s{tag}", "size": size,
                                   field: {"value": value, "unit": unit}})
        for mute in (False, True):
            result.append({"id": f"mute_{int(mute)}_s{tag}", "size": size, "mute": mute})
        result.append({"id": f"combined_s{tag}", "size": size,
                       "object_gain": {"value": .5}, "block_gain": {"value": .25}, "mute": False})
        result.append({"id": f"muted_zero_s{tag}", "size": size, "mute": True,
                       "object_gain": {"value": 0}})
        result.append({"id": f"block_gain_steps_s{tag}", "size": size, "blocks": [
            {"rtime": at, "duration": 48000 if at < 144000 else 96000, "gain": {"value": gain}}
            for at, gain in ((0, 1), (48000, 0), (96000, .25), (144000, 2))]})
    for size in (0, .25):
        tag = str(size).replace(".", "p")
        result.extend([
            {"id": f"object_zero_duration_s{tag}", "size": size, "object_duration": 0},
            {"id": f"block_zero_duration_s{tag}", "size": size, "blocks": [{"rtime": 0, "duration": 0}]},
            {"id": f"missing_object_start_s{tag}", "size": size, "object_start": None},
            {"id": f"missing_object_duration_s{tag}", "size": size, "object_duration": None},
            {"id": f"object_start_s{tag}", "size": size, "object_start": 48000,
             "blocks": [{"rtime": 0, "duration": FRAMES - 48000}]},
            {"id": f"object_duration_s{tag}", "size": size, "object_duration": 96000},
            {"id": f"block_start_s{tag}", "size": size, "blocks": [{"rtime": 48000, "duration": 192000}]},
            {"id": f"block_duration_s{tag}", "size": size, "blocks": [{"rtime": 0, "duration": 96000}]},
            {"id": f"missing_duration_s{tag}", "size": size, "blocks": [{"rtime": 0}]},
            {"id": f"missing_rtime_s{tag}", "size": size, "blocks": [{"duration": FRAMES}]},
            {"id": f"block_gap_s{tag}", "size": size, "blocks": [
                {"rtime": 0, "duration": 48000}, {"rtime": 96000, "duration": 144000}]},
            {"id": f"gap_motion_s{tag}", "size": size, "blocks": [
                {"rtime": 0, "duration": 48000},
                {"rtime": 96000, "duration": 144000, "xyz": [-.4, -.6, .7]}]},
        ])
        for offset in (31, 32, 33, 511, 512, 513):
            result.append({"id": f"first_block_{offset}_s{tag}", "size": size,
                           "blocks": [{"rtime": offset, "duration": FRAMES - offset}]})
            result.append({"id": f"boundary_{offset}_s{tag}", "size": size,
                           "object_start": 48000 + offset, "object_duration": 96000,
                           "blocks": [{"rtime": 0, "duration": 48000},
                                      {"rtime": 48000 + 512, "duration": 47000, "xyz": [-.2, .8, .5]}]})
    result.extend([{"id": "two_object_" + mode, "two_objects": mode, "size": .25,
                    "object_duration": 96000, "object_gain": {"value": .5}}
                   for mode in ("handoff", "overlap")])
    return result


def final_cases(seed):
    generator = random.Random(seed)
    result = []
    for index in range(16):
        first = generator.choice([0, 31, 32, 511, 512, 513, 48031])
        times = [first, 96000 if first >= 48000 else 48000 + generator.randrange(1024),
                 144000 + generator.randrange(1024)]
        events = []
        for at in times:
            gain = generator.uniform(.05, 2)
            unit = generator.choice(["linear", "dB"])
            events.append({"rtime": at,
                           "duration": generator.choice([None, 48000, 48031]),
                           "xyz": [round(generator.uniform(-.9, .9), 4),
                                   round(generator.uniform(-.9, .9), 4),
                                   round(generator.uniform(0, 1), 4)],
                           "size": round(generator.uniform(0, 1), 4) if index % 4 else 0,
                           "gain": {"unit": unit, "value": gain if unit == "linear" else 20 * math.log10(gain)}})
        result.append({"id": f"final_{index:02}", "blocks": events,
                       "object_start": generator.choice([0, 31, 48000, None]),
                       "object_duration": generator.choice([96000, 192000, 240000, None]),
                       "object_gain": {"value": generator.choice([0, .25, .5, 1, 2])},
                       "mute": generator.choice([False, True, None])})
    return result


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--output-dir", type=Path, required=True)
    parser.add_argument("--case", help="Generate just this case, otherwise write specifications only")
    args = parser.parse_args()
    root = args.output_dir.resolve()
    root.mkdir(parents=True, exist_ok=True)
    specifications = boundary_cases()
    (root / "specifications.json").write_text(json.dumps(specifications, indent=2) + "\n")
    template = prepare_template(root)
    if args.case:
        selected = next(item for item in specifications if item["id"] == args.case)
        print(make_case(template, root / args.case, selected)["adm"])


if __name__ == "__main__":
    main()
