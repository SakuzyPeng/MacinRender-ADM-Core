#!/usr/bin/env python3
"""Check signal superposition and slot independence for two sized ADM Objects."""

import argparse
import hashlib
import json
import wave
from pathlib import Path

import numpy as np

from measure_point_suite import decode_wav
from measure_size_field import canonical_pcm
from measure_static_bank import file_sha256, read_multichannel_adm


def relative_error(reference: np.ndarray, observed: np.ndarray) -> float:
    return float(np.linalg.norm(reference - observed) / max(np.linalg.norm(reference), 1e-12))


def main() -> None:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--suite-manifest", type=Path, required=True)
    parser.add_argument("--case", required=True)
    parser.add_argument("--dar-run", type=Path, required=True)
    parser.add_argument("--channel-map", type=Path, required=True)
    parser.add_argument("--single-object-reference", type=Path,
                        help="Prior same-signal single-object superposition WAV")
    parser.add_argument("--output", type=Path, required=True)
    args = parser.parse_args()
    if args.output.exists():
        parser.error("output exists")
    suite = json.loads(args.suite_manifest.read_text())
    case = next((item for item in suite["cases"] if item["case_id"] == args.case), None)
    if case is None or "input_object_channels" not in case:
        parser.error("case is not a two-object probe")
    adm = Path(case["adm"])
    if file_sha256(adm) != case["sha256"]:
        raise ValueError("final ADM changed")
    with wave.open(str(adm), "rb") as reader:
        if hashlib.sha256(reader.readframes(case["duration_samples"])).hexdigest() != case["pcm_sha256"]:
            raise ValueError("final ADM PCM changed")
    run = json.loads(args.dar_run.read_text())
    if not run["success"] or run.get("restore_errors") or Path(run["adm"]).resolve() != adm.resolve() or (
        run["baseline_settings_sha256"] != run["settings_sha256_after"]
    ):
        raise ValueError("Dolby export or state restoration incomplete")
    mapping = json.loads(args.channel_map.read_text())["layouts"]
    source = read_multichannel_adm(adm, case["num_channels"], case["duration_samples"])
    starts = case["signal_start_samples"]
    frames = case["burst_samples"]
    a = source[starts[0]:starts[0] + frames, case["input_object_channels"][0]]
    b = source[starts[1]:starts[1] + frames, case["input_object_channels"][1]]
    joint = source[starts[2]:starts[2] + frames, case["input_object_channels"][0]] + (
        source[starts[2]:starts[2] + frames, case["input_object_channels"][1]]
    )
    report = {"case_id": case["case_id"], "adm_sha256": case["sha256"],
              "source_superposition_relative_l2": relative_error(joint, a + b), "layouts": {}}
    for artifact in run["outputs"]:
        layout = artifact["layout"]
        wav = Path(artifact["path"])
        if file_sha256(wav) != artifact["sha256"]:
            raise ValueError("Dolby WAV changed")
        y = canonical_pcm(decode_wav(wav, artifact["channels"], case["duration_samples"]),
                          layout, mapping[layout], False).astype(np.float64)
        first, last = 24_000, frames - 4800
        y0 = y[starts[0] + first:starts[0] + last]
        y1 = y[starts[1] + first:starts[1] + last]
        y2 = y[starts[2] + first:starts[2] + last]
        row = {"wav": str(wav.resolve()), "sha256": artifact["sha256"],
               "two_object_sum_relative_pcm_l2": relative_error(y2, y0 + y1)}
        if args.single_object_reference:
            single = canonical_pcm(decode_wav(args.single_object_reference, artifact["channels"],
                                              case["duration_samples"]),
                                   layout, mapping[layout], False).astype(np.float64)
            row["slot_change_relative_pcm_l2"] = [
                relative_error(y[starts[index] + first:starts[index] + last],
                               single[starts[index] + first:starts[index] + last])
                for index in range(3)
            ]
        report["layouts"][layout] = row
    args.output.write_text(json.dumps(report, indent=2) + "\n")
    print(args.output)


if __name__ == "__main__":
    main()
