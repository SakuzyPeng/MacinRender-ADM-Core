#!/usr/bin/env python3
"""Synthetic 7.1.2 bed routing/field isolation through the normal ADM export path.

The source topology and opaque DBMD stay unchanged. This is a research suite;
it does not enable non-silent beds in the production compatibility backend.
"""

import argparse
import copy
import gzip
import hashlib
import json
import struct
import subprocess
import sys
import tempfile
from pathlib import Path
from xml.etree import ElementTree as ET

import numpy as np

from gain_trace_adm import gain_field, inspect_adm, object_fields, time_field
from make_semantic_suite import (FRAMES, child, chunks, named, prepare_template, set_gain,
                                 set_time, validate_probe_topology, write_riff)
from measure_point_suite import decode_wav
from measure_size_field import canonical_pcm
from measure_static_bank import file_sha256
from run_compat_suite import validate_reference
from run_semantic_suite import export_reference, read_json

HERE = Path(__file__).resolve().parent
ROOT = HERE.parents[2]
LABELS = ["RC_L", "RC_R", "RC_C", "RC_LFE", "RC_Lss", "RC_Rss", "RC_Lrs", "RC_Rrs", "RC_Lts", "RC_Rts"]
SEED = 0xBED260926


def digest_pcm(pcm):
    return hashlib.sha256(pcm.astype("<f4").tobytes()).hexdigest()


def inspect_bed(path):
    identity = inspect_adm(path)
    parts = dict(chunks(path))
    root = ET.fromstring(parts[b"axml"])
    for item in root.iter():
        item.tag = item.tag.split("}")[-1]
    channels = {item.attrib["audioChannelFormatID"]: item for item in root.iter("audioChannelFormat")}
    streams = {item.attrib["audioStreamFormatID"]: item.findtext("audioChannelFormatIDRef")
               for item in root.iter("audioStreamFormat")}
    tracks = {item.attrib["audioTrackFormatID"]: streams[item.findtext("audioStreamFormatIDRef")]
              for item in root.iter("audioTrackFormat")}
    objects = [object_fields(item) for item in root.iter("audioObject")]
    rows = []
    count, uids = struct.unpack_from("<HH", parts[b"chna"])
    if count != 11 or uids != 11 or len(parts[b"chna"]) != 444:
        raise ValueError("bed probes require the verified 10-bed + 1-object topology")
    indices = []
    for index in range(uids):
        pcm, uid, track, pack, _ = struct.unpack_from("<H12s14s11sc", parts[b"chna"], 4 + 40 * index)
        indices.append(pcm)
        clean = lambda value: value.rstrip(b"\0").decode("ascii")
        uid, track, pack = map(clean, (uid, track, pack))
        channel = channels[tracks[track]]
        if channel.attrib.get("typeDefinition") != "DirectSpeakers":
            continue
        owners = [item for item in objects if uid in item["track_uids"]]
        if len(owners) != 1:
            raise ValueError("bed requires one unambiguous owner per PCM binding")
        blocks = []
        for block in channel.findall("audioBlockFormat"):
            blocks.append({"id": block.attrib["audioBlockFormatID"],
                           "labels": [item.text for item in block.findall("speakerLabel")],
                           "cartesian": block.findtext("cartesian"),
                           "position": {item.attrib["coordinate"]: float(item.text)
                                        for item in block.findall("position")},
                           "gain": gain_field(block), "rtime": time_field(block, "rtime", 0),
                           "duration": time_field(block, "duration"),
                           "xml": ET.tostring(block, encoding="unicode")})
        rows.append({"input_channel": pcm - 1, "track_uid": uid, "track_format": track,
                     "pack_format": pack, "channel_format": tracks[track], "owner": owners[0],
                     "blocks": blocks})
    if sorted(indices) != list(range(1, 12)) or len(rows) != 10:
        raise ValueError("bed CHNA does not bind every PCM channel exactly once")
    rows.sort(key=lambda item: item["input_channel"])
    identity["bed"] = rows
    return identity


