#!/usr/bin/env python3
"""Apply the frozen size power, spectrum, correlation and tail criteria."""

import argparse
import json
import math
from pathlib import Path

import numpy as np


def relative_error(reference: np.ndarray, observed: np.ndarray) -> float:
    return float(np.linalg.norm(observed - reference) / max(np.linalg.norm(reference), 1e-12))


def main() -> None:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--field-report", type=Path, required=True)
    parser.add_argument("--output", type=Path, required=True)
    args = parser.parse_args()
    if args.output.exists():
        parser.error("output exists")
    field = json.loads(args.field_report.read_text())
    if not field.get("candidates"):
        raise ValueError("field report has no candidate PCM")
    spectra = np.load(field["spectral_archive"])
    report = {"case_id": field["case_id"], "adm_sha256": field["adm_sha256"], "layouts": {}}
    for layout, reference in field["layouts"].items():
        candidate = field["candidates"].get(layout)
        if candidate is None or len(reference["objects"]) != len(candidate["objects"]):
            raise ValueError(f"missing candidate or object mismatch for {layout}")
        rows = []
        for expected, observed in zip(reference["objects"], candidate["objects"]):
            if (expected["label"], expected["xyz"], expected["size"]) != (
                observed["label"], observed["xyz"], observed["size"]
            ):
                raise ValueError("reference and candidate describe different objects")
            power_db = 10.0 * math.log10(observed["total_power"] / expected["total_power"])
            energy_error = relative_error(np.array(expected["normalized_energy_share"]),
                                          np.array(observed["normalized_energy_share"]))
            ref_band = spectra[f"{expected['spectrum_key']}_band_power"]
            test_band = spectra[f"{observed['spectrum_key']}_band_power"]
            active = ref_band >= np.sum(ref_band, axis=1, keepdims=True) * 1e-4
            active_db = 10.0 * np.log10(np.maximum(test_band[active], 1e-30) /
                                         np.maximum(ref_band[active], 1e-30))
            ref_cov = spectra[f"{expected['spectrum_key']}_covariance"].astype(np.complex128)
            test_cov = spectra[f"{observed['spectrum_key']}_covariance"].astype(np.complex128)
            covariance_error = (
                np.linalg.norm(test_cov - ref_cov, axis=(1, 2)) /
                np.maximum(np.linalg.norm(ref_cov, axis=(1, 2)), 1e-12)
            )
            ref_tail = np.array(expected["tail_decay_db_1ms"])
            test_tail = np.array(observed["tail_decay_db_1ms"])
            if ref_tail.shape != test_tail.shape:
                raise ValueError("reference and candidate tails have different lengths")
            if "tail_measurable_1ms" not in expected:
                raise ValueError("re-measure tail noise floor before scoring this report")
            measurable = np.array(expected["tail_measurable_1ms"], dtype=bool)
            tail_curve_error = float(np.max(np.abs(test_tail[measurable] - ref_tail[measurable]))) if np.any(
                measurable
            ) else None
            reference_tail_power = expected["tail_total_power_ratio"]
            observed_tail_power = observed["tail_total_power_ratio"]
            noise_floor = np.array(expected["tail_noise_energy_ratio_1ms"])
            candidate_remaining = np.array(observed["tail_cumulative_power_ratio_1ms"])
            below_floor = np.flatnonzero(~measurable)
            extra_tail_ok = not len(below_floor) or bool(np.all(candidate_remaining[below_floor] <= noise_floor[below_floor]))
            # Classification must use the same measured floor as the curve.
            # A quiet but measurable reference tail is not an absent tail.
            total_noise_floor = float(noise_floor[0]) if len(noise_floor) else 0.0
            if reference_tail_power > total_noise_floor:
                tail_power_error_db = 10.0 * math.log10(max(observed_tail_power, 1e-30) / reference_tail_power)
                tail_power_ok = abs(tail_power_error_db) <= 1.0
            else:
                tail_power_error_db = None
                tail_power_ok = observed_tail_power <= total_noise_floor
            lfe_power = observed["speaker_energy"][3]
            passed = (energy_error <= 0.05 and abs(power_db) <= 0.1 and
                      np.max(np.abs(active_db)) <= 0.5 and np.max(covariance_error) <= 0.05 and
                      (tail_curve_error is None or tail_curve_error <= 1.0) and tail_power_ok and extra_tail_ok and
                      lfe_power <= 1e-12 and np.isfinite(np.array(observed["speaker_energy"])).all())
            rows.append({"label": expected["label"], "size": expected["size"],
                         "normalized_energy_share_l2": energy_error,
                         "total_power_error_db": power_db,
                         "max_active_band_power_error_db": float(np.max(np.abs(active_db))),
                         "max_band_covariance_l2": float(np.max(covariance_error)),
                         "max_tail_curve_error_db": tail_curve_error,
                         "tail_total_power_error_db": tail_power_error_db,
                         "extra_tail_below_reference_floor_ok": extra_tail_ok,
                         "lfe_power": lfe_power, "passes": bool(passed)})
        report["layouts"][layout] = {"objects": rows, "passes": all(row["passes"] for row in rows)}
    report["passes_both_layouts"] = all(item["passes"] for item in report["layouts"].values())
    args.output.write_text(json.dumps(report, indent=2) + "\n")
    print(args.output)


if __name__ == "__main__":
    main()
