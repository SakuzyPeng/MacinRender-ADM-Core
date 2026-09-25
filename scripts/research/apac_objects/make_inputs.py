#!/usr/bin/env python3
"""Generate synthetic PCM and AIA 1.4 metadata for the verified APAC ingestion path.

The metadata is a binary bitstream transported through signed 16-bit words in a
normalized Float32 channel. It must not be treated as an audible audio channel.
"""

import argparse
import json
from pathlib import Path
import plistlib
import struct

import numpy as np


class Bits:
    def __init__(self):
        self.values = []

    def put(self, value, count):
        if value < 0 or value >= 1 << count:
            raise ValueError((value, count))
        self.values.extend((value >> bit) & 1 for bit in range(count - 1, -1, -1))

    def floating(self, value):
        self.put(struct.unpack(">I", struct.pack(">f", value))[0], 32)

    def finish(self):
        padding = (-len(self.values)) % 8
        values = self.values + [0] * padding
        return bytes(sum(values[i + j] << (7 - j) for j in range(8)) for i in range(0, len(values), 8))


def position(bits, azimuth):
    # aia_format::Position::Parse, then ItemPosition::operator=/Update.
    bits.put(0, 1)
    bits.put(0, 6)  # reference coordinate frame
    bits.put(0, 1)
    bits.floating(1.0)  # range used by the position quantizer
    bits.put(8, 5)  # spatial precision selector
    bits.put(2, 5)  # rotation precision selector
    bits.put(1, 1)  # position present
    bits.put(0, 1)  # spherical
    for value in (azimuth, 0.0, 1.0):
        bits.floating(value)
    bits.put(0, 1)  # no rotation
    bits.put(0, 1)


def metadata(azimuths):
    # MetadataBase::ParseMetadata -> apdd -> GroupData -> RendererData type 0.
    data = Bits()
    data.put(len(azimuths), 11)
    for index, azimuth in enumerate(azimuths):
        data.put(1, 1)  # group update present
        data.put(index, 11)
        data.put(0, 3)  # one block (count minus one)
        data.put(0, 1)  # no explicit block start
        data.put(0, 1)  # no explicit block duration
        data.put(1, 11)  # one renderer parameter
        data.put(0, 11)  # position
        position(data, azimuth)
    payload = data.finish()
    body = b"\x00\x01apdd" + struct.pack(">H", len(payload)) + payload
    return b"\xff\xff" + struct.pack(">H", 7 + len(body)) + b"\x01\x04\x00" + body


def settings(objects, metadata_channels=1, component_groups=None):
    groups = component_groups or [objects]
    if sum(groups) != objects or any(count < 1 for count in groups):
        raise ValueError("component groups must partition the object audio channels")
    components = []
    first = 0
    for count in groups:
        components.append({
            "key": "Object",
            "Object": [
                {"key": "Object Count", "current value": count},
                {"key": "Channel Map", "current minimum range": first,
                 "current maximum range": first + count - 1},
            ],
        })
        first += count
    parameters = [
        {"key": "Audio Scene Components", "ASComponents": components},
        {"key": "Codec Configurations", "ASComponents": components},
    ]
    if metadata_channels:
        parameters.append({"key": "APAC Metadata", "Metadata": [{
            "key": "Channel Map", "current minimum range": objects,
            "current maximum range": objects + metadata_channels - 1,
        }]})
    return {"version": 1, "sub version": 2, "parameters": parameters}


def generate(output, objects=1, trajectory="left", frames=None, component_groups=None, metadata_channels=None):
    sample_rate = 48000
    frames = frames if frames is not None else sample_rate * (3 if trajectory == "moving" else 1)
    if not 1 <= objects <= 253 or frames < 1024:
        raise ValueError("invalid object count or duration")
    required_channels = (len(metadata([0] * objects)) + 2047) // 2048
    metadata_channels = required_channels if metadata_channels is None else metadata_channels
    if metadata_channels < required_channels or objects + metadata_channels > 255:
        raise ValueError("insufficient metadata capacity or channel map range")
    if metadata_channels > 1 and frames % 1024:
        raise ValueError("multi-channel metadata experiments require whole 1024-sample frames")
    config = settings(objects, metadata_channels, component_groups)
    output = Path(output)
    output.mkdir(parents=True, exist_ok=False)
    pcm = np.zeros((frames, objects + metadata_channels), dtype="<f4")
    # Integer FFT bins also support short, frame-aligned boundary fixtures.
    frequencies = [round((440 + i * 53) * frames / sample_rate) * sample_rate / frames for i in range(objects)]
    for i, frequency in enumerate(frequencies):
        pcm[:, i] = .1 * np.sin(2 * np.pi * frequency * np.arange(frames) / sample_rate)
    timeline = []
    for start in range(0, frames, 1024):
        if trajectory == "moving":
            azimuths = [60 - 120 * min(1, max(0, (start / sample_rate - .5) / 2))] * objects
        elif trajectory == "spread":
            azimuths = [-150 + 300 * (i + .5) / objects for i in range(objects)]
        else:
            azimuths = [60 if trajectory == "left" else -60] * objects
        blob = metadata(azimuths)
        words = np.frombuffer(blob + b"\x00" * (len(blob) % 2), dtype="<i2").astype("<f4") / 32768
        # PCMMetadataReader pulls the last metadata channel first, then works
        # backwards through the range, concatenating 1024 words per channel.
        for channel in range(metadata_channels):
            chunk = words[channel * 1024:(channel + 1) * 1024]
            count = min(len(chunk), frames - start)
            pcm[start:start + count, objects + metadata_channels - 1 - channel] = chunk[:count]
        timeline.append({"sample": start, "azimuths": azimuths, "metadata_hex": blob.hex()})
    pcm.tofile(output / "input.f32")
    (output / "settings.plist").write_bytes(plistlib.dumps(config))
    expected = {
        "sample_rate": sample_rate, "frames": frames, "objects": objects,
        "component_groups": component_groups or [objects], "audio_frequencies": frequencies,
        "metadata_frame_samples": 1024, "metadata_channels": metadata_channels, "timeline": timeline,
    }
    (output / "expected.json").write_text(json.dumps(expected, indent=2) + "\n")
    return expected


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--output", required=True)
    parser.add_argument("--objects", type=int, default=1)
    parser.add_argument("--trajectory", choices=("left", "right", "moving", "spread"), default="left")
    parser.add_argument("--frames", type=int)
    parser.add_argument("--component-groups", type=int, nargs="+")
    parser.add_argument("--metadata-channels", type=int)
    args = parser.parse_args()
    generate(args.output, args.objects, args.trajectory, args.frames, args.component_groups, args.metadata_channels)


if __name__ == "__main__":
    main()
