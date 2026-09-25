#!/usr/bin/env python3
"""Run a static real-ADM APAC trial with the default native decoder, outside CTest."""

import argparse
import hashlib
import json
import os
from pathlib import Path
import shutil

from make_adm_input import generate, require
from run_case import run_case
from verify_adm import verify_audio, verify_metadata
from wrap_caf import wrap


def digest(path):
    value = hashlib.sha256()
    with path.open("rb") as source:
        for data in iter(lambda: source.read(4 * 1024 * 1024), b""):
            value.update(data)
    return value.hexdigest()


def normalized_timing(prefix, expected):
    path = Path(str(prefix) + ".timing.json")
    timing = json.loads(path.read_text())
    require(timing["valid_frames"] == expected["staged_frames"], "encoder input frame mismatch")
    original = Path(str(prefix) + ".encoder-timing.json")
    require(not original.exists(), "timing already normalized")
    shutil.copyfile(path, original)
    timing["valid_frames"] = expected["frames"]
    timing["trailing_frames"] += expected["input_padding_frames"]
    path.write_text(json.dumps(timing) + "\n")


def packet_window(prefix, output, first, count, leading=0):
    output.mkdir(parents=True, exist_ok=False)
    entries = [json.loads(row) for row in Path(str(prefix) + ".packet_index.jsonl").read_text().splitlines()]
    entries = entries[first:first + count]
    require(bool(entries), "empty packet window")
    start = entries[0]["offset"]
    length = entries[-1]["offset"] + entries[-1]["bytes"] - start
    with Path(str(prefix) + ".packets").open("rb") as source:
        source.seek(start)
        data = source.read(length)
    require(len(data) == length, "truncated compressed window")
    (output / "stream.packets").write_bytes(data)
    for entry in entries:
        entry["offset"] -= start
    (output / "stream.packet_index.jsonl").write_text(
        "".join(json.dumps(e, separators=(",", ":")) + "\n" for e in entries))
    for suffix in (".cookie", ".asbd"):
        shutil.copyfile(str(prefix) + suffix, str(output / "stream") + suffix)
    (output / "stream.timing.json").write_text(json.dumps({
        "valid_frames": len(entries) * 1024 - leading, "leading_frames": leading, "trailing_frames": 0}) + "\n")
    (output / "origin.json").write_text(json.dumps({"first_original_packet": first, "packets": len(entries)}) + "\n")


