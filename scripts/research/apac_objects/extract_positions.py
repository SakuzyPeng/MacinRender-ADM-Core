#!/usr/bin/env python3
"""Export positions read from the decoder, never from expected/input metadata."""

import argparse
import csv
import json
from pathlib import Path

import numpy as np


def extract(prefix, decode_result, decoded_pcm, output, objects):
    timing = json.loads(Path(str(prefix) + ".timing.json").read_text())
    result = json.loads(Path(decode_result).read_text())
    if result["returncode"] or result["timed_out"]:
        raise ValueError("decode case failed")
    leading, valid = timing["leading_frames"], timing["valid_frames"]
    positions = {}
    for event in result["events"]:
        if event.get("decode_metadata_index") is None or event.get("object_index") is None:
            continue
        if "derived_spherical" not in event:
            continue
        sample = event["g_probe_pcm_output_frames"] - leading
        if 0 <= sample < valid:
            positions[sample, event["object_index"]] = {
                "sample": sample, "object_index": event["object_index"],
                "group_id": event["group_id"], "position": event["derived_spherical"],
                "quantized": event["quantized"], "precision_bits": event["precision_bits"],
                "packet_index": event["g_probe_decode_packet_index"],
            }
    if not positions or set(index for sample, index in positions if sample == 0) != set(range(objects)):
        raise ValueError("missing decoded object positions at valid sample zero")
    pcm = np.fromfile(decoded_pcm, dtype="<f4").reshape(-1, objects)
    required = leading + valid + timing["trailing_frames"]
    if len(pcm) != required:
        raise ValueError(f"decoded frames {len(pcm)} != packet timing {required}")
    output = Path(output)
    output.mkdir(parents=True, exist_ok=False)
    pcm[leading:leading + valid].tofile(output / "audio.f32")
    rows = [positions[key] for key in sorted(positions)]
    with (output / "positions.csv").open("w") as stream:
        writer = csv.writer(stream)
        for row in rows:
            writer.writerow([row["sample"], row["object_index"], *row["position"]])
    (output / "positions.json").write_text(json.dumps({
        "source": "native decoder after metadata deserialization; no encoder/input trajectory used",
        "decode_result": str(Path(decode_result).resolve()), "timing": timing, "positions": rows,
    }, indent=2) + "\n")
    return rows


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--prefix", required=True)
    parser.add_argument("--decode-result", required=True)
    parser.add_argument("--decoded-pcm", required=True)
    parser.add_argument("--output", required=True)
    parser.add_argument("--objects", type=int, required=True)
    args = parser.parse_args()
    rows = extract(args.prefix, args.decode_result, args.decoded_pcm, args.output, args.objects)
    print(json.dumps({"decoded_position_rows": len(rows)}))


if __name__ == "__main__":
    main()