def source_signal(kind):
    pcm = np.zeros((FRAMES, 11), dtype=np.float64)
    pulses = []
    if kind == "impulses":
        for channel in range(10):
            for offset, amplitude in ((0, .25), (4095, -.25), (8193, .125), (12319, .25)):
                sample = 4096 + channel * 20480 + offset
                pcm[sample, channel] = amplitude
                pulses.append({"input_channel": channel, "sample": sample, "amplitude": amplitude})
    elif kind == "prbs":
        rng = np.random.default_rng(SEED)
        pcm[4096:192000, :10] = (rng.integers(0, 2, size=(192000 - 4096, 10)) * 2 - 1) / 64
    else:
        raise ValueError("unknown bed signal")
    return pcm, pulses


def encode24(pcm):
    values = np.rint(pcm * (1 << 23)).astype(np.int32).reshape(-1)
    if np.any(values < -(1 << 23)) or np.any(values >= (1 << 23)):
        raise ValueError("probe PCM clips")
    return np.column_stack([values & 255, (values >> 8) & 255, (values >> 16) & 255]).astype(np.uint8).tobytes()


def specifications():
    return [{"id": "impulses", "signal": "impulses"}, {"id": "prbs", "signal": "prbs"},
            {"id": "object_gain_0", "object_gain": {"value": 0}},
            {"id": "object_gain_half", "object_gain": {"value": .5}},
            {"id": "block_gain_0", "block_gain": {"value": 0}},
            {"id": "block_gain_half_db", "block_gain": {"value": -6.020599913279624, "unit": "dB"}},
            {"id": "object_mute", "mute": "1"},
            {"id": "object_window", "start": 48000, "duration": 96000},
            {"id": "block_window", "rtime": 48000, "block_duration": 96000},
            {"id": "positions_origin", "positions_origin": True},
            {"id": "labels_swap", "swap": "labels"},
            {"id": "positions_swap", "swap": "positions"},
            {"id": "block_gain_step", "gain_step": True}]


def make_case(template, directory, spec):
    directory.mkdir(parents=True, exist_ok=True)
    parts = chunks(template)
    root = ET.fromstring(dict(parts)[b"axml"])
    if "}" in root.tag:
        ET.register_namespace("", root.tag.split("}")[0][1:])
    bed = [item for item in named(root, "audioChannelFormat")
           if item.attrib.get("typeDefinition") == "DirectSpeakers"]
    if [named(item, "speakerLabel")[0].text for item in bed] != LABELS:
        raise ValueError("source template bed labels/order changed")
    owner = next(item for item in named(root, "audioObject") if item.attrib["audioObjectID"] == "AO_1001")
    if "object_gain" in spec:
        set_gain(owner, spec["object_gain"])
    if "mute" in spec:
        child(owner, "mute", spec["mute"])
    for field in ("start", "duration"):
        if field in spec:
            set_time(owner, field, spec[field])
    for index, channel in enumerate(bed):
        block = named(channel, "audioBlockFormat")[0]
        if "block_gain" in spec:
            set_gain(block, spec["block_gain"])
        if "rtime" in spec:
            set_time(block, "rtime", spec["rtime"])
            set_time(block, "duration", spec["block_duration"])
        if spec.get("positions_origin") and index != 3:
            for position in named(block, "position"):
                position.text = "0"
        if spec.get("gain_step"):
            set_time(block, "rtime", 0)
            set_time(block, "duration", 96031)
            second = copy.deepcopy(block)
            second.set("audioBlockFormatID", block.attrib["audioBlockFormatID"][:-8] + "00000002")
            set_time(second, "rtime", 96031)
            set_time(second, "duration", FRAMES - 96031)
            set_gain(second, {"value": .5})
            channel.append(second)
    if "swap" in spec:
        for left, right in ((0, 1), (8, 9)):
            tag = "speakerLabel" if spec["swap"] == "labels" else "position"
            first, second = named(bed[left], tag), named(bed[right], tag)
            for a, b in zip(first, second):
                a.text, b.text = b.text, a.text
    pcm, pulses = source_signal(spec.get("signal", "prbs"))
    encoded = ET.tostring(root, encoding="utf-8", xml_declaration=True)
    output = [(kind, encoded if kind == b"axml" else encode24(pcm) if kind == b"data" else payload)
              for kind, payload in parts]
    validate_probe_topology(output, 11, dict(parts)[b"dbmd"])
    path = directory / "input.wav"
    if path.exists():
        if chunks(path) != output:
            raise ValueError("existing bed probe differs from this specification/template")
    else:
        write_riff(path, output)
    manifest = {"specification": spec, "template_sha256": file_sha256(template),
                "identity": inspect_bed(path), "pulses": pulses, "prbs_seed": SEED,
                "signal_start": 4096, "signal_stop": 192000,
                "generator_sha256": file_sha256(Path(__file__))}
    if manifest["identity"]["objects"][0]["input_channel"] != 10 or np.any(pcm[:, 10]):
        raise ValueError("auxiliary object must remain silent and retain its PCM binding")
    original = inspect_bed(template)
    for field in ("chna_sha256", "dbmd_sha256", "frames", "channels"):
        if original[field] != manifest["identity"][field]:
            raise ValueError(f"bed experiment changed source topology: {field}")
    (directory / "case.json").write_text(json.dumps(manifest, indent=2) + "\n")
    return path, manifest, pcm