def run(source, output, codec, omit_extent=False, cleanup=False, total_bitrate=None, objects_per_component=7,
        discard_diffuse=False):
    require(not any(os.environ.get(name) for name in (
        "APAC_PROFILE_CEILING", "APAC_DECODER_METADATA", "APAC_BINARY_METADATA", "DYLD_INSERT_LIBRARIES")),
        "disable diagnostic capability/reader overrides before this default-capability trial")
    output.mkdir(parents=True, exist_ok=False)
    expected = generate(source, output / "input", omit_fully_diffuse_extent=omit_extent, total_bitrate=total_bitrate,
                        objects_per_component=objects_per_component, discard_diffuse=discard_diffuse)
    expected_path = output / "input/expected.json"
    prefix = output / "encode/stream"
    transient = [output / "input/input.f32", output / "decode/decoded.f32"]

    def invoke(name, command, timeout=30):
        result = run_case([str(arg) for arg in command], output / name, timeout)
        print(json.dumps({"case": name, "returncode": result["returncode"],
                          "elapsed_seconds": result["elapsed_seconds"]}), flush=True)
        require(result["returncode"] == 0 and not result["timed_out"], f"{name} failed; evidence preserved")
        return result

    # A full 42-track programme is longer than the synthetic one-second cases.
    invoke("encode", [codec, "encode", output / "input/settings.plist", output / "input/input.f32",
                      expected["input_channels"], expected["channels"], prefix, 0, expected["total_bitrate"]], 300)
    invoke("decode", [codec, "decode", prefix, output / "decode/decoded.f32", expected["channels"]], 300)
    normalized_timing(prefix, expected)
    audio = verify_audio(expected_path, prefix, output / "decode/decoded.f32", output / "audio-verification.json")
    packet_window(prefix, output / "head", 0, 8, leading=2048)
    scripts = Path(__file__).resolve().parent
    saved = {key: os.environ.get(key) for key in ("APAC_TRACE_POSITIONS", "APAC_TRACE_LFE", "APAC_ADM_TRACE_PACKETS")}
    try:
        os.environ.update(APAC_TRACE_POSITIONS="0", APAC_TRACE_LFE="1", APAC_ADM_TRACE_PACKETS="8")
        invoke("head-trace", ["xcrun", "lldb", "--batch", "-o", f"command script import {scripts}/trace_codec.py",
                              "-o", f"command script import {scripts}/trace_adm.py", "-o", "run", "--", codec,
                              "decode", output / "head/stream", output / "head-trace/decoded.f32", expected["channels"]], 60)
    finally:
        for key, value in saved.items():
            if value is None:
                os.environ.pop(key, None)
            else:
                os.environ[key] = value
    metadata = verify_metadata(expected_path, output / "head-trace/result.json", output / "metadata-verification.json")
    deliverables = output / "deliverables"
    deliverables.mkdir()
    suffix = "pos-diffuse" if omit_extent else "with-extent"
    if discard_diffuse:
        suffix = "pos-only" if omit_extent else "with-extent-no-diffuse"
    if total_bitrate is not None:
        suffix += "_" + format(total_bitrate / 1000, "g") + "kbps"
    if objects_per_component != 7:
        suffix += "_groups" + str(objects_per_component)
    filename = deliverables / (source.stem + "_7.1.2BED_" + str(expected["objects"]) + "Objects_" + suffix)
    caf, mp4 = Path(str(filename) + ".caf"), Path(str(filename) + ".mp4")
    wrap(prefix, caf)
    invoke("mp4-wrap", ["/usr/bin/afconvert", caf, mp4, "-f", "mp4f", "-d", "0"])
    containers = []
    for kind, path in (("caf", caf), ("mp4", mp4)):
        imported = output / ("import-" + kind) / "stream"
        invoke("import-" + kind, [codec, "import", path, imported])
        row = {"container": str(path), "packets_identical": digest(Path(str(prefix) + ".packets")) ==
               digest(Path(str(imported) + ".packets")), "cookie_identical":
               Path(str(prefix) + ".cookie").read_bytes() == Path(str(imported) + ".cookie").read_bytes(),
               "timing_matches": json.loads(Path(str(prefix) + ".timing.json").read_text()) ==
               json.loads(Path(str(imported) + ".timing.json").read_text())}
        require(all(row[key] for key in ("packets_identical", "cookie_identical", "timing_matches")),
                "container changed codec data or timing")
        containers.append(row)
        transient.append(Path(str(imported) + ".packets"))
    (output / "container-verification.json").write_text(json.dumps(containers, indent=2) + "\n")
    packet_path = Path(str(prefix) + ".packets")
    seconds = expected["frames"] / expected["sample_rate"]
    summary = {"audio_passed": audio["passed"], "metadata_passed_for_encoded_fields": metadata["passed"],
               "sample_rate": expected["sample_rate"], "valid_frames": expected["frames"], "seconds": seconds,
               "audio_channels": expected["channels"], "objects": expected["objects"],
               "objects_per_component": objects_per_component, "component_groups": expected["component_groups"],
               "target_bitrate_bps": expected["total_bitrate"], "component_bitrates_bps": expected["component_bitrates"],
               "actual_packet_bitrate_bps": packet_path.stat().st_size * 8 / seconds,
               "packet_bytes": packet_path.stat().st_size, "packet_sha256": digest(packet_path),
               "actual_profile_level": [metadata["profile"], metadata["level"]],
               "minimum_non_lfe_correlation": audio["minimum_non_lfe_correlation"],
               "minimum_non_lfe_snr_db": audio["minimum_non_lfe_snr_db"], "lfe": audio["lfe"],
               "complete_adm_roundtrip": False, "omitted_extent": expected["omitted_extent"],
               "discard_diffuse": discard_diffuse, "discarded_diffuse": expected["discarded_diffuse"],
               "source_sha256": digest(source), "containers": containers,
               "decoder_modified": False, "system_spatial_playback_verified": False}
    (output / "summary.json").write_text(json.dumps(summary, ensure_ascii=False, indent=2) + "\n")
    manifest = [{"path": str(path.relative_to(output)), "bytes": path.stat().st_size,
                 "sha256": digest(path)} for path in (caf, mp4, output / "summary.json", expected_path)]
    (output / "SHA256.json").write_text(json.dumps(manifest, indent=2) + "\n")
    if cleanup:
        transient.append(packet_path)
        removed = [{"path": str(path.relative_to(output)), "bytes": path.stat().st_size,
                    "sha256": digest(path)} for path in transient]
        (output / "removed-intermediates.json").write_text(json.dumps(removed, indent=2) + "\n")
        for path in transient:
            path.unlink()
    return summary


if __name__ == "__main__":
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("source", type=Path)
    parser.add_argument("output", type=Path)
    parser.add_argument("--codec", required=True, type=Path, help="existing -O2 -g codec_probe binary")
    parser.add_argument("--omit-fully-diffuse-extent", action="store_true")
    parser.add_argument("--discard-diffuse", action="store_true",
                        help="explicitly remove object diffuse; log source values and verify decoded diffuse=0")
    parser.add_argument("--bitrate-kbps", type=int,
                        help="total target in decimal kbps, including all BED/object components")
    parser.add_argument("--objects-per-component", type=int, choices=range(1, 8), default=7,
                        help="use 1 for the AVFoundation stereo fallback compatibility trial")
    parser.add_argument("--cleanup", action="store_true", help="remove large staging/decoded/import PCM after verification")
    args = parser.parse_args()
    bitrate = None if args.bitrate_kbps is None else args.bitrate_kbps * 1000
    run(args.source.resolve(), args.output.resolve(), args.codec.resolve(), args.omit_fully_diffuse_extent,
        args.cleanup, bitrate, args.objects_per_component, args.discard_diffuse)
