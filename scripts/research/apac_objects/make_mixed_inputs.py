#!/usr/bin/env python3
"""Synthetic channel-bed + objects + metadata input for the APAC mixed-path probe."""

import argparse
import copy
import json
from pathlib import Path
import plistlib
import struct

import numpy as np

from make_inputs import Bits, position


def mixed_metadata(azimuths):
    data = Bits()
    data.put(len(azimuths) + 1, 11)  # one bed renderer group, then object groups
    data.put(0, 1)  # preserve the bed's fixed speaker layout
    for index, azimuth in enumerate(azimuths, 1):
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


def generate(output, objects=1, trajectory="left", bed_kind="center_lfe", explicit_bitrates=False):
    if not 1 <= objects <= 23:
        raise ValueError("this mixed-path experiment supports 1..23 objects")
    labels = ["kAudioChannelLabel_LFEScreen"] if bed_kind == "lfe_only" else [
        "kAudioChannelLabel_Center", "kAudioChannelLabel_Right" if bed_kind == "control" else "kAudioChannelLabel_LFEScreen"]
    bed_channels = len(labels)
    components = [{"key": "Channel Bed", "Channel Bed": [
        {"key": "ChannelLayoutLabel", "current value": labels},
        {"key": "Channel Map", "current minimum range": 0, "current maximum range": bed_channels - 1},
    ]}]
    offset = 0
    groups = []
    while offset < objects:
        count = min(7, objects - offset)
        groups.append(count)
        components.append({"key": "Object", "Object": [
            {"key": "Object Count", "current value": count},
            {"key": "Channel Map", "current minimum range": bed_channels + offset,
             "current maximum range": bed_channels + offset + count - 1},
        ]})
        offset += count
    audio_channels = objects + bed_channels
    config = {"version": 1, "sub version": 2, "parameters": [
        {"key": "Audio Scene Components", "ASComponents": copy.deepcopy(components)},
        {"key": "Codec Configurations", "ASComponents": copy.deepcopy(components)},
        {"key": "APAC Metadata", "Metadata": [{"key": "Channel Map", "current minimum range": audio_channels,
                                                "current maximum range": audio_channels}]},
    ]}
    total_bitrate = 256000 * audio_channels
    if explicit_bitrates:
        has_lfe = bed_kind != "control"
        bed_bitrate = 256000 * (bed_channels - int(has_lfe)) + 16000 * int(has_lfe)
        for component in config["parameters"][1]["ASComponents"]:
            key = component["key"]
            bitrate = bed_bitrate if key == "Channel Bed" else 256000 * component["Object"][0]["current value"]
            component[key].append({"key": "Bit Rate", "current value": bitrate})
        total_bitrate = bed_bitrate + 256000 * objects
    frames = 144000 if trajectory == "moving" else 48000
    time = np.arange(frames) / 48000
    pcm = np.zeros((frames, audio_channels + 1), dtype="<f4")
    if bed_channels == 2:
        pcm[:, 0] = .07 * np.sin(2 * np.pi * 660 * time)
    lfe_channel = bed_channels - 1
    # A deliberate out-of-band tone distinguishes LFE coding from an ordinary channel.
    pcm[:, lfe_channel] = .05 * np.sin(2 * np.pi * 60 * time) + .05 * np.sin(2 * np.pi * 1000 * time)
    frequencies = [1400 + i * 53 for i in range(objects)]
    for i, frequency in enumerate(frequencies):
        pcm[:, bed_channels + i] = .1 * np.sin(2 * np.pi * frequency * time)
    timeline = []
    for start in range(0, frames, 1024):
        if trajectory == "moving":
            azimuths = [60 - 120 * min(1, max(0, (start / 48000 - .5) / 2))] * objects
        elif trajectory == "spread":
            azimuths = [-150 + 300 * (i + .5) / objects for i in range(objects)]
        else:
            azimuths = [60 if trajectory == "left" else -60] * objects
        blob = mixed_metadata(azimuths)
        words = np.frombuffer(blob + b"\x00" * (len(blob) % 2), dtype="<i2").astype("<f4") / 32768
        if len(words) > 1024:
            raise ValueError("one metadata channel is insufficient")
        n = min(len(words), frames - start)
        pcm[start:start + n, -1] = words[:n]
        timeline.append({"sample": start, "azimuths": azimuths})
    output = Path(output)
    output.mkdir(parents=True, exist_ok=False)
    pcm.tofile(output / "input.f32")
    (output / "settings.plist").write_bytes(plistlib.dumps(config))
    expected = {"sample_rate": 48000, "frames": frames, "objects": objects, "bed_channels": bed_channels,
                "audio_channels": audio_channels, "input_channels": audio_channels + 1, "bed_labels": labels,
                "lfe_channel": None if bed_kind == "control" else lfe_channel,
                "lowpass_probe_channel": lfe_channel, "component_groups": groups,
                "object_frequencies": frequencies, "metadata_frame_samples": 1024, "timeline": timeline}
    expected.update(explicit_component_bitrates=explicit_bitrates, total_bitrate=total_bitrate)
    (output / "expected.json").write_text(json.dumps(expected, indent=2) + "\n")
    return expected


if __name__ == "__main__":
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--output", required=True)
    parser.add_argument("--objects", type=int, default=1)
    parser.add_argument("--trajectory", choices=("left", "right", "moving", "spread"), default="left")
    parser.add_argument("--bed", choices=("center_lfe", "control", "lfe_only"), default="center_lfe")
    parser.add_argument("--explicit-bitrates", action="store_true")
    args = parser.parse_args()
    generate(args.output, args.objects, args.trajectory, args.bed, args.explicit_bitrates)