def difference(actual, expected):
    residual = actual.astype(float) - expected.astype(float)
    return {"max_abs_pcm": float(np.max(np.abs(residual))),
            "relative_l2": float(np.linalg.norm(residual) / max(np.linalg.norm(expected), 1e-30)),
            "all_samples_exact": bool(np.array_equal(actual, expected))}


def impulse_measurement(pcm, manifest):
    grouped = [[] for _ in range(10)]
    for pulse in manifest["pulses"]:
        grouped[pulse["input_channel"]].append(pcm[pulse["sample"]].astype(float) / pulse["amplitude"])
    matrix = np.array([np.median(group, axis=0) for group in grouped])
    expected = np.zeros_like(pcm)
    for pulse in manifest["pulses"]:
        expected[pulse["sample"]] = matrix[pulse["input_channel"]] * pulse["amplitude"]
    return {"matrix_input_by_output": matrix.tolist(),
            "repeat_gain_max_abs": max(float(np.max(np.abs(np.array(group) - matrix[index])))
                                        for index, group in enumerate(grouped)),
            "power_per_input": np.sum(matrix ** 2, axis=1).tolist(),
            "zero_delay_gain_prediction": difference(pcm, expected)}


def measure(root, directory, adm, manifest, source, profile, mapping):
    if not (directory / "reference/run.json").exists():
        export_reference(adm, directory, profile)
    run_path = directory / "reference/run.json"
    run = read_json(run_path)
    # Failed imports are evidence too, but must not be treated as successful audio.
    if not run.get("success"):
        if run.get("restore_errors") or run.get("baseline_settings_sha256") != run.get("settings_sha256_after"):
            raise ValueError("failed bed experiment did not restore Renderer state")
        result = {"specification": manifest["specification"], "adm_sha256": file_sha256(adm),
                  "accepted": False, "export_error": run.get("error"), "state_restored": True}
        log = directory / "renderer.log"
        result["renderer_errors"] = [line for line in log.read_text().splitlines() if "ERROR" in line] if log.exists() else []
        result["status"] = ("bed_format_rejected" if any("unsupported bed format" in line
                            for line in result["renderer_errors"]) else "export_failed")
        (directory / "result.json").write_text(json.dumps(result, indent=2) + "\n")
        return result
    for artifact in run["outputs"]:
        path = Path(artifact["path"])
        if not path.exists() and path.with_suffix(".wav.gz").exists():
            data = gzip.decompress(path.with_suffix(".wav.gz").read_bytes())
            if hashlib.sha256(data).hexdigest() != artifact["sha256"]:
                raise ValueError("compressed reference WAV hash mismatch")
            path.write_bytes(data)
    validate_reference(run_path, adm, {"7.1.4", "9.1.6"})
    result = {"specification": manifest["specification"], "adm_sha256": file_sha256(adm),
              "accepted": True, "state_restored": True, "fixed_delay_samples": 0, "global_level": 1,
              "layouts": {}}
    retained = {}
    for output in run["outputs"]:
        layout = output["layout"]
        pcm = canonical_pcm(decode_wav(Path(output["path"]), output["channels"], FRAMES),
                            layout, mapping[layout], False)
        row = {"decoded_canonical_pcm_sha256": digest_pcm(pcm), "finite": bool(np.isfinite(pcm).all()),
               "channel_energy": np.sum(pcm.astype(float) ** 2, axis=0).tolist(),
               "output_labels": [item["label"] for item in sorted(mapping[layout]["interleaved_to_mono"],
                                                                    key=lambda item: item["mono_index"])]}
        if manifest["pulses"]:
            row.update(impulse_measurement(pcm, manifest))
        else:
            impulse = root / "impulses/result.json"
            if impulse.exists():
                matrix = np.array(read_json(impulse)["layouts"][layout]["matrix_input_by_output"])
                row["impulse_matrix_prediction"] = difference(pcm, source[:, :10] @ matrix)
            baseline = root / "prbs/reference-pcm.npz"
            if manifest["specification"]["id"] != "prbs" and baseline.exists():
                with np.load(baseline) as archive:
                    control = archive[layout]
                baseline_hash = read_json(root / "prbs/result.json")["layouts"][layout]["decoded_canonical_pcm_sha256"]
                if digest_pcm(control) != baseline_hash:
                    raise ValueError("retained baseline PCM hash mismatch")
                row["baseline_comparison"] = difference(pcm, control)
        result["layouts"][layout] = row
        retained[layout] = pcm
    # Canonical references are retained losslessly; pruning raw WAVs is a separate audited step.
    np.savez_compressed(directory / "reference-pcm.npz", **retained)
    (directory / "result.json").write_text(json.dumps(result, indent=2) + "\n")
    return result


