#!/usr/bin/env python3
"""Field-isolated diffuse/divergence references. Does not enable production features."""

import argparse
import hashlib
import json
from pathlib import Path
from xml.etree import ElementTree as ET

import numpy as np

from gain_trace_adm import inspect_adm
from make_semantic_suite import child, chunks, make_case, named, prepare_template, validate_probe_topology, write_riff
from measure_point_suite import decode_wav
from run_semantic_suite import export_reference, read_json

HERE = Path(__file__).resolve().parent
FIELDS = ("diffuse", "objectDivergence")


def field_snapshot(path):
    xml = ET.fromstring(next(payload for kind, payload in chunks(path) if kind == b"axml"))
    rows = []
    for channel in named(xml, "audioChannelFormat"):
        if channel.attrib.get("typeDefinition") != "Objects":
            continue
        for block in named(channel, "audioBlockFormat"):
            fields = {}
            for name in FIELDS:
                items = [item for item in block if item.tag.split("}")[-1] == name]
                if len(items) > 1:
                    raise ValueError("duplicate spatial semantic field")
                fields[name] = ({"text": items[0].text, "attributes": dict(items[0].attrib)} if items else None)
            rows.append({"block_id": block.attrib["audioBlockFormatID"], **fields})
    return rows


def stripped_xml(parts):
    root = ET.fromstring(next(payload for kind, payload in parts if kind == b"axml"))
    for block in named(root, "audioBlockFormat"):
        for item in list(block):
            if item.tag.split("}")[-1] in FIELDS:
                block.remove(item)
    return ET.tostring(root)


def variant(base, destination, changes):
    parts = chunks(base)
    before = stripped_xml(parts)
    xml = ET.fromstring(next(payload for kind, payload in parts if kind == b"axml"))
    if "}" in xml.tag:
        ET.register_namespace("", xml.tag.split("}")[0][1:])
    blocks = [block for channel in named(xml, "audioChannelFormat")
              if channel.attrib.get("typeDefinition") == "Objects" for block in named(channel, "audioBlockFormat")]
    if len(changes) != len(blocks):
        raise ValueError("spatial changes must specify each original block")
    for block, values in zip(blocks, changes):
        for name, value in values.items():
            if name not in FIELDS:
                raise ValueError("unidentified field in isolation probe")
            if isinstance(value, dict):
                child(block, name, value["value"], {key: str(item) for key, item in value.items() if key != "value"})
            else:
                child(block, name, value)
    encoded = ET.tostring(xml, encoding="utf-8", xml_declaration=True)
    changed = [(kind, encoded if kind == b"axml" else payload) for kind, payload in parts]
    if stripped_xml(changed) != before:
        raise ValueError("field isolation changed unrelated ADM metadata")
    identity = inspect_adm(base)
    dbmd = next(payload for kind, payload in parts if kind == b"dbmd")
    validate_probe_topology(changed, identity["channels"], dbmd)
    write_riff(destination, changed)
    actual = inspect_adm(destination)
    for key in ("pcm_sha256", "chna_sha256", "dbmd_sha256", "frames", "channels"):
        if actual[key] != identity[key]:
            raise ValueError(f"field isolation changed {key}")
    return actual


def groups():
    result = []
    for size in (0, .25):
        group = {"id": f"static_{size}", "size": size, "variants": {}}
        for value in (.01, .25, .5, .75, 1):
            group["variants"][f"diffuse_{value}"] = [{"diffuse": value}]
        group["variants"]["diffuse_absent"] = [{"diffuse": None}]
        for value, distance in ((.25, 1), (1, .25), (1, 1)):
            group["variants"][f"divergence_{value}_range_{distance}"] = [
                {"objectDivergence": {"value": value, "positionRange": distance}}]
        result.append(group)
    sequence = [0, .01, .25, .5, .75, 1, .5, 0]
    for size in (0, .25):
        blocks = [{"rtime": index * 24000, "duration": 24000 if index < 7 else 72000}
                  for index in range(8)]
        result.append({"id": f"dynamic_{size}", "size": size, "blocks": blocks, "variants": {
            "diffuse": [{"diffuse": value} for value in sequence],
            "divergence": [{"objectDivergence": {"value": value, "positionRange": 1}} for value in sequence],
            "combined": [{"diffuse": value, "objectDivergence": {"value": 1 - value, "positionRange": 1}}
                         for value in sequence]}})
    result.append({"id": "size_motion", "blocks": [
        {"rtime": 0, "duration": 48000, "size": 0, "xyz": [0, 0, 0]},
        {"rtime": 48000, "duration": 48000, "size": .01, "xyz": [.4, .7, .5]},
        {"rtime": 96000, "duration": 48000, "size": 1, "xyz": [0, 0, 1]},
        {"rtime": 144000, "duration": 96000, "size": .25, "xyz": [-.6, -.4, .5]}], "variants": {
            "diffuse": [{"diffuse": value} for value in (1, .5, 0, .25)],
            "divergence": [{"objectDivergence": {"value": value, "positionRange": 1}} for value in (.25, 1, .5, 0)]}})
    return result


