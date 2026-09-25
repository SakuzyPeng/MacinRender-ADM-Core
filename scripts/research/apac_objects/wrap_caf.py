#!/usr/bin/env python3
"""Package the exact native APAC packets in CAF, without adding channel coordinates."""

import argparse
import json
from pathlib import Path
import struct


def variable_uint(value):
    result = [value & 127]
    value >>= 7
    while value:
        result.append((value & 127) | 128)
        value >>= 7
    return bytes(reversed(result))


def wrap(prefix, output):
    prefix = str(prefix)
    sample_rate, codec, flags, bytes_per_packet, frames_per_packet, _, channels, bits, _ = struct.unpack(
        "<d8I", Path(prefix + ".asbd").read_bytes())
    entries = [json.loads(line) for line in Path(prefix + ".packet_index.jsonl").read_text().splitlines()]
    timing = json.loads(Path(prefix + ".timing.json").read_text())
    packets = Path(prefix + ".packets").read_bytes()
    if sum(entry["bytes"] for entry in entries) != len(packets):
        raise ValueError("packet table does not match compressed data")
    if len(entries) * frames_per_packet != sum(timing.values()):
        raise ValueError("packet frames do not match valid/leading/trailing frames")
    table = struct.pack(">qqii", len(entries), timing["valid_frames"], timing["leading_frames"], timing["trailing_frames"])
    for entry in entries:
        if not bytes_per_packet:
            table += variable_uint(entry["bytes"])
        if not frames_per_packet:
            table += variable_uint(entry["frames"])
    chunks = [
        (b"desc", struct.pack(">d6I", sample_rate, codec, flags, bytes_per_packet, frames_per_packet, channels, bits)),
        (b"kuki", Path(prefix + ".cookie").read_bytes()),
        (b"pakt", table),
        (b"data", b"\x00" * 4 + packets),
    ]
    with Path(output).open("xb") as file:
        file.write(b"caff\x00\x01\x00\x00")
        for kind, data in chunks:
            file.write(kind + struct.pack(">q", len(data)) + data)


if __name__ == "__main__":
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("prefix")
    parser.add_argument("output")
    args = parser.parse_args()
    wrap(args.prefix, args.output)
