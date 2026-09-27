#!/usr/bin/env python3
"""Compare equal PRBS bursts after changing level or absolute render time."""

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


def relative_error(a: np.ndarray, b: np.ndarray) -> float:
    return float(np.linalg.norm(a - b) / max(np.linalg.norm(a), 1e-12))


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
    if case is None or "segments" not in case or "pcm_sha256" not in case:
        parser.error("case is not a size transfer probe")
    adm = Path(case["adm"])
    if file_sha256(adm) != case["sha256"]:
        raise ValueError("final ADM differs from manifest")
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
              "pcm_sha256": case["pcm_sha256"], "fixed_delay_samples": 0,
              "global_level": 1.0, "layouts": {}}
    for artifact in run["outputs"]:
        layout = artifact["layout"]
        wav = Path(artifact["path"])
        if file_sha256(wav) != artifact["sha256"]:
            raise ValueError("Dolby WAV changed")
        rendered = canonical_pcm(decode_wav(wav, artifact["channels"], case["duration_samples"]),
                                 layout, mapping[layout], False).astype(np.float64)
        bursts = []
        for segment in case["segments"]:
            start = segment["start_samples"]
            end = start + case["burst_samples"]
            xx = source[start:end, case["input_object_channel"]].astype(np.float64)
            yy = rendered[start:end]
            first, last = 24_000, len(xx) - 4800
            xx = xx[first:last]
            yy = yy[first:last]
            amplitude = math.sqrt(float(np.mean(xx * xx)))
            power = np.mean(yy * yy, axis=0) / (amplitude * amplitude)
            band_power, covariance, coherence = spectral_field(xx, yy)
            bursts.append({"segment": segment, "source": xx / amplitude,
                           "rendered": yy / amplitude, "power": power,
                           "band_power": band_power / (amplitude * amplitude),
                           "covariance": covariance, "coherence": coherence})
        reference = bursts[0]
        rows = []
        for item in bursts[1:]:
            p0 = reference["power"]
            p1 = item["power"]
            active = p0 > float(np.sum(p0)) * 1e-4
            spectral_db = 10.0 * np.log10(np.maximum(item["band_power"][:, active], 1e-20) /
                                            np.maximum(reference["band_power"][:, active], 1e-20))
            covariance_error = (
                np.linalg.norm(item["covariance"] - reference["covariance"], axis=(1, 2)) /
                np.maximum(np.linalg.norm(reference["covariance"], axis=(1, 2)), 1e-12)
            )
            rows.append({"level_dbfs": item["segment"]["level_dbfs"],
                         "relative_start_shift_samples": (item["segment"]["start_samples"] % 240_000) -
                         (reference["segment"]["start_samples"] % 240_000),
                         "absolute_start_sample": item["segment"]["start_samples"],
                         "normalized_source_pcm_l2": relative_error(reference["source"], item["source"]),
                         "normalized_output_pcm_l2": relative_error(reference["rendered"], item["rendered"]),
                         "normalized_energy_share_l2": relative_error(p0 / p0.sum(), p1 / p1.sum()),
                         "relative_total_power_db": 10.0 * math.log10(float(p1.sum() / p0.sum())),
                         "max_active_band_power_difference_db": float(np.max(np.abs(spectral_db))),
                         "mean_band_covariance_l2": float(np.mean(covariance_error)),
                         "max_band_covariance_l2": float(np.max(covariance_error))})
        report["layouts"][layout] = {"wav": str(wav.resolve()), "sha256": artifact["sha256"],
                                      "comparisons": rows}
    args.output.write_text(json.dumps(report, indent=2) + "\n")
    print(args.output)


if __name__ == "__main__":
    main()