def measure_reference(directory, adm, profile, control=None):
    run_path = directory / "reference/run.json"
    if not run_path.exists():
        export_reference(adm, directory, profile)
    run = read_json(run_path)
    if (not run["success"] or run.get("restore_errors") or
            run["baseline_settings_sha256"] != run["settings_sha256_after"] or
            run["adm_sha256"] != hashlib.sha256(adm.read_bytes()).hexdigest() or
            {item["layout"] for item in run["outputs"]} != {"7.1.4", "9.1.6"}):
        raise ValueError(f"reference export/identity/restore failed: {run_path}")
    rows, pcm = {}, {}
    for output in run["outputs"]:
        layout = output["layout"]
        path = Path(output["path"])
        if hashlib.sha256(path.read_bytes()).hexdigest() != output["sha256"]:
            raise ValueError("export WAV hash changed")
        value = decode_wav(path, output["channels"], 240000)
        pcm[layout] = value
        row = {"decoded_pcm_sha256": hashlib.sha256(value.astype("<f4").tobytes()).hexdigest(),
               "finite": bool(np.isfinite(value).all()), "channel_energy": np.sum(value.astype(float) ** 2, axis=0).tolist()}
        if control is not None:
            difference = value - control[layout]
            row.update(all_samples_exact=bool(np.array_equal(value, control[layout])),
                       max_pcm_error=float(np.max(np.abs(difference))),
                       relative_pcm_l2=float(np.linalg.norm(difference) / max(np.linalg.norm(control[layout]), 1e-30)))
        rows[layout] = row
    return rows, pcm


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--output-dir", type=Path, required=True)
    parser.add_argument("--only", help="Optional comma-separated groups")
    parser.add_argument("--resume", action="store_true")
    args = parser.parse_args()
    root = args.output_dir.resolve()
    root.mkdir(parents=True, exist_ok=True)
    template = prepare_template(root, 0x53504154)
    profile = read_json(HERE / "renderer55_gain_profile.json")
    summary = {"scope": "direct ADM Cartesian diffuse/divergence field isolation, 48 kHz", "groups": []}
    selected = [item for item in groups() if not args.only or item["id"] in args.only.split(",")]
    for group in selected:
        if (root / "STOP").exists():
            break
        directory = root / group["id"]
        directory.mkdir(exist_ok=True)
        base_spec = {key: value for key, value in group.items() if key != "variants"}
        baseline = make_case(template, directory / "baseline", base_spec)
        print(group["id"], "baseline", flush=True)
        base_rows, control = measure_reference(directory / "baseline", Path(baseline["adm"]), profile)
        group_report = {"id": group["id"], "baseline": baseline, "baseline_measurement": base_rows, "variants": []}
        for name, changes in group["variants"].items():
            work = directory / name
            work.mkdir(exist_ok=True)
            result_path = work / "result.json"
            adm = work / "input.wav"
            identity = variant(Path(baseline["adm"]), adm, changes)
            if result_path.exists() and args.resume:
                retained = read_json(result_path)
                if retained["changes"] != changes or retained["adm_sha256"] != identity["sha256"]:
                    raise ValueError("cannot resume a changed field-isolation case")
                group_report["variants"].append(retained)
                continue
            print(group["id"], name, flush=True)
            manifest = {"changes": changes, "identity": identity, "fields": field_snapshot(adm)}
            (work / "case.json").write_text(json.dumps(manifest, indent=2) + "\n")
            measurements, _ = measure_reference(work, adm, profile, control)
            result = {"id": name, "changes": changes, "adm_sha256": identity["sha256"],
                      "baseline_adm_sha256": baseline["sha256"],
                      "fields": manifest["fields"], "layouts": measurements,
                      "identical_both_layouts": all(row["all_samples_exact"] for row in measurements.values())}
            result_path.write_text(json.dumps(result, indent=2) + "\n")
            group_report["variants"].append(result)
            # The complete canonical control remains; identical variant WAVs
            # need only their exact hashes and field descriptions for replay.
            if result["identical_both_layouts"]:
                run = read_json(work / "reference/run.json")
                pruned = []
                for item in run["outputs"]:
                    path = Path(item["path"])
                    pruned.append({"path": str(path), "sha256": item["sha256"], "retained_control": str(directory / "baseline/reference/run.json")})
                    path.unlink()
                (work / "pruned.json").write_text(json.dumps(pruned, indent=2) + "\n")
        summary["groups"].append(group_report)
        (root / "summary.json").write_text(json.dumps(summary, indent=2) + "\n")
    print(root / "summary.json")


if __name__ == "__main__":
    main()
