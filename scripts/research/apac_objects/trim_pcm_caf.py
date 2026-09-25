#!/usr/bin/env python3
"""Copy a frame-exact range of constant-packet PCM CAF without conversion."""

import argparse
import hashlib
import json
from pathlib import Path
import struct


def trim(source, output, start_frame, frames):
    if start_frame < 0 or frames < 1:
        raise ValueError("invalid PCM range")
    chunks = []
    with source.open("rb") as stream:
        header = stream.read(8)
        if header[:6] != b"caff\x00\x01":
            raise ValueError("CAF version 1 required")
        while stream.tell() < source.stat().st_size:
            raw = stream.read(12)
            if len(raw) != 12:
                raise ValueError("truncated chunk header")
            kind, length = struct.unpack(">4sq", raw)
            offset = stream.tell()
            if length < 0 or offset + length > source.stat().st_size:
                raise ValueError("unknown-length/truncated chunk unsupported")
            chunks.append((kind, offset, length))
            stream.seek(offset + length)
        if sum(kind == b"data" for kind, _, _ in chunks) != 1:
            raise ValueError("exactly one data chunk required")
        descriptor = next(row for row in chunks if row[0] == b"desc")
        stream.seek(descriptor[1])
        rate, codec, flags, packet_bytes, packet_frames, channels, bits = struct.unpack(">d4s5I", stream.read(32))
        if codec != b"lpcm" or packet_frames != 1 or not packet_bytes:
            raise ValueError("constant-packet one-frame PCM required")
        if any(kind == b"pakt" for kind, _, _ in chunks):
            raise ValueError("PCM packet table needs explicit handling")
        data = next(row for row in chunks if row[0] == b"data")
        source_frames, remainder = divmod(data[2] - 4, packet_bytes)
        if remainder or start_frame + frames > source_frames:
            raise ValueError("range exceeds complete PCM frames")
        output.parent.mkdir(parents=True, exist_ok=True)
        digest = hashlib.sha256()
        with output.open("xb") as target:
            target.write(header)
            for kind, offset, length in chunks:
                stream.seek(offset)
                if kind == b"data":
                    target.write(struct.pack(">4sq", kind, 4 + frames * packet_bytes))
                    target.write(stream.read(4))
                    stream.seek(offset + 4 + start_frame * packet_bytes)
                    remaining = frames * packet_bytes
                    while remaining:
                        block = stream.read(min(remaining, 4 * 1024**2))
                        if not block:
                            raise ValueError("truncated PCM")
                        target.write(block)
                        digest.update(block)
                        remaining -= len(block)
                else:
                    if length > 1024**2:
                        raise ValueError("unexpectedly large metadata chunk")
                    target.write(struct.pack(">4sq", kind, length))
                    target.write(stream.read(length))
    summary = {"source": str(source.resolve()), "source_frames": source_frames, "sample_rate": rate,
               "channels": channels, "bits": bits, "caf_format_flags": flags, "start_frame": start_frame,
               "frames": frames, "seconds": frames / rate, "source_pcm_offset": data[1] + 4 + start_frame * packet_bytes,
               "pcm_bytes": frames * packet_bytes, "pcm_sha256": digest.hexdigest(),
               "pcm_copied_without_conversion": True}
    output.with_suffix(".json").write_text(json.dumps(summary, indent=2) + "\n")
    print(json.dumps(summary, indent=2))


if __name__ == "__main__":
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("source", type=Path)
    parser.add_argument("output", type=Path)
    parser.add_argument("--start-frame", type=int, default=0)
    parser.add_argument("--frames", type=int, required=True)
    args = parser.parse_args()
    trim(args.source, args.output, args.start_frame, args.frames)
