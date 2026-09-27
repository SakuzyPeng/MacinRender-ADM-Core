#!/usr/bin/env python3
"""Measure sized-object energy envelopes and channel routing across ADM events."""

import argparse
import json
from pathlib import Path

import numpy as np

from measure_point_suite import decode_wav
from measure_size_field import canonical_pcm
from measure_static_bank import file_sha256, read_multichannel_adm

WINDOW = 960  # 20 ms at 48 kHz
HOP = 48      # 1 ms


def energy_envelope(x: np.ndarray, y: np.ndarray) -> tuple[np.ndarray, np.ndarray]:
    squared_input = np.r_[0.0, np.cumsum(x.astype(np.float64) ** 2)]
    squared_output = np.vstack([np.zeros((1, y.shape[1])),
                                np.cumsum(y.astype(np.float64) ** 2, axis=0)])
    starts = np.arange(0, len(x) - WINDOW + 1, HOP)
    input_energy = squared_input[starts + WINDOW] - squared_input[starts]
    speaker_energy = squared_output[starts + WINDOW] - squared_output[starts]
    return starts + WINDOW // 2, speaker_energy / np.maximum(input_energy[:, None], 1e-12)


def event_metrics(times: np.ndarray, powers: np.ndarray, events: list[dict], layout: str) -> list[dict]:
    rows = []
    for index, event in enumerate(events):
        at = event["start_samples"]
        stable = powers[(times >= at + 24_000) & (times < min(at + 43_200, len(powers) * HOP))]
        if len(stable) == 0:
            raise ValueError(f"no stable interval after size event {index}")
        averaged = np.mean(stable, axis=0)
        total = float(np.sum(averaged))
        if total <= 1e-10:
            raise ValueError(f"size event {index} is silent")
        row = {"start_sample": at, "xyz": event["position"], "size": event.get("size", 0.0),
               "stable_total_power": total, "stable_speaker_energy": averaged.tolist(),
               "stable_normalized_energy_share": (averaged / total).tolist(),
               "stable_lfe_power": float(averaged[3])}
        if layout == "9.1.6":
            row["extra_wide_and_top_middle_power"] = float(np.sum(averaged[[8, 9, 12, 13]]))
        if index:
            previous = powers[(times >= at - 12_000) & (times < at - 2400)]
            next_stable = np.mean(stable, axis=0)
            before = np.mean(previous, axis=0)
            delta = next_stable - before
            norm_squared = float(delta @ delta)
            if norm_squared > 1e-6:
                mask = (times >= at - 2400) & (times <= at + 24_000)
                progress = ((powers[mask] - before) @ delta) / norm_squared
                crossing = np.flatnonzero(progress >= 0.1)
                row["observed_change_onset_sample"] = int(times[mask][crossing[0]]) if len(crossing) else None
            else:
                row["observed_change_onset_sample"] = None
        rows.append(row)
    return rows


def main() -> None:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--suite-manifest", type=Path, required=True)
    parser.add_argument("--case", required=True)
    parser.add_argument("--dar-run", type=Path, required=True)
    parser.add_argument("--channel-map", type=Path, required=True)
    parser.add_argument("--candidate-714", type=Path)
    parser.add_argument("--candidate-916", type=Path)
    parser.add_argument("--output", type=Path, required=True)
    args = parser.parse_args()
    if args.output.exists() or args.output.with_suffix(".npz").exists():
        parser.error("output or energy-envelope archive exists")
    suite = json.loads(args.suite_manifest.read_text())
    case = next((item for item in suite["cases"] if item["case_id"] == args.case), None)
    if case is None or "events" not in case or not all("size" in event for event in case["events"]):
        parser.error("case is not a size-motion probe")
    adm = Path(case["adm"])
    if file_sha256(adm) != case["sha256"]:
        raise ValueError("final ADM differs from manifest")
    run = json.loads(args.dar_run.read_text())
    if not run["success"] or run.get("restore_errors") or Path(run["adm"]).resolve() != adm.resolve() or (
        run["baseline_settings_sha256"] != run["settings_sha256_after"]
    ):
        raise ValueError("Dolby export or state restoration is incomplete")
    mapping = json.loads(args.channel_map.read_text())["layouts"]
    frames = case["duration_samples"]
    source = read_multichannel_adm(adm, 11, frames)[:, case["input_object_channel"]]
    report = {"case_id": case["case_id"], "adm_sha256": case["sha256"],
              "window_frames": WINDOW, "hop_frames": HOP, "fixed_delay_samples": 0,
              "global_level": 1.0, "layouts": {}, "candidates": {}}
    arrays = {}
    for artifact in run["outputs"]:
        layout = artifact["layout"]
        if file_sha256(Path(artifact["path"])) != artifact["sha256"]:
            raise ValueError("Dolby WAV changed")
        paths = [("layouts", Path(artifact["path"]), False)]
        candidate = args.candidate_714 if layout == "7.1.4" else args.candidate_916
        if candidate is not None:
            paths.append(("candidates", candidate, True))
        for group, path, is_candidate in paths:
            pcm = canonical_pcm(decode_wav(path, artifact["channels"], frames),
                                layout, mapping[layout], is_candidate)
            times, energy = energy_envelope(source, pcm)
            key = f"{group}_{layout.replace('.', '')}"
            arrays[f"{key}_time_samples"] = times
            arrays[f"{key}_speaker_power"] = energy.astype(np.float32)
            report[group][layout] = {"wav": str(path.resolve()), "sha256": file_sha256(path),
                                     "events": event_metrics(times, energy, case["events"], layout)}
        if candidate is not None:
            reference = arrays[f"layouts_{layout.replace('.', '')}_speaker_power"]
            observed = arrays[f"candidates_{layout.replace('.', '')}_speaker_power"]
            scores = []
            for event in case["events"][1:]:
                at = event["start_samples"]
                selection = (times >= at - 2400) & (times <= at + 24_000)
                rmse = float(np.linalg.norm(observed[selection] - reference[selection]) /
                             max(np.linalg.norm(reference[selection]), 1e-12))
                ref_event = next(row for row in report["layouts"][layout]["events"]
                                 if row["start_sample"] == at)
                test_event = next(row for row in report["candidates"][layout]["events"]
                                  if row["start_sample"] == at)
                ref_onset, test_onset = (ref_event.get("observed_change_onset_sample"),
                                         test_event.get("observed_change_onset_sample"))
                offset = abs(test_onset - ref_onset) if ref_onset is not None and test_onset is not None else None
                scores.append({"start_sample": at, "energy_envelope_normalized_rmse": rmse,
                               "onset_error_samples": offset,
                               "reference_has_onset": ref_onset is not None,
                               "candidate_has_onset": test_onset is not None})
            report["candidates"][layout]["scores"] = scores
            report["candidates"][layout]["passes_dynamic_thresholds"] = all(
                score["energy_envelope_normalized_rmse"] <= 0.02 and
                (not score["reference_has_onset"] or
                 (score["candidate_has_onset"] and score["onset_error_samples"] <= 48))
                for score in scores
            )
    archive = args.output.with_suffix(".npz")
    np.savez_compressed(archive, **arrays)
    report["envelope_archive"] = str(archive.resolve())
    args.output.write_text(json.dumps(report, indent=2) + "\n")
    print(args.output)


if __name__ == "__main__":
    main()
