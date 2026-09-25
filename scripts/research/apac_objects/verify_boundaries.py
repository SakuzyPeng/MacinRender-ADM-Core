#!/usr/bin/env python3
"""Verify object-count observations without treating encoding alone as playback support."""

import argparse
import json
from pathlib import Path

from verify_results import inspect


def verify(root):
    rows = json.loads((root / "boundaries.json").read_text())
    assert len(rows) == 24, "incomplete boundary schedule"
    checked = []
    for row in rows:
        assert not row["encode_timed_out"], row["case"]
        if row["encode_returncode"] == 0 and row.get("decode_returncode") == 0:
            assert not row["decode_timed_out"]
            item = inspect(root, row["case"], row["case"], row["objects"])
            decoded = json.loads((root / "cases" / (row["case"] + "-decode") / "result.json").read_text())
            assert "APAC_DECODER_METADATA" not in decoded["research_environment"]
            assert "APAC_PROFILE_CEILING" not in decoded["research_environment"]
            checked.append(item)
        elif row.get("decode_returncode") == 1:
            assert not row["decode_timed_out"]
            assert row["decode_last_event"] == [{"stage": "decoder_produce", "status": -50}], row
        elif row["encode_returncode"]:
            assert row["encode_last_event"] in [
                [{"stage": "set_acs", "status": 560226676}],
                [{"stage": "initialize", "status": 560226676}],
                [{"stage": "initialize", "status": 561214580}],
                [{"stage": "cookie_info", "status": 560100710}],
            ], row
        else:
            assert row["objects"] == 128 and row["diagnostic_ceiling"] is None
            assert row["decode_validation"] == "not_run_encoder_capacity_probe"
    def find(count, groups, ceiling):
        return next(row for row in rows if (row["objects"], row["component_groups"], row["diagnostic_ceiling"]) == (count, groups, ceiling))
    assert find(70, [7] * 10, None)["verification"]["passed"]
    assert find(71, [7] * 10 + [1], None)["decode_returncode"] == 1
    assert find(128, [7] * 18 + [2], None)["encode_returncode"] == 0
    assert find(129, [7] * 18 + [3], None)["encode_returncode"] == 1
    assert find(129, [65, 64], 2)["encode_returncode"] == 0
    capacities = json.loads((root / "decoder_capacity.json").read_text())
    assert [(item["objects"], item["observations"][0]["required_bytes"],
             item["observations"][0]["capacity_bytes"]) for item in capacities] == [(70, 4070, 4096), (71, 4128, 4096)]
    assert not any(item["decoder_modified"] for item in capacities)
    containers = json.loads((root / "container_boundary_reads.json").read_text())
    for item in containers:
        assert item["returncode"] == 0 and item["event"]["status"] == 2
        assert item["event"]["frames"] == (8192 if item["objects"] == 70 else 6144)
    semantics = json.loads((root / "asset_pcm_semantics.json").read_text())
    assert semantics["frames"] == 8192 and not semantics["dominant_tones_match"]
    mapping = json.loads((root / "channel_map_boundary.json").read_text())
    assert mapping["encode_returncode"] == 1 and mapping["last_event"] == [{"stage": "set_acs", "status": 560226676}]
    return {
        "all_checks_passed": True,
        "meaning": "Observed successes and expected failures matched; not all object counts are playable.",
        "decoder_policy": "System default capabilities and configuration; read-only debugger observations only.",
        "max_full_native_roundtrip_tested": 70,
        "next_object_count_rejected_by_default_metadata_sink": 71,
        "metadata_sink_capacity_bytes": 4096,
        "default_encoder_automatic_layout_boundary": {"completed": 128, "rejected": 129},
        "diagnostic_encoder_largest_complete_output_tested": 129,
        "diagnostic_encoder_upper_limit_established": False,
        "format_wide_object_limit_established": False,
        "verified_native_cases": checked,
        "decoder_capacity": capacities,
        "avassetreader_counts": containers,
        "avassetreader_preserves_individual_object_signals": False,
        "avassetreader_pcm_semantics": semantics,
        "channel_map_boundary": mapping,
    }


if __name__ == "__main__":
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--output", type=Path, required=True)
    args = parser.parse_args()
    result = verify(args.output)
    (args.output / "verification.json").write_text(json.dumps(result, indent=2) + "\n")
    print(json.dumps({"observations_verified": True, "native_roundtrip_cases": len(result["verified_native_cases"]),
                      "max_full_native_roundtrip_tested": result["max_full_native_roundtrip_tested"]}))
