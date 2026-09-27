#!/usr/bin/env python3
"""Release CLI acceptance for the integrated bed route and mixed Objects DSP."""

import argparse
import gzip
import hashlib
import json
import subprocess
import tempfile
from pathlib import Path

import numpy as np

from gain_trace_adm import inspect_adm
from make_semantic_suite import chunks, make_case, validate_probe_topology, write_riff
from measure_point_suite import decode_wav
from measure_size_field import canonical_pcm
from measure_static_bank import file_sha256, read_multichannel_adm
from run_semantic_suite import export_reference, read_json
from run_compat_suite import validate_reference
from score_semantic_pcm import score
from trace_bed_semantics import audit_exports, compact, difference, encode24, source_signal

HERE = Path(__file__).resolve().parent
ROOT = HERE.parents[2]
BINARY = ROOT / "build/release/mradm"


def materialize(path):
    if not path.exists():
        data = gzip.decompress(path.with_suffix(path.suffix + ".gz").read_bytes())
        path.write_bytes(data)
    return path


def render(adm, directory, layout, mapping, tag="full", policy=None, window=None):
    wav = directory / f"{layout}-{tag}.wav"
    semantic = directory / f"{layout}-{tag}-semantic.json"
    command = [str(BINARY), "render", "-i", str(adm), "-o", str(wav), "--renderer", "saf",
               "--speaker-panner", "room-compat", "--output-layout", layout, "--no-peak-limit",
               "--output-bit-depth", "f32", "--write-semantic-report", str(semantic)]
    if policy is not None:
        policy_path = directory / f"{tag}-policy.json"
        policy_path.write_text(json.dumps(policy, indent=2) + "\n")
        command += ["--semantic-policy", str(policy_path)]
    frames = 240000
    if window is not None:
        start, end = window
        command += ["--start", str(start / 48000), "--end", str(end / 48000)]
        frames = end - start
    with (directory / f"{layout}-{tag}.log").open("w") as log:
        subprocess.run(command, stdout=log, stderr=subprocess.STDOUT, check=True)
    pcm = canonical_pcm(decode_wav(wav, mapping[layout]["channels"], frames), layout, mapping[layout], True)
    report = read_json(semantic)
    if report["renderer_effective"]["status"] != "prepared":
        raise ValueError("render did not publish prepared semantics")
    return pcm


