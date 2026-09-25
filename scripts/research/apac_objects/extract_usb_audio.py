#!/usr/bin/env python3
"""Extract a selected AirPods USB OUT endpoint from a local Darwin USB capture.

Selection includes device location, endpoint, successful completion and payload
length. No live capture, default-device change or playback operation is performed.
Payload is preserved as signed LE 32-bit stereo and exported as 24-bit WAV only
after verifying the low byte is zero. Capture time is distinct from PCM time.
"""

import argparse
import collections
import hashlib
import json
from pathlib import Path
import subprocess
import wave

import numpy as np


def extract(pcap, output, location, endpoint=5, rate=48000, tshark="tshark"):
    output.mkdir(parents=True, exist_ok=False)
    fields = ["frame.number", "frame.time_epoch", "usb.darwin.io_id",
              "usb.darwin.io_len", "usb.iso.data"]
    display_filter = (f"usb.darwin.location_id == {location} && "
                      f"usb.darwin.endpoint_address == {endpoint} && "
                      "usb.darwin.request_type == 1 && usb.darwin.io_status == 0 && usb.darwin.io_len > 0")
    command = [tshark, "-r", str(pcap), "-Y", display_filter, "-T", "fields"]
    for field in fields:
        command += ["-e", field]
    result = subprocess.run(command, capture_output=True, text=True, check=True, timeout=30)
    packets, sizes, chunks, seen = [], collections.Counter(), [], set()
    cursor = 0
    for line in result.stdout.splitlines():
        number, timestamp, io_id, size, hex_data = line.split("\t")
        payload = b"".join(bytes.fromhex(part.replace(":", "")) for part in hex_data.split(",") if part)
        if len(payload) != int(size) or len(payload) % 8:
            raise ValueError(f"packet {number}: payload length {len(payload)}, io_len {size}")
        if io_id in seen:
            raise ValueError(f"duplicate completed I/O ID {io_id}")
        seen.add(io_id)
        packets.append({"packet": int(number), "time_epoch": float(timestamp), "io_id": io_id,
                        "first_pcm_frame": cursor, "pcm_frames": len(payload) // 8})
        cursor += len(payload) // 8
        sizes[len(payload)] += 1
        chunks.append(payload)
    if not packets:
        raise ValueError("no selected successful audio transfers; capture is not evidence of silence")
    raw = b"".join(chunks)
    pcm = np.frombuffer(raw, dtype="<i4").reshape(-1, 2).astype(np.float64) / 2**31
    low_zero = bool(np.all(np.frombuffer(raw, dtype=np.uint8)[::4] == 0))
    (output / "audio.s32le").write_bytes(raw)
    if low_zero:
        with wave.open(str(output / "audio.wav"), "wb") as wav:
            wav.setnchannels(2)
            wav.setsampwidth(3)
            wav.setframerate(rate)
            wav.writeframes(np.frombuffer(raw, dtype=np.uint8).reshape(-1, 4)[:, 1:].tobytes())
    gaps = np.diff([packet["time_epoch"] for packet in packets])
    rms = np.sqrt(np.mean(pcm**2, axis=0))
    summary = {"command": command, "source": str(pcap.resolve()), "location_id": location,
               "endpoint": endpoint, "frames": len(pcm), "sample_rate_assumption": rate,
               "channels": 2, "pcm_seconds": len(pcm) / rate, "packets": len(packets),
               "packet_bytes": dict(sorted(sizes.items())), "all_payload_lengths_match": True,
               "unique_completed_io_ids": True, "low_byte_zero": low_zero,
               "capture_time_span": packets[-1]["time_epoch"] - packets[0]["time_epoch"],
               "maximum_completion_gap_seconds": float(np.max(gaps)) if len(gaps) else None,
               "peak": np.max(np.abs(pcm), axis=0).tolist(), "rms": rms.tolist(),
               "source_sha256": hashlib.sha256(pcap.read_bytes()).hexdigest(),
               "pcm_sha256": hashlib.sha256(raw).hexdigest(),
               "observation": "USB OUT payload; no normalization or spatial processing applied"}
    (output / "packets.json").write_text(json.dumps(packets, indent=2) + "\n")
    (output / "summary.json").write_text(json.dumps(summary, indent=2) + "\n")
    print(json.dumps(summary, indent=2))


if __name__ == "__main__":
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("pcap", type=Path)
    parser.add_argument("output", type=Path)
    parser.add_argument("--location", required=True, type=lambda value: int(value, 0))
    parser.add_argument("--endpoint", default=5, type=lambda value: int(value, 0))
    parser.add_argument("--rate", default=48000, type=int)
    parser.add_argument("--tshark", default="tshark")
    args = parser.parse_args()
    extract(args.pcap, args.output, args.location, args.endpoint, args.rate, args.tshark)
