#!/usr/bin/env python3
"""Subtract identical PRBS bursts to reveal the active size impulse response."""

import argparse
import hashlib
import json
import wave
from pathlib import Path

import numpy as np

from measure_point_suite import decode_wav
from measure_size_field import canonical_pcm
from measure_static_bank import file_sha256, read_multichannel_adm

PRE = 1024
POST = 8192


def relative_error(reference: np.ndarray, observed: np.ndarray) -> float:
    return float(np.linalg.norm(observed - reference) / max(np.linalg.norm(reference), 1e-12))


def main() -> None:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--suite-manifest", type=Path, required=True)
    parser.add_argument("--case", required=True)
    parser.add_argument("--dar-run", type=Path, required=True)
    parser.add_argument("--channel-map", type=Path, required=True)
    parser.add_argument("--isolated-response", type=Path,
                        help="Optional NPZ from size_impulse_identification for a cold-versus-warm comparison")
    parser.add_argument("--output", type=Path, required=True)
    args = parser.parse_args()
    if args.output.exists() or args.output.with_suffix(".npz").exists():
        parser.error("output or response archive exists")
    suite = json.loads(args.suite_manifest.read_text())
    case = next((item for item in suite["cases"] if item["case_id"] == args.case), None)
    if case is None or "pulses" not in case or "pcm_sha256" not in case:
        parser.error("case is not a warm impulse probe")
    adm = Path(case["adm"])
    if file_sha256(adm) != case["sha256"]:
        raise ValueError("ADM differs from manifest")
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
    cold = np.load(args.isolated_response) if args.isolated_response else None
    report = {"case_id": case["case_id"], "adm_sha256": case["sha256"], "layouts": {}}
    arrays = {}
    starts = case["signal_start_samples"]
    frames = case["burst_samples"]
    for artifact in run["outputs"]:
        layout = artifact["layout"]
        wav = Path(artifact["path"])
        if file_sha256(wav) != artifact["sha256"]:
            raise ValueError("Dolby WAV changed")
        pcm = canonical_pcm(decode_wav(wav, artifact["channels"], case["duration_samples"]),
                            layout, mapping[layout], False).astype(np.float64)
        control = pcm[starts[0]:starts[0] + frames]
        probe = pcm[starts[1]:starts[1] + frames]
        difference = probe - control
        source_control = source[starts[0]:starts[0] + frames, case["input_object_channel"]]
        source_probe = source[starts[1]:starts[1] + frames, case["input_object_channel"]]
        source_delta = source_probe - source_control
        expected_positions = [pulse["sample"] - starts[1] for pulse in case["pulses"]]
        if not np.array_equal(np.flatnonzero(source_delta), expected_positions):
            raise ValueError("final ADM probe differs from control beyond the impulses")
        rows = []
        for index, pulse in enumerate(case["pulses"]):
            onset = expected_positions[index]
            amplitude = float(source_delta[onset])
            response = difference[onset - PRE:onset + POST] / amplitude
            pre_power = float(np.sum(response[:PRE] ** 2))
            post_power = float(np.sum(response[PRE:] ** 2))
            row = {"sample": pulse["sample"], "level": pulse["amplitude"],
                   "source_level_exact": amplitude,
                   "signed_direct_gain": response[PRE].tolist(),
                   "pre_response_energy_ratio": pre_power / max(post_power, 1e-12),
                   "tail_energy_fraction_after_1ms": float(np.sum(response[PRE + 48:] ** 2) /
                                                           max(post_power, 1e-12)),
                   "tail_energy_fraction_after_20ms": float(np.sum(response[PRE + 960:] ** 2) /
                                                            max(post_power, 1e-12))}
            if cold is not None:
                key = f"{layout.replace('.', '')}_1_first"
                isolated = cold[key][:PRE + POST]
                row["cold_response_relative_l2"] = relative_error(isolated, response)
            arrays[f"{layout.replace('.', '')}_{index}"] = response.astype(np.float32)
            rows.append(row)
        report["layouts"][layout] = {"wav": str(wav.resolve()), "sha256": artifact["sha256"],
                                      "warm_responses": rows,
                                      "response_changes_l2": [relative_error(arrays[f"{layout.replace('.', '')}_0"],
                                                                            arrays[f"{layout.replace('.', '')}_{j}"])
                                                              for j in range(1, len(rows))]}
    archive = args.output.with_suffix(".npz")
    np.savez_compressed(archive, **arrays)
    report["response_archive"] = str(archive.resolve())
    args.output.write_text(json.dumps(report, indent=2) + "\n")
    print(args.output)


if __name__ == "__main__":
    main()