def mixed_case(template, directory, spec, include_bed):
    manifest = make_case(template, directory, spec)
    adm = Path(manifest["adm"])
    parts = chunks(adm)
    source = read_multichannel_adm(adm, 11, 240000)
    if include_bed:
        bed, _ = source_signal("prbs")
        source[:, :10] = bed[:, :10]
    output = [(kind, encode24(source) if kind == b"data" else data) for kind, data in parts]
    validate_probe_topology(output, 11, dict(parts)[b"dbmd"])
    write_riff(adm, output)
    manifest["sha256"] = file_sha256(adm)
    manifest["identity"] = inspect_adm(adm)
    manifest["mixed_bed"] = include_bed
    (directory / "case.json").write_text(json.dumps(manifest, indent=2) + "\n")
    return adm, manifest, source


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--reference-root", type=Path, required=True)
    parser.add_argument("--channel-map", type=Path, required=True)
    parser.add_argument("--output-dir", type=Path, required=True)
    args = parser.parse_args()
    baseline = args.reference_root.resolve()
    root = args.output_dir.resolve()
    root.mkdir(parents=True, exist_ok=True)
    mapping = read_json(args.channel_map)["layouts"]
    profile = read_json(HERE / "renderer55_gain_profile.json")
    report = {"build": "Release", "binary_sha256": file_sha256(BINARY), "static": [], "mixed": [],
              "fixed_delay_samples": 0, "global_level": 1, "reference_root": str(baseline),
              "reference_summary_sha256": file_sha256(baseline / "summary.json"),
              "channel_map_sha256": file_sha256(args.channel_map),
              "driver_sha256": file_sha256(Path(__file__)),
              "scorer_sha256": file_sha256(HERE / "score_semantic_pcm.py")}
    for case in read_json(baseline / "summary.json")["cases"]:
        name = case["specification"]["id"]
        directory = root / name
        directory.mkdir(exist_ok=True)
        with tempfile.TemporaryDirectory(prefix="bed-source-", dir=root) as temporary:
            original = baseline / name / "input.wav"
            data = original.read_bytes() if original.exists() else gzip.decompress(original.with_suffix(".wav.gz").read_bytes())
            if hashlib.sha256(data).hexdigest() != case["adm_sha256"]:
                raise ValueError("static final ADM hash mismatch")
            adm = Path(temporary) / "input.wav"
            adm.write_bytes(data)
            if not case["accepted"]:
                command = [str(BINARY), "render", "-i", str(adm), "-o", str(directory / "rejected.wav"),
                           "--renderer", "saf", "--speaker-panner", "room-compat", "--output-layout", "9.1.6",
                           "--write-semantic-report", str(directory / "rejection.json")]
                result = subprocess.run(command, capture_output=True, text=True)
                rejected = read_json(directory / "rejection.json")
                if not result.returncode or rejected["renderer_effective"]["status"] != "unsupported":
                    raise ValueError("invalid bed label binding was not explicitly rejected")
                report["static"].append({"case": name, "unsupported": True, "passes": True})
                continue
            with np.load(baseline / name / "reference-pcm.npz") as archive:
                for layout in mapping:
                    expected = archive[layout]
                    if hashlib.sha256(expected.astype("<f4").tobytes()).hexdigest() != case["layouts"][layout]["decoded_canonical_pcm_sha256"]:
                        raise ValueError("retained static reference changed")
                    actual = render(adm, directory, layout, mapping)
                    result = difference(actual, expected)
                    row = {"case": name, "layout": layout, "adm_sha256": file_sha256(adm),
                           "difference": result, "passes": result["max_abs_pcm"] <= 2e-7}
                    if name == "prbs":
                        cropped = render(adm, directory, layout, mapping, "crop", window=(32767, 144511))
                        repeated = render(adm, directory, layout, mapping, "repeat")
                        row["crop_exact"] = bool(np.array_equal(cropped, actual[32767:144511]))
                        row["repeat_exact"] = bool(np.array_equal(repeated, actual))
                        row["passes"] &= row["crop_exact"] and row["repeat_exact"]
                    if name == "object_gain_0":
                        policy = {"schema": "mradm.semantic-policy.v1", "objects": [
                            {"id": "AO_1001", "gain": {"scale": .5, "mute": False}}]}
                        controlled = render(adm, directory, layout, mapping, "user-half", policy=policy)
                        row["user_gain_on_native_zero"] = difference(controlled, actual * .5)
                        row["passes"] &= row["user_gain_on_native_zero"]["max_abs_pcm"] <= 2e-7
                    if name == "block_gain_0":
                        for label, gain, indices, factor in (("RC_Lts", {"scale": .5, "mute": False},
                                                              [8, 10] if layout == "7.1.4" else [12], .5),
                                                             ("RC_LFE", {"mute": True}, [3], 0)):
                            policy = {"schema": "mradm.semantic-policy.v1", "objects": [
                                {"id": "AO_1001", "direct_speakers": {"speaker_label": label, "gain": gain}}]}
                            controlled = render(adm, directory, layout, mapping, "channel-" + label, policy=policy)
                            wanted = actual.copy()
                            wanted[:, indices] *= factor
                            row[label + "_user_override"] = difference(controlled, wanted)
                            row["passes"] &= row[label + "_user_override"]["max_abs_pcm"] <= 2e-7
                    report["static"].append(row)
        print(name, "verified", flush=True)
        (root / "report.json").write_text(json.dumps(report, indent=2) + "\n")
    blocks = [{"rtime": 0, "duration": 48031, "size": .01, "xyz": [.3, .6, .2]},
              {"rtime": 48031, "duration": 48000, "size": .25, "xyz": [-.4, -.5, .7]},
              {"rtime": 96031, "duration": 48000, "size": 1, "xyz": [0, 0, 1]},
              {"rtime": 144031, "duration": 95969, "size": 0, "xyz": [0, 0, 0]}]
    specifications = [({"id": "mixed_point", "size": 0}, True),
                      ({"id": "mixed_size", "size": .25}, True),
                      ({"id": "mixed_motion", "blocks": blocks}, True),
                      ({"id": "object_motion", "blocks": blocks}, False)]
    retained = {}
    for spec, include_bed in specifications:
        directory = root / spec["id"]
        adm, manifest, source = mixed_case(baseline / "template.wav", directory, spec, include_bed)
        if not (directory / "reference/run.json").exists():
            export_reference(adm, directory, profile)
        run = read_json(directory / "reference/run.json")
        for artifact in run["outputs"]:
            materialize(Path(artifact["path"]))
        validate_reference(directory / "reference/run.json", adm, {"7.1.4", "9.1.6"})
        retained[spec["id"]] = {}
        for output in run["outputs"]:
            layout = output["layout"]
            expected = canonical_pcm(decode_wav(Path(output["path"]), output["channels"], 240000), layout, mapping[layout], False)
            actual = render(adm, directory, layout, mapping)
            metrics, arrays = score(source[:, 10], expected, actual, manifest, expected_lfe=source[:, 3])
            row = {"case": spec["id"], "layout": layout, "adm_sha256": file_sha256(adm),
                   "metrics": metrics, "difference": difference(actual, expected), "passes": metrics["passes"]}
            retained[spec["id"]][layout] = (expected, actual)
            np.savez_compressed(directory / f"{layout}-measurements.npz", **arrays)
            if spec["id"] == "mixed_motion":
                cropped = render(adm, directory, layout, mapping, "crop", window=(32767, 144511))
                repeated = render(adm, directory, layout, mapping, "repeat")
                row["crop_exact"] = bool(np.array_equal(cropped, actual[32767:144511]))
                row["repeat_exact"] = bool(np.array_equal(repeated, actual))
                row["passes"] &= row["crop_exact"] and row["repeat_exact"]
            report["mixed"].append(row)
        print(spec["id"], "scored", flush=True)
        (root / "report.json").write_text(json.dumps(report, indent=2) + "\n")
    report["policies"] = []
    directory = root / "mixed_motion"
    adm = directory / "input.wav"
    for layout in mapping:
        reference, full = retained["mixed_motion"][layout]
        object_reference, object_pcm = retained["object_motion"][layout]
        with np.load(baseline / "prbs/reference-pcm.npz") as archive:
            bed_reference = archive[layout]
        row = {"layout": layout, "reference_additivity": difference(reference, bed_reference + object_reference),
               "candidate_additivity": difference(full, bed_reference + object_pcm)}
        for name, gain in (("scale", {"scale": .5}), ("mute", {"mute": True})):
            policy = {"schema": "mradm.semantic-policy.v1", "objects": [
                {"id": "AO_1001", "gain": gain}]}
            actual = render(adm, directory, layout, mapping, "bed-" + name, policy=policy)
            expected = object_pcm if name == "mute" else object_pcm + .5 * bed_reference
            row[name] = difference(actual, expected)
        row["passes"] = all(value["max_abs_pcm"] <= 3e-7 for value in row.values() if isinstance(value, dict))
        report["policies"].append(row)
    report["state_restored"] = audit_exports(root)["all_restored"]
    report["passes"] = report["state_restored"] and all(row["passes"] for group in ("static", "mixed", "policies") for row in report[group])
    (root / "report.json").write_text(json.dumps(report, indent=2) + "\n")
    compact(root)
    if not report["passes"]:
        raise SystemExit("bed integration acceptance failed; inspect report.json")


if __name__ == "__main__":
    main()
