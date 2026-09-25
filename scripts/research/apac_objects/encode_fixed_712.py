#!/usr/bin/env python3
"""Encode a static 7.1.2 BED + mono-object ADM BWF to APAC CAF and MP4.

This is one research entry point for the already established position-only
conversion. It rejects other beds and unsupported ADM fields. Verification is
opt-in; default encoding does not decode or play the output.
"""

import argparse
import hashlib
import json
import os
from pathlib import Path
import subprocess

from make_adm_input import component_bitrates, inspect, generate, require
from run_adm_trial import normalized_timing, run as run_verified
from run_case import run_case
from run_experiments import environment
from wrap_caf import wrap


scripts = Path(__file__).resolve().parent
repository = scripts.parents[2]
codec_source = scripts / "codec_probe.cpp"


def digest(path):
    with path.open("rb") as stream:
        return hashlib.file_digest(stream, "sha256").hexdigest()


def codec_binary(override):
    if override:
        binary = override.resolve()
        require(binary.is_file() and os.access(binary, os.X_OK), "codec binary is not executable")
        return binary, None
    binary = repository / "local/apac-object-experiments/bin/codec_probe"
    if binary.is_file() and binary.stat().st_mtime_ns >= codec_source.stat().st_mtime_ns:
        return binary, None
    binary.parent.mkdir(parents=True, exist_ok=True)
    temporary = binary.with_name(binary.name + f".building-{os.getpid()}")
    command = ["xcrun", "clang++", "-std=c++20", "-O2", "-g", "-Wno-deprecated-declarations",
               str(codec_source), "-framework", "AudioToolbox", "-framework", "CoreFoundation",
               "-o", str(temporary)]
    try:
        subprocess.run(command, check=True, capture_output=True, text=True, timeout=30)
        temporary.replace(binary)
    finally:
        temporary.unlink(missing_ok=True)
    return binary, command


def invoke(command, output, timeout=30):
    result = run_case([str(value) for value in command], output, timeout)
    require(result["returncode"] == 0 and not result["timed_out"],
            f"{output.name} failed; see {output / 'result.json'} and stderr.txt")
    return result


