#!/usr/bin/env python3
"""Compare sample-domain object-gain envelopes for one moving ADM object."""

import argparse
import json
from pathlib import Path

import numpy as np

from measure_point_suite import decode_wav
from measure_static_bank import file_sha256, read_multichannel_adm

WINDOW_FRAMES = 64
STEP_FRAMES = 16


def gain_envelope(source: np.ndarray, rendered: np.ndarray) -> tuple[np.ndarray, np.ndarray]:
    x = source.astype(np.float64)
    y = rendered.astype(np.float64)
    product = x[:, None] * y
    cumulative_product = np.vstack([np.zeros((1, y.shape[1])), np.cumsum(product, axis=0)])
    cumulative_energy = np.r_[0.0, np.cumsum(x * x)]
    starts = np.arange(0, len(x) - WINDOW_FRAMES + 1, STEP_FRAMES)
    numerator = cumulative_product[starts + WINDOW_FRAMES] - cumulative_product[starts]
    denominator = cumulative_energy[starts + WINDOW_FRAMES] - cumulative_energy[starts]
    gains = numerator / np.maximum(denominator[:, None], 1e-12)
    return starts + WINDOW_FRAMES // 2, gains


def canonical_dolby(pcm: np.ndarray, mapping: list[dict]) -> np.ndarray:
    result = np.zeros_like(pcm)
    for item in mapping:
        result[:, item["mono_index"]] = pcm[:, item["interleaved_index"]]
    return result


def canonical_project(pcm: np.ndarray, layout: str) -> np.ndarray:
    return pcm[:, [0, 1, 2, 3, 6, 7, 4, 5, 8, 9, 10, 11]] if layout == "7.1.4" else pcm


def window_mean(times: np.ndarray, gains: np.ndarray, first: int, last: int) -> np.ndarray:
    selected = gains[(times >= first) & (times < last)]
    if selected.size == 0:
        raise ValueError("motion window contains no gain estimates")
    return np.mean(selected, axis=0)


def event_score(times: np.ndarray, reference: np.ndarray, candidate: np.ndarray, at: int) -> dict:
    pre_ref = window_mean(times, reference, at - 1440, at - 480)
    post_ref = window_mean(times, reference, at + 960, at + 1920)
    pre_candidate = window_mean(times, candidate, at - 1440, at - 480)
    delta = post_ref - pre_ref
    changing = float(np.linalg.norm(delta))
    selection = (times >= at - 240) & (times < at + 2400)
    ref = reference[selection]
    test = candidate[selection]
    relative_rmse = float(np.linalg.norm(test - ref) / max(np.linalg.norm(ref), 1e-12))
    onset_error = None
    if changing > 0.02:
        moment = times[selection]
        denominator = float(delta @ delta)
        ref_progress = ((ref - pre_ref) @ delta) / denominator
        test_progress = ((test - pre_candidate) @ delta) / denominator
        ref_crossings = np.flatnonzero(ref_progress >= 0.1)
        test_crossings = np.flatnonzero(test_progress >= 0.1)
        if ref_crossings.size and test_crossings.size:
            onset_error = int(moment[test_crossings[0]] - moment[ref_crossings[0]])
    return {"sample": at, "change_norm": changing, "envelope_relative_rmse": relative_rmse,
            "onset_error_samples": onset_error}


def main() -> None:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--suite-manifest", type=Path, required=True)
    parser.add_argument("--case", required=True)
    parser.add_argument("--dar-run", type=Path, required=True)
    parser.add_argument("--channel-map", type=Path, required=True)
    parser.add_argument("--candidate-714", type=Path, required=True)
    parser.add_argument("--candidate-916", type=Path, required=True)
    parser.add_argument("--output", type=Path, required=True)
    args = parser.parse_args()
    if args.output.exists():
        parser.error("output exists")
    suite = json.loads(args.suite_manifest.read_text())
    case = next((item for item in suite["cases"] if item["case_id"] == args.case), None)
    if case is None or "events" not in case:
        parser.error("case is not a motion probe")
    adm = Path(case["adm"])
    if file_sha256(adm) != case["sha256"]:
        raise ValueError("ADM differs from suite manifest")
    run = json.loads(args.dar_run.read_text())
    if not run["success"] or Path(run["adm"]).resolve() != adm.resolve():
        raise ValueError("Dolby run did not export this ADM")
    mapping = json.loads(args.channel_map.read_text())["layouts"]
    frames = case["duration_samples"]
    x = read_multichannel_adm(adm, 11, frames)[:, case["input_object_channel"]]
    report = {"case_id": args.case, "adm_sha256": case["sha256"], "layouts": {}}
    for artifact in run["outputs"]:
        layout = artifact["layout"]
        channels = artifact["channels"]
        path = Path(artifact["path"])
        if file_sha256(path) != artifact["sha256"]:
            raise ValueError("Dolby WAV changed")
        original = decode_wav(path, channels, frames)
        reference = canonical_dolby(original, mapping[layout]["interleaved_to_mono"])
        candidate_path = args.candidate_714 if layout == "7.1.4" else args.candidate_916
        candidate = canonical_project(decode_wav(candidate_path, channels, frames), layout)
        times, ref_gains = gain_envelope(x, reference)
        candidate_times, test_gains = gain_envelope(x, candidate)
        if not np.array_equal(times, candidate_times):
            raise ValueError("gain-envelope grids differ")
        events = [event_score(times, ref_gains, test_gains, event["start_samples"])
                  for event in case["events"] if event["start_samples"] >= 1440
                  and event["start_samples"] + 2400 < frames]
        onsets = [abs(item["onset_error_samples"]) for item in events if item["onset_error_samples"] is not None]
        report["layouts"][layout] = {
            "events": events,
            "max_envelope_relative_rmse": max(item["envelope_relative_rmse"] for item in events),
            "max_onset_error_samples": max(onsets) if onsets else None,
            "whole_program_relative_pcm_error": float(np.linalg.norm(candidate.astype(np.float64)
                                                                     - reference.astype(np.float64))
                                                       / max(np.linalg.norm(reference), 1e-12)),
        }
    args.output.write_text(json.dumps(report, indent=2) + "\n")
    print(args.output)


if __name__ == "__main__":
    main()
