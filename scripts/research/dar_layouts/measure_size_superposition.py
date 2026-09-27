#!/usr/bin/env python3
"""Test whether a sized object's output for A+B equals the sum of A and B."""

import argparse
import hashlib
import json
import math
import wave
from pathlib import Path

import numpy as np

from measure_point_suite import decode_wav
from measure_size_field import canonical_pcm, spectral_field
from measure_static_bank import file_sha256, read_multichannel_adm


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
    if args.output.exists():
        parser.error("output exists")
    suite = json.loads(args.suite_manifest.read_text())
    case = next((item for item in suite["cases"] if item["case_id"] == args.case), None)
    if case is None or "signal_start_samples" not in case:
        parser.error("case is not a superposition probe")
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
        raise ValueError("Dolby export or state restoration is incomplete")
    mapping = json.loads(args.channel_map.read_text())["layouts"]
    source = read_multichannel_adm(adm, case["num_channels"], case["duration_samples"])
    report = {"case_id": case["case_id"], "adm_sha256": case["sha256"],
              "pcm_sha256": case["pcm_sha256"], "layouts": {}}
    starts = case["signal_start_samples"]
    frames = case["burst_samples"]
    x = [source[start:start + frames, case["input_object_channel"]].astype(np.float64)
         for start in starts]
    source_error = relative_error(x[2], x[0] + x[1])
    if source_error > 5e-6:
        raise ValueError("final ADM does not contain the intended A+B source")
    for artifact in run["outputs"]:
        layout = artifact["layout"]
        wav = Path(artifact["path"])
        if file_sha256(wav) != artifact["sha256"]:
            raise ValueError("Dolby WAV changed")
        rendered = canonical_pcm(decode_wav(wav, artifact["channels"], case["duration_samples"]),
                                 layout, mapping[layout], False).astype(np.float64)
        y = [rendered[start:start + frames] for start in starts]
        first, last = 24_000, frames - 4800
        observed = y[2][first:last]
        prediction = y[0][first:last] + y[1][first:last]
        source_pcm = x[2][first:last]
        band_observed, covariance_observed, _ = spectral_field(source_pcm, observed)
        band_prediction, covariance_prediction, _ = spectral_field(source_pcm, prediction)
        reference_share = np.mean(observed * observed, axis=0)
        predicted_share = np.mean(prediction * prediction, axis=0)
        active = band_observed >= band_observed.sum(axis=1, keepdims=True) * 1e-4
        covariance_error = (
            np.linalg.norm(covariance_observed - covariance_prediction, axis=(1, 2)) /
            np.maximum(np.linalg.norm(covariance_observed, axis=(1, 2)), 1e-12)
        )
        report["layouts"][layout] = {
            "wav": str(wav.resolve()), "sha256": artifact["sha256"],
            "source_superposition_relative_l2": source_error,
            "speaker_pcm_superposition_relative_l2": relative_error(observed, prediction),
            "normalized_energy_share_l2": relative_error(reference_share / reference_share.sum(),
                                                          predicted_share / predicted_share.sum()),
            "total_power_error_db": 10.0 * math.log10(float(predicted_share.sum() /
                                                            reference_share.sum())),
            "max_active_band_power_error_db": float(np.max(np.abs(10.0 * np.log10(
                np.maximum(band_prediction[active], 1e-30) /
                np.maximum(band_observed[active], 1e-30)
            )))),
            "max_band_covariance_l2": float(np.max(covariance_error)),
        }
    args.output.write_text(json.dumps(report, indent=2) + "\n")
    print(args.output)


if __name__ == "__main__":
    main()