def encode(source, output, codec, bitrate_kbps, keep_intermediates):
    output.mkdir(parents=True, exist_ok=False)
    prefix = output / "encode/stream"
    try:
        expected = generate(source, output / "input", omit_fully_diffuse_extent=True,
                            total_bitrate=bitrate_kbps * 1000, objects_per_component=7, discard_diffuse=True)
        seconds = expected["frames"] / 48000
        invoke([codec, "encode", output / "input/settings.plist", output / "input/input.f32",
                expected["input_channels"], expected["channels"], prefix, 0, expected["total_bitrate"]],
               output / "encode", max(30, int(seconds * 2 + 30)))
        normalized_timing(prefix, expected)
        name = f"{source.stem}_7.1.2BED_{expected['objects']}Objects_pos-only_{bitrate_kbps}kbps"
        deliverables = output / "deliverables"
        deliverables.mkdir()
        caf, mp4 = deliverables / (name + ".caf"), deliverables / (name + ".mp4")
        wrap(prefix, caf)
        invoke(["/usr/bin/afconvert", caf, mp4, "-f", "mp4f", "-d", "0"], output / "mp4-wrap")
        packets = Path(str(prefix) + ".packets")
        summary = {"source": str(source), "sample_rate": 48000, "valid_frames": expected["frames"],
                   "seconds": seconds, "bed_layout": "kAudioChannelLayoutTag_Atmos_7_1_2",
                   "bed_channels": 10, "objects": expected["objects"],
                   "object_component_groups": expected["component_groups"], "diffuse": 0,
                   "discarded_diffuse": expected["discarded_diffuse"], "omitted_extent": expected["omitted_extent"],
                   "target_bitrate_bps": expected["total_bitrate"],
                   "actual_packet_bitrate_bps": packets.stat().st_size * 8 / seconds,
                   "packet_bytes": packets.stat().st_size, "packet_sha256": digest(packets),
                   "source_pcm_excerpt_sha256": expected["source_pcm_excerpt_sha256"],
                   "verification": "not_run", "decoder_modified": False,
                   "output_files": [str(caf), str(mp4)]}
        (output / "summary.json").write_text(json.dumps(summary, ensure_ascii=False, indent=2) + "\n")
        return summary
    finally:
        if not keep_intermediates:
            removed = []
            for path in (output / "input/input.f32", Path(str(prefix) + ".packets")):
                if path.exists():
                    removed.append({"path": str(path.relative_to(output)), "bytes": path.stat().st_size,
                                    "sha256": digest(path)})
                    path.unlink()
            (output / "removed-intermediates.json").write_text(json.dumps(removed, indent=2) + "\n")


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("source", type=Path, help="48 kHz PCM24 static ADM BWF with a 7.1.2 BED")
    parser.add_argument("output", type=Path, help="new result directory; never overwrites an existing directory")
    parser.add_argument("--bitrate-kbps", type=int, default=12000, help="total target, decimal kbps (default: 12000)")
    parser.add_argument("--verify", action="store_true", help="run full native audio/metadata/container checks")
    parser.add_argument("--keep-intermediates", action="store_true", help="retain large staged/decoded PCM")
    parser.add_argument("--codec", type=Path, help="existing optimized codec_probe; otherwise build one shared binary")
    args = parser.parse_args()
    source, output = args.source.expanduser().resolve(), args.output.expanduser().resolve()
    require(source.is_file() and source != output, "source file not found")
    require(not output.exists(), "output directory already exists")
    require(1 <= args.bitrate_kbps <= 4294967, "bitrate must fit UInt32 bit/s")
    require(not any(os.environ.get(key) for key in
                    ("APAC_PROFILE_CEILING", "APAC_DECODER_METADATA", "APAC_BINARY_METADATA",
                     "DYLD_INSERT_LIBRARIES")), "remove diagnostic environment overrides before encoding")
    source_info = inspect(source)  # validates the exact 7.1.2 labels and the supported static ADM subset
    require(1 <= source_info["objects"] <= 70, "object count outside the validated default decoder range")
    groups = [min(7, source_info["objects"] - first) for first in range(0, source_info["objects"], 7)]
    component_bitrates(groups, args.bitrate_kbps * 1000)
    for track in source_info["tracks"][10:]:
        require(not any(track[key] for key in ("width", "height", "depth")) or track["diffuse"] == 1,
                "extent outside the validated diffuse=1 fallback")
    facts = environment()  # refuse the version-specific private ASC protocol after a component update
    codec, build_command = codec_binary(args.codec)
    if args.verify:
        summary = run_verified(source, output, codec, omit_extent=True, cleanup=not args.keep_intermediates,
                               total_bitrate=args.bitrate_kbps * 1000, objects_per_component=7,
                               discard_diffuse=True)
    else:
        summary = encode(source, output, codec, args.bitrate_kbps, args.keep_intermediates)
    (output / "entrypoint.json").write_text(json.dumps(
        {"source": str(source), "bed_layout": "7.1.2", "codec_binary": str(codec),
         "codec_source_sha256": digest(codec_source), "build_command": build_command,
         "component": facts, "full_verification_requested": args.verify,
         "intermediates_retained": args.keep_intermediates}, indent=2) + "\n")
    deliverables = output / "deliverables"
    filenames = sorted(deliverables.glob("*"))
    require(len(filenames) == 2 and {p.suffix for p in filenames} == {".caf", ".mp4"},
            "encoder did not produce both containers")
    mp4 = next(p for p in filenames if p.suffix == ".mp4")
    caf = next(p for p in filenames if p.suffix == ".caf")
    readme = (f"# APAC 7.1.2 BED＋{summary['objects']} 对象\n\n"
              f"[MP4](<{mp4.name}>) · [CAF](<{caf.name}>)\n\n"
              f"输入：{source}；48 kHz、{summary['seconds']:.6f} 秒。\n"
              f"固定 7.1.2 BED，按每组件至多 7 对象分组；所有对象 diffuse=0。\n"
              f"移除 {len(summary['discarded_diffuse'])} 个非零 diffuse 和 "
              f"{len(summary['omitted_extent'])} 个尺寸；源值留在 input/expected.json。\n"
              f"目标 {args.bitrate_kbps} kbps；压缩包实测 "
              f"{summary['actual_packet_bitrate_bps'] / 1000:.3f} kbps。\n"
              f"验证状态：{'全长原生验证完成' if args.verify else '未运行可选验证'}。\n"
              f"源文件不改写；MP4 由 APAC CAF 无重编码封装。\n\n"
              f"[汇总](../summary.json) · [运行环境](../entrypoint.json) · [哈希](../SHA256.json)\n")
    (deliverables / "README.md").write_text(readme)
    items = [caf, mp4, deliverables / "README.md", output / "summary.json",
             output / "input/expected.json", output / "entrypoint.json"]
    if args.verify:
        items += [output / name for name in ("audio-verification.json", "metadata-verification.json",
                                             "container-verification.json")]
    (output / "SHA256.json").write_text(json.dumps(
        [{"path": str(path.relative_to(output)), "bytes": path.stat().st_size, "sha256": digest(path)}
         for path in items], ensure_ascii=False, indent=2) + "\n")
    print(json.dumps({"mp4": str(mp4), "caf": str(caf), "seconds": summary["seconds"],
                      "objects": summary["objects"], "verified": args.verify}, ensure_ascii=False))


if __name__ == "__main__":
    main()
