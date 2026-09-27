#!/usr/bin/env python3
"""Recover stable size FIRs at several positions and extents from PRBS subtraction."""

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
    parser.add_argument("--output", type=Path, required=True)
    args = parser.parse_args()
    if args.output.exists() or args.output.with_suffix(".npz").exists():
        parser.error("output or FIR archive exists")
    suite = json.loads(args.suite_manifest.read_text())
    case = next((item for item in suite["cases"] if item["case_id"] == args.case), None)
    if case is None or "points" not in case or "pcm_sha256" not in case:
        parser.error("case is not a warm FIR bank")
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
    report = {"case_id": case["case_id"], "adm_sha256": case["sha256"], "layouts": {}}
    arrays = {}
    frames = case["burst_samples"]
    for artifact in run["outputs"]:
        layout = artifact["layout"]
        wav = Path(artifact["path"])
        if file_sha256(wav) != artifact["sha256"]:
            raise ValueError("Dolby WAV changed")
        pcm = canonical_pcm(decode_wav(wav, artifact["channels"], case["duration_samples"]),
                            layout, mapping[layout], False).astype(np.float64)
        rows = []
        for index, item in enumerate(case["points"]):
            control_start, probe_start = item["control_sample"], item["probe_sample"]
            control = pcm[control_start:control_start + frames]
            probe = pcm[probe_start:probe_start + frames]
            delta = probe - control
            source_control = source[control_start:control_start + frames, case["input_object_channel"]]
            source_probe = source[probe_start:probe_start + frames, case["input_object_channel"]]
            source_delta = source_probe - source_control
            expected_positions = [pulse["sample"] - probe_start for pulse in item["pulses"]]
            if not np.array_equal(np.flatnonzero(source_delta), expected_positions):
                raise ValueError(f"source differs beyond pulses: {item['label']}")
            responses = []
            for pulse in item["pulses"]:
                onset = pulse["sample"] - probe_start
                amp = float(source_delta[onset])
                responses.append(delta[onset - PRE:onset + POST] / amp)
            first, last = responses
            baseline_error = relative_error(control[3000:expected_positions[0] - 1000],
                                            probe[3000:expected_positions[0] - 1000])
            stable_response = last[PRE:]
            output = np.fft.irfft(
                np.fft.rfft(source_control.astype(np.float64), 1 << (frames + POST - 1).bit_length())[:, None] *
                np.fft.rfft(stable_response, 1 << (frames + POST - 1).bit_length(), axis=0),
                1 << (frames + POST - 1).bit_length(), axis=0,
            )[:frames]
            prediction_error = relative_error(control[24_000:frames - 4800], output[24_000:frames - 4800])
            key = f"{layout.replace('.', '')}_{index}"
            arrays[f"{key}_fir"] = stable_response.astype(np.float32)
            arrays[f"{key}_early"] = first[PRE:].astype(np.float32)
            energy = np.sum(stable_response ** 2, axis=0)
            total = float(np.sum(energy))
            rows.append({"label": item["label"], "xyz": item["xyz"], "size": item["size"],
                         "control_probe_pre_pulse_relative_l2": baseline_error,
                         "early_late_impulse_relative_l2": relative_error(first, last),
                         "stable_fir_predicts_independent_prbs_l2": prediction_error,
                         "signed_direct_gain": stable_response[0].tolist(),
                         "speaker_energy": energy.tolist(),
                         "normalized_energy_share": (energy / total).tolist(),
                         "total_energy": total,
                         "tail_fraction_after_1ms": float(np.sum(stable_response[48:] ** 2) / total),
                         "tail_fraction_after_20ms": float(np.sum(stable_response[960:] ** 2) / total),
                         "fir_key": key})
        report["layouts"][layout] = {"wav": str(wav.resolve()), "sha256": artifact["sha256"],
                                      "points": rows}
    archive = args.output.with_suffix(".npz")
    np.savez_compressed(archive, **arrays)
    report["fir_archive"] = str(archive.resolve())
    args.output.write_text(json.dumps(report, indent=2) + "\n")
    print(args.output)


if __name__ == "__main__":
    main()
