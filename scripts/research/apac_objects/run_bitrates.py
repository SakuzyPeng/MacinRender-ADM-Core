#!/usr/bin/env python3
"""Probe global and per-component APAC bitrates using the default native decoder."""

import argparse
import hashlib
import json
import os
from pathlib import Path
import plistlib

import numpy as np

from make_inputs import generate as generate_objects
from make_mixed_inputs import generate as generate_mixed
from run_case import run_case
from run_experiments import environment


def prepare(root):
    root.mkdir(parents=True, exist_ok=True)
    binary = root / "bin/bitrate_probe"
    if not (root / "environment.json").exists():
        (root / "environment.json").write_text(json.dumps(environment(), indent=2) + "\n")
    if not binary.exists():
        binary.parent.mkdir(exist_ok=True)
        result = run_case(["xcrun", "clang++", "-std=c++20", "-O2", "-g", "-Wno-deprecated-declarations",
                           str(Path(__file__).resolve().parent / "bitrate_probe.cpp"), "-framework", "AudioToolbox",
                           "-framework", "CoreFoundation", "-o", str(binary)], root / "cases/build-bitrate-probe", 30)
        if result["returncode"]:
            raise RuntimeError("bitrate probe compilation failed")


def inputs(root):
    if not (root / "inputs/object").exists():
        generate_objects(root / "inputs/object", frames=48000)
    if not (root / "inputs/lfe-object").exists():
        generate_mixed(root / "inputs/lfe-object", bed_kind="lfe_only", explicit_bitrates=True)