def routing_matrix(layout):
    # Confirmed standard bed routes, expressed through the existing public
    # matrix option. Equal weights implement equal power; no fitted correction.
    if layout == "7.1.4":
        targets = [[name] for name in ("M+030", "M-030", "M+000", "LFE1", "M+090", "M-090", "M+135", "M-135")]
        targets += [["U+045", "U+135"], ["U-045", "U-135"]]
    elif layout == "9.1.6":
        targets = [[name] for name in ("M+030", "M-030", "M+000", "LFE1", "M+110", "M-110", "M+150", "M-150", "U+110", "U-110")]
    else:
        raise ValueError("unverified bed output layout")
    return {"schema": "mradm.direct-speakers-matrix.v1", "output_layout": layout,
            "routes": [{"source_label": label, "targets": [{"label": target, "weight": 1} for target in target_list]}
                       for index, (label, target_list) in enumerate(zip(LABELS, targets)) if index != 3]}


def compare_saf(root, mapping):
    subprocess.run(["cmake", "--build", "--preset", "release", "--target", "mradm"], cwd=ROOT, check=True)
    binary = ROOT / "build/release/mradm"
    directory = root / "candidate"
    directory.mkdir(exist_ok=True)
    report = {"build": "Release", "binary_sha256": file_sha256(binary), "cases": []}
    for layout in ("7.1.4", "9.1.6"):
        matrix = directory / f"bed-{layout}.json"
        matrix.write_text(json.dumps(routing_matrix(layout), indent=2) + "\n")
        for case in ("impulses", "prbs"):
            manifest = read_json(root / case / "case.json")
            adm = root / case / "input.wav"
            if file_sha256(adm) != manifest["identity"]["sha256"]:
                raise ValueError("candidate ADM changed")
            reference_row = read_json(root / case / "result.json")["layouts"][layout]
            with np.load(root / case / "reference-pcm.npz") as archive:
                reference = archive[layout]
            if digest_pcm(reference) != reference_row["decoded_canonical_pcm_sha256"]:
                raise ValueError("candidate comparison reference changed")
            for mode in ("label", "matrix"):
                prefix = f"{case}-{layout}-{mode}"
                wav = directory / (prefix + ".wav")
                semantic = directory / (prefix + "-semantic.json")
                with tempfile.TemporaryDirectory(prefix="bed-candidate-", dir=directory) as temporary:
                    temporary_wav = Path(temporary) / "output.wav"
                    command = [str(binary), "render", "-i", str(adm), "-o", str(temporary_wav), "--renderer", "saf",
                               "--output-layout", layout, "--no-peak-limit", "--output-bit-depth", "f32",
                               "--direct-speakers-routing", mode, "--write-semantic-report", str(semantic)]
                    if mode == "matrix":
                        command += ["--direct-speakers-matrix", str(matrix)]
                    with (directory / (prefix + ".log")).open("w") as log:
                        subprocess.run(command, stdout=log, stderr=subprocess.STDOUT, check=True)
                    temporary_wav.replace(wav)
                pcm = canonical_pcm(decode_wav(wav, mapping[layout]["channels"], FRAMES), layout, mapping[layout], True)
                row = {"case": case, "layout": layout, "mode": mode, "adm_sha256": file_sha256(adm),
                       "wav_sha256": file_sha256(wav), "semantic_report_sha256": file_sha256(semantic),
                       "pcm_comparison": difference(pcm, reference)}
                if mode == "matrix":
                    row["matrix_sha256"] = file_sha256(matrix)
                if case == "impulses":
                    row["impulses"] = impulse_measurement(pcm, manifest)
                    a = np.array(row["impulses"]["matrix_input_by_output"])
                    b = np.array(reference_row["matrix_input_by_output"])
                    row["gain_relative_l2_per_input"] = (np.linalg.norm(a - b, axis=1) / np.linalg.norm(b, axis=1)).tolist()
                report["cases"].append(row)
    report["matrix_static_diagnostic_passed"] = all(row["pcm_comparison"]["max_abs_pcm"] <= 2e-7
                                                   for row in report["cases"] if row["mode"] == "matrix")
    (directory / "report.json").write_text(json.dumps(report, indent=2) + "\n")
    return report


