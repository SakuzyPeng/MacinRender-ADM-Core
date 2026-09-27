#!/usr/bin/env python3
"""Measure sample-domain impulse responses and time invariance for size probes."""

import argparse
import hashlib
import json
from pathlib import Path

import numpy as np

from measure_point_suite import decode_wav
from measure_static_bank import file_sha256, read_multichannel_adm

PRE_FRAMES = 1024
FIR_FRAMES = 10_000


def canonical_pcm(pcm: np.ndarray, mapping: list[dict]) -> np.ndarray:
    result = np.zeros_like(pcm)
    for item in mapping:
        result[:, item["mono_index"]] = pcm[:, item["interleaved_index"]]
    return result


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
        parser.error("output report or impulse archive already exists")
    suite = json.loads(args.suite_manifest.read_text())
    case = next((item for item in suite["cases"] if item["case_id"] == args.case), None)
    if case is None or "objects" not in case or "pcm_sha256" not in case:
        parser.error("case is not a size impulse probe")
    adm = Path(case["adm"])
    if file_sha256(adm) != case["sha256"]:
        raise ValueError("final ADM differs from manifest")
    run = json.loads(args.dar_run.read_text())
    if not run["success"] or run["restore_errors"] or Path(run["adm"]).resolve() != adm.resolve():
        raise ValueError("Dolby export did not finish and restore the same ADM")
    if run["settings_sha256_after"] != run["baseline_settings_sha256"]:
        raise ValueError("Renderer settings were not restored")
    channel_map = json.loads(args.channel_map.read_text())["layouts"]
    frames = case["duration_samples"]
    source = read_multichannel_adm(adm, case["num_channels"], frames)
    # Verify the authoritative 24-bit PCM payload, not floating-point decoding.
    import wave
    with wave.open(str(adm), "rb") as reader:
        if hashlib.sha256(reader.readframes(frames)).hexdigest() != case["pcm_sha256"]:
            raise ValueError("final ADM PCM changed")
    archive = {}
    report = {"case_id": args.case, "adm_sha256": case["sha256"],
              "pcm_sha256": case["pcm_sha256"], "fixed_delay_samples": 0,
              "global_level": 1.0, "layouts": {}}
    for artifact in run["outputs"]:
        layout = artifact["layout"]
        path = Path(artifact["path"])
        if file_sha256(path) != artifact["sha256"] or not channel_map[layout]["all_samples_exact"]:
            raise ValueError(f"unverified Renderer output or channel map: {layout}")
        rendered = canonical_pcm(decode_wav(path, artifact["channels"], frames).astype(np.float64),
                                 channel_map[layout]["interleaved_to_mono"])
        rows = []
        for index, item in enumerate(case["objects"]):
            first = index * 96_000
            last = first + 96_000
            channel = item["input_channel"]
            pulses = item["pulses"]
            responses = []
            for pulse in pulses:
                at = pulse["sample"]
                amplitude = float(source[at, channel])
                if abs(amplitude - pulse["amplitude"]) > 2 / 8388608:
                    raise ValueError(f"final ADM pulse amplitude changed: {item['label']}")
                if at - PRE_FRAMES < first or at + FIR_FRAMES > last:
                    raise ValueError("pulse does not have a complete measurement window")
                responses.append(rendered[at - PRE_FRAMES:at + FIR_FRAMES] / amplitude)
            h0 = responses[0][PRE_FRAMES:]
            prediction = np.zeros_like(rendered[first:last])
            for pulse in pulses:
                onset = pulse["sample"] - first
                usable = min(FIR_FRAMES, len(prediction) - onset)
                prediction[onset:onset + usable] += float(source[pulse["sample"], channel]) * h0[:usable]
            compare_from = pulses[1]["sample"] - first
            compare_to = pulses[-1]["sample"] - first + FIR_FRAMES
            predicted_error = relative_error(rendered[first + compare_from:first + compare_to],
                                             prediction[compare_from:compare_to])
            direct = responses[-1][PRE_FRAMES:]
            full_tail = rendered[pulses[-1]["sample"]:last] / float(source[pulses[-1]["sample"], channel])
            energy = np.sum(full_tail * full_tail, axis=0)
            total = float(energy.sum())
            if total < 1e-10:
                raise ValueError(f"silent Renderer output for {item['label']}")
            post_1ms = float(np.sum(full_tail[48:] ** 2) / total)
            post_20ms = float(np.sum(full_tail[960:] ** 2) / total)
            post_100ms = float(np.sum(full_tail[4800:] ** 2) / total)
            maximum = float(np.max(np.abs(full_tail)))
            support = np.flatnonzero(np.any(np.abs(full_tail) > maximum * 1e-5, axis=1))
            row = {"label": item["label"], "xyz": item["xyz"], "size": item["size"],
                   "first_sample_after_pulse": int(support[0]) if len(support) else None,
                   "signed_direct_gain": [float(v) for v in direct[0]],
                   "speaker_energy": [float(v) for v in energy],
                   "normalized_energy_share": [float(v / total) for v in energy],
                   "total_energy": total,
                   "tail_energy_fraction_after_1ms": post_1ms,
                   "tail_energy_fraction_after_20ms": post_20ms,
                   "tail_energy_fraction_after_100ms": post_100ms,
                   "repeat_response_relative_l2": [relative_error(responses[0], response)
                                                   for response in responses[1:]],
                   "first_response_predicts_later_pulses_l2": predicted_error}
            archive[f"{layout.replace('.', '')}_{index}_first"] = responses[0].astype(np.float32)
            archive[f"{layout.replace('.', '')}_{index}_last"] = full_tail.astype(np.float32)
            rows.append(row)
        report["layouts"][layout] = {"wav": str(path.resolve()), "sha256": artifact["sha256"],
                                      "objects": rows}
    np.savez_compressed(args.output.with_suffix(".npz"), **archive)
    report["response_archive"] = str(args.output.with_suffix(".npz").resolve())
    args.output.write_text(json.dumps(report, indent=2) + "\n")
    print(args.output)


if __name__ == "__main__":
    main()