def run(root, name, source, audio_channels, global_rate, component_rates=None, mode=-1, quality=-1):
    for key in list(os.environ):
        if key.startswith("APAC_"):
            os.environ.pop(key)
    directory = root / "settings" / name
    directory.mkdir(parents=True, exist_ok=False)
    config = plistlib.loads((source / "settings.plist").read_bytes())
    components = config["parameters"][1]["ASComponents"]
    for index, component in enumerate(components):
        key = component["key"]
        component[key] = [field for field in component[key] if field["key"] != "Bit Rate"]
        if component_rates is not None and component_rates[index] is not None:
            component[key].append({"key": "Bit Rate", "current value": component_rates[index]})
    setting = directory / "settings.plist"
    setting.write_bytes(plistlib.dumps(config))
    expected = json.loads((source / "expected.json").read_text())
    input_channels = expected.get("input_channels", expected["objects"] + expected.get("metadata_channels", 1))
    output = root / "cases" / (name + "-encode")
    binary = root / "bin/bitrate_probe"
    encoded = run_case([str(binary), "encode-rate", str(mode), str(quality), str(setting), str(source / "input.f32"),
                        str(input_channels), str(audio_channels), str(output / "stream"), "0", str(global_rate)], output, 30)
    row = {"case": name, "source": str(source), "audio_channels": audio_channels, "requested_global_bps": global_rate,
           "requested_component_bps": component_rates, "requested_mode": mode, "requested_vbr_quality": quality,
           "encode_returncode": encoded["returncode"], "encode_timed_out": encoded["timed_out"],
           "encode_last_event": encoded["events"][-1:]}
    row["property_observations"] = [{**event, "property_fourcc": event["property"].to_bytes(4, "big").decode("ascii")}
                                    for event in encoded["events"] if event["stage"].startswith("bitrate")]
    if encoded["returncode"] == 0:
        timing = json.loads((output / "stream.timing.json").read_text())
        packets = [json.loads(line) for line in (output / "stream.packet_index.jsonl").read_text().splitlines()]
        payload = (output / "stream.packets").read_bytes()
        frames = timing["valid_frames"]
        middle = packets[4:-4]
        row.update(payload_sha256=hashlib.sha256(payload).hexdigest(), cookie_sha256=hashlib.sha256((output / "stream.cookie").read_bytes()).hexdigest(),
                   payload_bytes=len(payload), packets=len(packets), valid_frames=frames,
                   payload_bps_per_valid_duration=len(payload) * 8 * 48000 / frames,
                   payload_bps_per_coded_duration=len(payload) * 8 * 48000 / (len(packets) * 1024),
                   middle_packet_bps=sum(packet["bytes"] for packet in middle) * 8 * 48000 / (len(middle) * 1024) if middle else None)
        decoded_dir = root / "cases" / (name + "-decode")
        decoded = run_case([str(binary), "decode", str(output / "stream"), str(decoded_dir / "decoded.f32"), str(audio_channels)], decoded_dir, 30)
        row.update(decode_returncode=decoded["returncode"], decode_timed_out=decoded["timed_out"], decode_last_event=decoded["events"][-1:])
        if decoded["returncode"] == 0:
            audio = np.fromfile(decoded_dir / "decoded.f32", dtype="<f4").reshape(-1, audio_channels)
            complete = len(audio) == sum(timing.values())
            audio = audio[timing["leading_frames"]:timing["leading_frames"] + frames].astype("float64")
            source_audio = np.fromfile(source / "input.f32", dtype="<f4").reshape(-1, input_channels)[:, :audio_channels].astype("float64")
            spectrum = np.abs(np.fft.rfft(audio, axis=0)) ** 2
            rms = np.sqrt(np.mean(audio ** 2, axis=0))
            error = np.sum((audio - source_audio) ** 2, axis=0)
            energy = np.sum(source_audio ** 2, axis=0)
            row["audio_check"] = {"complete_frames": complete, "valid_frames": len(audio), "finite": bool(np.isfinite(audio).all()),
                                  "rms_per_channel": rms.tolist(), "non_silent_channels": int(np.sum(rms > .001)),
                                  "peak_hz_per_channel": (np.argmax(spectrum, axis=0) * 48000 / len(audio)).tolist(),
                                  "snr_db_per_channel": (10 * np.log10(energy / np.maximum(error, 1e-30))).tolist()}
    path = root / "bitrates.json"
    rows = json.loads(path.read_text()) if path.exists() else []
    rows.append(row)
    temporary = path.with_suffix(".tmp")
    temporary.write_text(json.dumps(rows, indent=2) + "\n")
    temporary.replace(path)
    compact = {key: value for key, value in row.items() if key not in ("property_observations", "source", "payload_sha256", "cookie_sha256", "audio_check")}
    if "audio_check" in row:
        compact["audio_check"] = {key: value for key, value in row["audio_check"].items() if key in ("complete_frames", "non_silent_channels")}
    print(json.dumps(compact), flush=True)
    return row


if __name__ == "__main__":
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--output", required=True, type=Path)
    parser.add_argument("--matrix", choices=("global", "object", "lfe", "mode"), required=True)
    args = parser.parse_args()
    root = args.output.resolve()
    prepare(root)
    inputs(root)
    if args.matrix in ("global", "object"):
        for rate in (0, 1, 6000, 16000, 31999, 32000, 48000, 64000, 128000, 160000, 160001,
                     192000, 256000, 320000, 512000, 1000000, 2000000, 0xFFFFFFFF):
            explicit = args.matrix == "object"
            run(root, f"{args.matrix}-{rate}", root / "inputs/object", 1, 256000 if explicit else rate,
                [rate] if explicit else None)
    elif args.matrix == "lfe":
        for rate in (0, 1, 4000, 7999, 8000, 12000, 16000, 24000, 31999, 32000, 64000, 128000, 256000):
            run(root, f"lfe-{rate}", root / "inputs/lfe-object", 2, 256000 + rate, [rate, 256000])
    else:
        for mode in (0, 1, 2, 3, 4):
            run(root, f"mode-{mode}", root / "inputs/object", 1, 128000, None, mode)
        for quality in (0, 32, 64, 96, 127, 128):
            run(root, f"vbr-quality-{quality}", root / "inputs/object", 1, 128000, None, 3, quality)