def capture_bed(root, channel_map):
    # Reopen the same PRBS master for each capture. Actual PCM pointers are
    # linked by analyze_bed_trace, including the adapter's LFE-first ordering.
    from analyze_bed_trace import analyze
    results = {}
    for layout in ("7.1.4", "9.1.6"):
        directory = root / "traces" / ("prbs-" + layout.replace(".", ""))
        if not directory.exists():
            subprocess.run([sys.executable, str(HERE / "run_gain_probe.py"), "trace", "--adm", str(root / "prbs/input.wav"),
                            "--layout", layout, "--profile", str(HERE / "renderer55_gain_profile.json"),
                            "--output-dir", str(directory), "--baseline-run", str(root / "prbs/reference/run.json")], check=True)
        run = read_json(directory / "export/run.json")
        for artifact in run["outputs"]:
            path = Path(artifact["path"])
            if not path.exists() and path.with_suffix(".wav.gz").exists():
                data = gzip.decompress(path.with_suffix(".wav.gz").read_bytes())
                if hashlib.sha256(data).hexdigest() != artifact["sha256"]:
                    raise ValueError("compressed traced PCM changed")
                path.write_bytes(data)
        result = analyze(directory, channel_map)
        results[layout] = {key: result[key] for key in ("verified_blocks", "max_errors", "passes_capture_validation")}
    return results


def compact(root):
    manifest_path = root / "storage-manifest.json"
    prior = read_json(manifest_path) if manifest_path.exists() else []
    records = {item["path"]: item for item in prior}
    for path in sorted([*root.rglob("*.wav"), *root.rglob("trace.json")]):
        relative = path.relative_to(root)
        if (relative == Path("template.wav") or relative.parts[0] in ("impulses", "prbs")):
            continue
        # Only artifacts generated in this new study are compacted. Retain
        # byte-exact WAV/trace data and verify it before removing the raw file.
        data = path.read_bytes()
        digest = hashlib.sha256(data).hexdigest()
        archive = path.with_suffix(path.suffix + ".gz")
        temporary = archive.with_suffix(archive.suffix + ".tmp")
        temporary.write_bytes(gzip.compress(data, compresslevel=6, mtime=0))
        if hashlib.sha256(gzip.decompress(temporary.read_bytes())).hexdigest() != digest:
            raise ValueError("lossless compaction verification failed")
        temporary.replace(archive)
        records[str(relative)] = {"path": str(relative), "sha256": digest, "bytes": len(data),
                                  "archive": str(archive.relative_to(root)), "archive_sha256": file_sha256(archive)}
        manifest_path.write_text(json.dumps(list(records.values()), indent=2) + "\n")
        path.unlink()


