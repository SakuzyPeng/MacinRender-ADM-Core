#!/usr/bin/env python3
"""Bound the diffuse-related real-time rendering failure in self-owned AVPlayer.

All players/codecs retain their default capabilities. The forwarding observer
copies AudioUnit output; diagnostic changes are confined to generated metadata.
"""

import argparse
import copy
import json
import os
from pathlib import Path
import plistlib

import numpy as np

from make_adm_input import component_bitrates, metadata
from run_case import run_case
from wrap_caf import wrap


def run(source, output, codec, player, observer, single_settings, layout_controls=False, diffuse_mix_controls=False,
        counts=None, diffuse_limit_controls=False, synthetic_boundary=False):
    output.mkdir(parents=True, exist_ok=False)
    expected = json.loads((source / "expected.json").read_text())
    original = np.fromfile(source / "input.f32", dtype="<f4").reshape(-1, 43)[:49152]
    config = plistlib.loads((source / "settings.plist").read_bytes())
    rows = []
    variants = [(f"scene-diffuse-{value:g}", value, False, False) for value in (0, .25, .5, .75, .99, 1)]
    variants += [("scene-groups1-diffuse1", 1, False, True), ("single-source-position", 1, True, False),
                 ("single-unit-distance", 1, True, False), ("single-equator", 1, True, False)]
    if layout_controls:
        variants = [(f"bed{bed}-objects{count}", 1, False, False) for bed, count in
                    ((10, 1), (10, 2), (10, 7), (10, 8), (10, 24), (2, 8), (2, 32))]
    if diffuse_mix_controls:
        variants = [(name, 1, False, False) for name in ("bed10-objects18", "bed10-objects19",
                    "bed10-objects32-all1", "bed10-objects32-all1-last0")]
    if counts:
        if any(count < 1 or count > 32 for count in counts):
            raise ValueError("this fixture supports 1..32 objects")
        layout_controls = True
        variants = [(f"bed10-objects{count}", 1, False, False) for count in counts]
    if diffuse_limit_controls:
        layout_controls = True
        variants = [(name, 1, False, False) for name in ("bed10-objects9-first8", "bed10-objects32-first8",
                    "bed10-objects32-first1", "bed10-objects32-first9")]
    if synthetic_boundary:
        layout_controls = True
        variants = [("bed2-objects8-first1", 1, False, False), ("bed2-objects9-first1", 1, False, False),
                    ("bed2-objects9-first1-point", 0, False, False)]
    for name, diffuse, single, groups1 in variants:
        # Observation is confined to our player, never the encoder or afconvert.
        for key in ("DYLD_INSERT_LIBRARIES", "APAC_SPATIAL_OBSERVER", "APAC_PROFILE_CEILING"):
            os.environ.pop(key, None)
        case = output / name
        case.mkdir()
        tracks = copy.deepcopy(expected["tracks"][10:11] if single else expected["tracks"][10:])
        settings = plistlib.loads(single_settings.read_bytes()) if single else copy.deepcopy(config)
        for track in tracks:
            track["width"] = track["height"] = track["depth"] = 0
            if track["channel"] < 18:
                track["diffuse"] = diffuse
        if single:
            x = np.zeros((len(original), 4), dtype="<f4")
            x[:, 2] = original[:, 10]
            tracks[0]["group_id"] = 1
            if name in ("single-unit-distance", "single-equator"):
                tracks[0]["spherical"][2] = 1
            if name == "single-equator":
                tracks[0]["spherical"] = [60, 0, 1]
        else:
            x = original.copy()
            x[:, :10], x[:, 18:42] = 0, 0
        bitrate_target = 768000 if single else 12000000
        if layout_controls or diffuse_mix_controls:
            bed_text, count_text = name.split("-")[:2]
            bed, count = int(bed_text[3:]), int(count_text[7:])
            tracks = tracks[:count]
            if "all1" in name:
                for track in tracks:
                    track["diffuse"] = 1
                if name.endswith("last0"):
                    tracks[-1]["diffuse"] = 0
            if "-first" in name:
                diffuse_count = int(name.rsplit("first", 1)[1].split("-")[0])
                for index, track in enumerate(tracks):
                    track["diffuse"] = diffuse if index < diffuse_count else 0
            x = np.zeros((len(original), bed + count + 1), dtype="<f4")
            x[:, bed:bed + min(8, count)] = original[:, 10:10 + min(8, count)]
            if "first1" in name:
                x[:, bed + 1:bed + count] = 0
            if synthetic_boundary:
                x[:, :-1] = 0
                x[:, bed] = .03 * np.sin(2 * np.pi * 1000 * np.arange(len(x)) / 48000)
                for index, track in enumerate(tracks):
                    track.update(name=f"synthetic-object-{index}", object_id=f"synthetic-{index}",
                                 uid=f"synthetic-{index}", spherical=[60, 0, 1], xyz=[-0.8660254, .5, 0])
            bed_config = config if bed == 10 else plistlib.loads(single_settings.read_bytes())
            components = [copy.deepcopy(bed_config["parameters"][0]["ASComponents"][0])]
            sizes = []
            for first in range(0, count, 7):
                size = min(7, count - first)
                sizes.append(size)
                components.append({"key": "Object", "Object": [{"key": "Object Count", "current value": size},
                    {"key": "Channel Map", "current minimum range": bed + first,
                     "current maximum range": bed + first + size - 1}]})
            outputs = copy.deepcopy(components)
            rates = [(bed - 1) * 256000 + 16000, *[size * 256000 for size in sizes]]
            for comp, bitrate in zip(outputs, rates):
                comp[comp["key"]].append({"key": "Bit Rate", "current value": bitrate})
            bitrate_target = sum(rates)
            settings = {"version": 1, "sub version": 2, "parameters": [
                {"key": "Audio Scene Components", "ASComponents": components},
                {"key": "Codec Configurations", "ASComponents": outputs},
                {"key": "APAC Metadata", "Metadata": [{"key": "Channel Map",
                    "current minimum range": bed + count, "current maximum range": bed + count}]}]}
        if groups1:
            components = [copy.deepcopy(config["parameters"][0]["ASComponents"][0])]
            for channel in range(10, 42):
                components.append({"key": "Object", "Object": [{"key": "Object Count", "current value": 1},
                    {"key": "Channel Map", "current minimum range": channel, "current maximum range": channel}]})
            settings["parameters"][0]["ASComponents"] = components
            outputs = copy.deepcopy(components)
            for comp, bitrate in zip(outputs, component_bitrates([1] * 32, 12000000)):
                comp[comp["key"]].append({"key": "Bit Rate", "current value": bitrate})
            settings["parameters"][1]["ASComponents"] = outputs
        blob = metadata(tracks)
        words = np.frombuffer(blob + b"\0" * (len(blob) % 2), dtype="<i2").astype("<f4") / 32768
        x[:, -1] = 0
        for start in range(0, len(x), 1024):
            x[start:start + len(words), -1] = words
        x.tofile(case / "input.f32")
        (case / "settings.plist").write_bytes(plistlib.dumps(settings))
        (case / "input-metadata.json").write_text(json.dumps(tracks, indent=2) + "\n")
        prefix = case / "encode/stream"
        result = run_case([str(codec), "encode", str(case / "settings.plist"), str(case / "input.f32"),
                           str(x.shape[1]), str(x.shape[1] - 1), str(prefix), "0",
                           str(bitrate_target)], case / "encode", 30)
        if result["returncode"]:
            raise RuntimeError(name + ": encode failed")
        caf, mp4 = case / "clip.caf", case / "clip.mp4"
        wrap(prefix, caf)
        result = run_case(["afconvert", str(caf), str(mp4), "-f", "mp4f", "-d", "0"], case / "wrap", 30)
        if result["returncode"]:
            raise RuntimeError(name + ": wrap failed")
        os.environ["DYLD_INSERT_LIBRARIES"] = str(observer)
        os.environ["APAC_SPATIAL_OBSERVER"] = str(case / "player/capture")
        result = run_case([str(player), str(mp4), str(case / "player/format.plist")], case / "player", 30)
        os.environ.pop("DYLD_INSERT_LIBRARIES", None)
        os.environ.pop("APAC_SPATIAL_OBSERVER", None)
        files = list((case / "player").glob("capture-unit*.f32"))
        if result["returncode"] or len(files) != 1:
            raise RuntimeError(name + ": player observation failed")
        rendered = np.fromfile(files[0], dtype="<f4").reshape(-1, 2).astype(float)
        steady = rendered[round(.25 * 48000):round(.9 * 48000)]
        row = {"case": name, "diffuse": diffuse, "single": single, "groups1": groups1,
               "frames_captured": len(rendered), "steady_window": [.25, .9],
               "steady_rms": np.sqrt(np.mean(steady**2, axis=0)).tolist(),
               "steady_peak": np.max(np.abs(steady), axis=0).tolist()}
        rows.append(row)
        (output / "verification.json").write_text(json.dumps(rows, indent=2) + "\n")
        print(json.dumps(row), flush=True)


if __name__ == "__main__":
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("source", type=Path)
    parser.add_argument("output", type=Path)
    parser.add_argument("--codec", type=Path, required=True)
    parser.add_argument("--player", type=Path, required=True)
    parser.add_argument("--observer", type=Path, required=True)
    parser.add_argument("--single-settings", type=Path, required=True)
    parser.add_argument("--layout-controls", action="store_true")
    parser.add_argument("--diffuse-mix-controls", action="store_true")
    parser.add_argument("--counts", nargs="+", type=int)
    parser.add_argument("--diffuse-limit-controls", action="store_true")
    parser.add_argument("--synthetic-boundary", action="store_true")
    args = parser.parse_args()
    run(*[getattr(args, name).resolve() for name in ("source", "output", "codec", "player", "observer", "single_settings")],
        layout_controls=args.layout_controls, diffuse_mix_controls=args.diffuse_mix_controls, counts=args.counts,
        diffuse_limit_controls=args.diffuse_limit_controls, synthetic_boundary=args.synthetic_boundary)
