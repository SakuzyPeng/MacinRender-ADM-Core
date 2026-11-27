#!/usr/bin/env python3
"""Bind measured offline speaker profiles to their original room for native playback."""
import argparse
import hashlib
import json
import math
from pathlib import Path
import shutil
import subprocess
import tempfile


def digest(data):
    return hashlib.sha256(data).hexdigest()


def component(directory, name, expected):
    if not name or Path(name).name != name or any(c in name for c in ("/", "\\", ":", "\0")):
        raise ValueError("Expected a flat component name")
    path = directory / name
    if path.is_symlink() or not path.is_file() or digest(path.read_bytes()) != expected:
        raise ValueError(f"Missing or mismatched component {name}")
    return path


def package(room_file, profiles, target, cli):
    original = room_file.read_bytes()
    room = json.loads(original)
    rate = room["sample_rate"]
    if "speaker_calibration" in room:
        raise ValueError("The room already carries speaker calibration")
    catalog = json.loads(subprocess.check_output([cli, "input-layouts", "--format", "json"]))
    orders = {row["id"]: [c["speaker_label"] for c in row["channels"]] for row in catalog["layouts"]}
    layouts = {}
    impulse = None
    for layout in ("7.1.4", "9.1.6", "22.2"):
        p = json.loads((profiles / f"{layout}-{rate}.json").read_bytes())
        if (p["schema"], p["version"], p["room_id"], p["room_manifest_sha256"], p["sample_rate"], p["layout"]) != (
            "mradm.offline-speaker-calibration", 1, room["id"], digest(original), rate, layout
        ):
            raise ValueError(f"{layout} calibration belongs to a different room or rate")
        rows = p["channels"]
        if [r["label"] for r in rows] != orders[layout]:
            raise ValueError(f"{layout} speaker order mismatch")
        for index, row in enumerate(rows, 1):
            if (row["index"] != index or row["kind"] != ("LFE" if row["label"].startswith("LFE") else "fullrange")
                    or type(row["delay_frames"]) is not int or not 0 <= row["delay_frames"] <= rate
                    or not math.isfinite(row["gain_db"])):
                raise ValueError(f"Invalid {layout} trim at channel {index}")
        layouts[layout] = [{k: row[k] for k in ("delay_frames", "gain_db")} for row in rows]
        asset = p["lfe_fir"]
        path = component(profiles, asset["file"], asset["sha256"])
        if asset["dtype"] != "<f8" or not 0 < asset["taps"] <= 2 * rate or path.stat().st_size != asset["taps"] * 8:
            raise ValueError("Invalid LFE FIR")
        entry = {"file": "speaker-lfe.f64", "bytes": path.stat().st_size, "dtype": "<f8",
                 "shape": [asset["taps"]], "sha256": asset["sha256"]}
        if impulse is not None and entry != impulse[1]:
            raise ValueError("Layouts must share the measured LFE response")
        impulse = path, entry
    room["version"] = 2
    room["speaker_calibration"] = {"version": 1, "layouts": layouts, "lfe_fir": impulse[1],
                                   "measured_manifest_sha256": digest(original)}
    if target.exists():
        raise FileExistsError(target)
    target.parent.mkdir(parents=True, exist_ok=True)
    with tempfile.TemporaryDirectory(dir=target.parent) as stage:
        out = Path(stage) / "package"
        out.mkdir()
        files = {a["file"]: component(room_file.parent, a["file"], a["sha256"]) for a in room["assets"].values()}
        default = room.get("default_hrir")
        if default and Path(default).name == default and (room_file.parent / default).is_file():
            header = room_file.parent / default
            data = json.loads(header.read_bytes())
            files[default] = header
            for a in data["assets"].values():
                files[a["file"]] = component(room_file.parent, a["file"], a["sha256"])
        definition = room_file.parent / "definition.json"
        if definition.is_file():
            files["definition.json"] = definition
        if impulse[1]["file"] in files:
            raise ValueError("Calibration FIR name conflicts with an acoustic asset")
        files[impulse[1]["file"]] = impulse[0]
        for name, path in files.items():
            shutil.copyfile(path, out / name)
        (out / "room.json").write_bytes((json.dumps(room, ensure_ascii=False, indent=2, allow_nan=False) + "\n").encode())
        checks = [f"{digest(path.read_bytes())}  {path.name}\n" for path in sorted(out.iterdir())]
        (out / "SHA256SUMS").write_bytes("".join(checks).encode())
        out.rename(target)


if __name__ == "__main__":
    parser = argparse.ArgumentParser(description=__doc__)
    for opt in ("room", "profiles", "output", "mradm"):
        parser.add_argument("--" + opt, type=Path, required=True)
    a = parser.parse_args()
    package(a.room, a.profiles, a.output, a.mradm.resolve())