def audit_exports(root):
    rows = []
    for path in sorted(root.rglob("run.json")):
        run = read_json(path)
        initial, final = run.get("initial", {}), run.get("final", {})
        rows.append({"run": str(path), "export_success": run.get("success", False),
                     "restore_errors": run.get("restore_errors"),
                     "keys_24_251_restored": run.get("baseline_settings_sha256") == run.get("settings_sha256_after"),
                     "master_restored": initial.get("master") == final.get("master"),
                     "layout_restored": initial.get("config") == final.get("config"),
                     "exporter_restored": initial.get("exporter") == final.get("exporter")})
    result = {"all_restored": bool(rows) and all(not row["restore_errors"] and
              all(row[key] for key in ("keys_24_251_restored", "master_restored", "layout_restored", "exporter_restored"))
              for row in rows), "runs": rows}
    (root / "state-audit.json").write_text(json.dumps(result, indent=2) + "\n")
    return result


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--output-dir", type=Path, required=True)
    parser.add_argument("--channel-map", type=Path, required=True)
    parser.add_argument("--only", help="comma-separated case IDs; default is the full field-isolation suite")
    parser.add_argument("--generate-only", action="store_true")
    parser.add_argument("--capture", action="store_true", help="validate normal-path OMO/OAR traces in both layouts")
    parser.add_argument("--compare-saf", action="store_true", help="Release label/matrix routing diagnostic on the same ADMs")
    parser.add_argument("--compact", action="store_true", help="losslessly compress generated intermediate WAVs/traces")
    args = parser.parse_args()
    root = args.output_dir.resolve()
    root.mkdir(parents=True, exist_ok=True)
    template = prepare_template(root, SEED)
    profile = read_json(HERE / "renderer55_gain_profile.json")
    mapping = read_json(args.channel_map)["layouts"]
    if set(mapping) != {"7.1.4", "9.1.6"} or not all(row["all_samples_exact"] for row in mapping.values()):
        raise ValueError("frozen named multi-mono channel map required")
    selected = args.only.split(",") if args.only else [item["id"] for item in specifications()]
    unknown = set(selected) - {item["id"] for item in specifications()}
    if unknown:
        raise ValueError(f"unknown bed cases: {unknown}")
    summary = {"scope": "7.1.2 DirectSpeakers bed; 48 kHz; Renderer 5.5 direct ADM",
               "profile": profile, "channel_map_sha256": file_sha256(args.channel_map), "cases": []}
    for spec in specifications():
        if spec["id"] not in selected:
            continue
        if (root / "STOP").exists():
            break
        print(spec["id"], flush=True)
        directory = root / spec["id"]
        adm, manifest, pcm = make_case(template, directory, spec)
        if not args.generate_only:
            summary["cases"].append(measure(root, directory, adm, manifest, pcm, profile, mapping))
        summary["cases"] = [read_json(root / item["id"] / "result.json") for item in specifications()
                            if (root / item["id"] / "result.json").exists()]
        (root / "summary.json").write_text(json.dumps(summary, indent=2) + "\n")
    if not args.generate_only:
        if args.capture:
            summary["traces"] = capture_bed(root, args.channel_map.resolve())
        if args.compare_saf:
            summary["candidate"] = compare_saf(root, mapping)
        summary["expected_outcomes_verified"] = bool(summary["cases"]) and all(row["accepted"] or
            (row["specification"]["id"] == "labels_swap" and row.get("status") == "bed_format_rejected")
            for row in summary["cases"])
        completed = {item["specification"]["id"] for item in summary["cases"]}
        summary["requested_cases_complete"] = set(selected).issubset(completed)
        summary["full_field_suite_complete"] = completed == {item["id"] for item in specifications()}
        summary["state_restored"] = audit_exports(root)["all_restored"]
        (root / "summary.json").write_text(json.dumps(summary, indent=2) + "\n")
        if not all(summary[key] for key in ("expected_outcomes_verified", "requested_cases_complete", "state_restored")):
            raise SystemExit("unexpected reference failure; retained in result.json")
        if args.capture and not all(row["passes_capture_validation"] for row in summary["traces"].values()):
            raise SystemExit("bed trace validation failed; retained in bed-validation.json")
        if args.compare_saf and not summary["candidate"]["matrix_static_diagnostic_passed"]:
            raise SystemExit("explicit bed matrix diagnostic failed; retained in candidate/report.json")
    if args.compact:
        compact(root)


if __name__ == "__main__":
    main()
