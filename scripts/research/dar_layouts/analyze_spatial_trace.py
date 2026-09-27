#!/usr/bin/env python3
"""Summarize parsed spatial fields and their actual OMO consumers from capture."""

import argparse
import gzip
import hashlib
import json
import struct
from pathlib import Path


def analyze(directory):
    summary = json.loads((directory / "summary.json").read_text())
    if not (summary.get("success") and summary.get("state_restored") and summary.get("hook_restored") and
            summary.get("pcm_comparison", {}).get("all_samples_exact")):
        raise ValueError("trace must match uninstrumented PCM and restore state")
    path = directory / "trace.json"
    data = path.read_bytes() if path.exists() else gzip.decompress(path.with_suffix(".json.gz").read_bytes())
    trace = json.loads(data)
    base, image = int(trace["load_base"], 0), int("0x100000000", 0)
    parsed, dispatch, processing = [], [], []
    for record in trace["records"]:
        regions = {item["name"]: bytes.fromhex(item["hex"]) for item in record["regions"]}
        if record.get("detail") == "adm_diffuse_boolean":
            state = regions["block_consumer_state56"]
            parsed.append({"argument": record["argument3"],
                           "presence_mask_offset22": struct.unpack_from("<H", state, 22)[0],
                           "stored_bool_offset44": state[44], "width_offset48": struct.unpack_from("<f", state, 48)[0]})
        elif record.get("detail") == "adm_block_element":
            name = regions.get("element_name_utf8", b"").decode()
            if name not in ("diffuse", "objectDivergence"):
                continue
            active = struct.unpack_from("<Q", regions["parser_header"], 16)[0]
            converter = regions.get("active_value_converter")
            dispatch.append({"element": name, "has_delegate": bool(active),
                             "converter_vtable": hex(struct.unpack_from("<Q", converter)[0] - base + image)
                             if converter else None})
        elif record["kind"] == "omo_node":
            before, after = regions["input_events72"], regions["direct_events72"]
            for channel in range(10, len(before) // 72):
                offset = channel * 72
                processing.append({"sample": record["sample_start"], "input_channel": channel,
                                   "input_size": struct.unpack_from("<f", before, offset + 52)[0],
                                   "input_diffuse_bool": struct.unpack_from("<I", before, offset + 56)[0],
                                   "direct_size": struct.unpack_from("<f", after, offset + 52)[0],
                                   "direct_diffuse_bool": struct.unpack_from("<I", after, offset + 56)[0]})
    return {"adm_sha256": summary["adm_sha256"], "layout": summary["layout"],
            "trace_sha256": hashlib.sha256(data).hexdigest(), "pcm_control_exact": True,
            "state_restored": True, "parsed_diffuse": parsed, "field_dispatch": dispatch,
            "omo_metadata": processing, "hooks": trace["hooks"]}


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--trace-root", type=Path, required=True)
    args = parser.parse_args()
    report = analyze(args.trace_root)
    path = args.trace_root / "spatial-semantics.json"
    path.write_text(json.dumps(report, indent=2) + "\n")
    print(path)


if __name__ == "__main__":
    main()
