#!/usr/bin/env python3
"""Small Release end-to-end checks for the self-defined 22.2 room extension.

These are ADM probes for our renderer, not Dolby reference masters. The opaque
DBMD chunk is omitted; no encoded Atmos or new Dolby export is involved.
"""

import argparse
import hashlib
import json
import struct
import subprocess
from pathlib import Path
from xml.etree import ElementTree as ET

import numpy as np

from make_semantic_suite import chunks, child, named, set_time, timecode, write_riff
from trace_bed_semantics import encode24
from measure_static_bank import file_sha256
from measure_size_field import spectral_field

HERE = Path(__file__).resolve().parent
ROOT = HERE.parents[2]
BINARY = ROOT / "build/release/mradm"
LABELS = ["M+060", "M-060", "M+000", "LFE1", "M+135", "M-135", "M+030", "M-030", "M+180", "LFE2",
          "M+090", "M-090", "U+045", "U-045", "U+000", "T+000", "U+135", "U-135", "U+090", "U-090",
          "U+180", "B+000", "B+045", "B-045"]


def make_probe(template, directory, identifier, events, pcm):
    directory.mkdir(parents=True, exist_ok=True)
    parts = dict(chunks(template))
    xml = ET.fromstring(parts[b"axml"])
    if "}" in xml.tag:
        ET.register_namespace("", xml.tag.split("}")[0][1:])
    for owner in named(xml, "audioObject"):
        set_time(owner, "start", 0)
        set_time(owner, "duration", len(pcm))
    channel = next(item for item in named(xml, "audioChannelFormat") if item.attrib.get("typeDefinition") == "Objects")
    for item in list(channel):
        if item.tag.split("}")[-1] == "audioBlockFormat":
            channel.remove(item)
    prefix = channel.tag.rsplit("}", 1)[0] + "}" if "}" in channel.tag else ""
    for index, event in enumerate(events):
        start = event["sample"]
        end = events[index + 1]["sample"] if index + 1 < len(events) else len(pcm)
        block = ET.SubElement(channel, prefix + "audioBlockFormat", {
            "audioBlockFormatID": f"AB_00031001_{index + 1:08x}", "rtime": timecode(start), "duration": timecode(end - start)})
        child(block, "cartesian", 1)
        for axis, value in zip("XYZ", event["xyz"]):
            ET.SubElement(block, prefix + "position", {"coordinate": axis}).text = str(value)
        for dimension in ("width", "height", "depth"):
            child(block, dimension, event.get("size", 0))
        child(block, "diffuse", 0)
    encoded = ET.tostring(xml, encoding="utf-8", xml_declaration=True)
    final = [(b"fmt ", parts[b"fmt "]), (b"axml", encoded), (b"chna", parts[b"chna"]), (b"data", encode24(pcm))]
    path = directory / "input.wav"
    write_riff(path, final)
    manifest = {"id": identifier, "events": events, "frames": len(pcm), "channels": 11,
                "source_sha256": file_sha256(path), "axml_sha256": hashlib.sha256(encoded).hexdigest(),
                "pcm_sha256": hashlib.sha256(final[-1][1]).hexdigest(),
                "chna_sha256": hashlib.sha256(parts[b"chna"]).hexdigest(), "dbmd": None,
                "reference": "own geometry invariants only; no Dolby 22.2 reference"}
    (directory / "case.json").write_text(json.dumps(manifest, indent=2) + "\n")
    return path


def caf_pcm(path):
    tag = None
    with path.open("rb") as stream:
        if stream.read(8) != b"caff\x00\x01\x00\x00":
            raise ValueError("unexpected CAF header")
        pcm = None
        while header := stream.read(12):
            kind, size = struct.unpack(">4sq", header)
            if size < 0:
                raise ValueError("incomplete CAF chunk")
            if kind == b"chan":
                payload = stream.read(size)
                tag = struct.unpack_from(">I", payload)[0]
            elif kind == b"desc":
                payload = stream.read(size)
                rate, fmt, flags, packet, frames, channels, bits = struct.unpack(">d4s5I", payload)
                if (rate, fmt, flags, packet, frames, channels, bits) != (48000, b"lpcm", 3, 96, 1, 24, 32):
                    raise ValueError("unexpected CAF sample format/order")
            elif kind == b"data":
                if stream.read(4) != bytes(4):
                    raise ValueError("edited CAF")
                pcm = np.frombuffer(stream.read(size - 4), dtype="<f4").reshape(-1, 24).copy()
            else:
                stream.seek(size, 1)
    if tag != (204 << 16) | 24 or pcm is None or not np.isfinite(pcm).all():
        raise ValueError("22.2 CAF lacks CICP_13 or valid PCM")
    return pcm


def render(adm, directory, tag="full", extra=()):
    output = directory / (tag + ".caf")
    semantic = directory / (tag + "-semantic.json")
    command = [str(BINARY), "render", "-i", str(adm), "-o", str(output), "--renderer", "triple-balance", "--output-layout", "22.2", "--no-peak-limit", "--output-bit-depth", "f32",
               "--write-semantic-report", str(semantic), *map(str, extra)]
    with (directory / (tag + ".log")).open("w") as log:
        subprocess.run(command, stdout=log, stderr=subprocess.STDOUT, check=True)
    report = json.loads(semantic.read_text())["renderer_effective"]
    if report["profile"] != "room-222-extension-v1" or report["spatial_reference"] is not None or report["status"] != "prepared":
        raise ValueError("22.2 scope/report does not describe an explicit self-defined extension")
    return caf_pcm(output), report


def spectral_checks(template, root):
    event = [{"sample": 0, "xyz": [0, 0, 0], "size": 1}]
    # An independently captured native filter basis validates every new output
    # route. It is not a claimed 22.2 speaker reference or fitted correction.
    fixtures = ROOT / "tests/fixtures/room_compat_size"
    x = np.fromfile(fixtures / "filter-input.f32", dtype="<f4")
    filters = np.fromfile(fixtures / "filter-expected.f32", dtype="<f4").reshape(-1, 4)
    if not np.array_equal(np.rint(x.astype(float) * (1 << 23)) / (1 << 23), x):
        raise ValueError("captured input cannot be represented exactly by PCM24")
    pcm = np.zeros((len(x), 11));pcm[:, 10] = x
    directory = root / "native-filter-basis"
    adm = make_probe(template, directory, "native-filter-basis", event, pcm)
    observed, metadata = render(adm, directory)
    gains = np.array(metadata["objects"][0]["tracks"][0]["events"][0]["target_mix_spread_gains"])
    expected = np.zeros((len(filters), 24))
    segment = x[-len(filters):].astype(float)
    for node in metadata["spatial_nodes"]:
        value = segment
        if node["filter"] >= 0:
            value = .9219544529914856 * segment + .3872983455657959 * node["filter_sign"] * filters[:, node["filter"]]
        expected[:, node["channel"]] = gains[node["channel"]] * value
    native_error = float(np.max(np.abs(expected - observed[-len(filters):])))
    # For full size at the room center, symmetric +/- filter pairs must cancel
    # to the common input component. This tests spectra/correlation without
    # fitting any gain, delay or per-channel correction to the observed PCM.
    signal = np.zeros((240000, 11))
    signal[:192000, 10] = (np.random.default_rng(0x222F1E1D).integers(0, 2, 192000) * 2 - 1) / 16
    directory = root / "spectral"
    adm = make_probe(template, directory, "spectral", event, signal)
    output, metadata = render(adm, directory)
    gain = np.array(metadata["objects"][0]["tracks"][0]["events"][0]["target_mix_spread_gains"])
    pairs = [(0, 1), (4, 5), (6, 7), (10, 11), (12, 13), (16, 17), (18, 19), (22, 23)]
    active = slice(4800, 187200)
    recovered = [(output[active, a] / gain[a] + output[active, b] / gain[b]) / (2 * .9219544529914856)
                 for a, b in pairs]
    recovered += [output[active, c] / gain[c] for c in (2, 8, 14, 15, 20, 21)]
    recovered = np.stack(recovered, axis=1)
    source = signal[active, 10]
    desired = np.broadcast_to(source[:, None], recovered.shape)
    ref_power, ref_matrix, _ = spectral_field(source, desired)
    power, matrix, coherence = spectral_field(source, recovered)
    band_error = float(np.max(np.abs(10 * np.log10(power / ref_power))))
    covariance_error = float(np.max(np.linalg.norm(matrix - ref_matrix, axis=(1, 2)) /
                                    np.linalg.norm(ref_matrix, axis=(1, 2))))
    sample_error = float(np.max(np.abs(recovered - desired)))
    np.savez_compressed(directory / "measurements.npz", reference_band_power=ref_power, candidate_band_power=power,
                        reference_cross_spectrum=ref_matrix, candidate_cross_spectrum=matrix, coherence=coherence)
    result = {"scope": "native four-filter component reference + self-model dry-component identities; no Dolby 22.2 output reference",
              "native_basis_max_pcm_error": native_error,
              "native_filter_fixture_sha256": file_sha256(fixtures / "filter-expected.f32"),
              "common_signal_max_pcm_error": sample_error, "max_band_error_db": band_error,
              "max_cross_spectrum_relative_error": covariance_error,
              "tail_silent_after_1024_samples": bool(not np.any(output[193024:])),
              "per_channel_fitting": False}
    result["passes"] = (native_error < 2e-6 and sample_error < 2e-6 and band_error < .001 and
                         covariance_error < 1e-5 and result["tail_silent_after_1024_samples"])
    return result


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--template", type=Path, required=True)
    parser.add_argument("--output-dir", type=Path, required=True)
    args = parser.parse_args()
    root = args.output_dir.resolve()
    root.mkdir(parents=True, exist_ok=True)
    report = {"binary_sha256": file_sha256(BINARY), "generator_sha256": file_sha256(Path(__file__)),
              "channel_labels_internal_and_caf_order": LABELS, "reference": None, "points": [], "bed": []}
    pulse = np.zeros((8192, 11));pulse[2048, 10] = .125
    origin = root / "origin"
    adm = make_probe(args.template, origin, "origin", [{"sample": 0, "xyz": [0, 0, 0]}], pulse)
    _, prepared = render(adm, origin)
    info = subprocess.check_output(["afinfo", str(origin / "full.caf")], text=True)
    (root / "afinfo.txt").write_text(info)
    if "Channel layout: 22.2" not in info:
        raise ValueError("CoreAudio did not identify CICP_13 as 22.2")
    for node in prepared["spatial_nodes"]:
        channel = node["channel"]
        if LABELS[channel] != node["label"]:
            raise ValueError("geometry label does not match the existing 22.2/CICP channel order")
        directory = root / f"node-{channel:02}"
        adm = make_probe(args.template, directory, f"node-{channel}", [{"sample": 0, "xyz": node["xyz"]}], pulse)
        pcm, _ = render(adm, directory)
        wanted = np.zeros_like(pcm);wanted[2048, channel] = .125
        error = float(np.max(np.abs(pcm - wanted)))
        report["points"].append({"channel": channel, "label": node["label"], "max_error": error, "passes": error < 1e-7})
        adm.unlink()  # reproducible small source; case.json retains all fields and hashes
    bed = np.zeros_like(pulse)
    for channel in range(10):
        bed[1024 + 512 * channel, channel] = .125
    directory = root / "bed"
    adm = make_probe(args.template, directory, "bed", [{"sample": 0, "xyz": [0, 0, 0]}], bed)
    for mode in ("direct", "split-power"):
        pcm, _ = render(adm, directory, mode, ("--lfe-routing", mode))
        wanted = np.zeros_like(pcm)
        for source, destination in enumerate([6, 7, 2, 3, 10, 11, 4, 5, 18, 19]):
            wanted[1024 + 512 * source, destination] = .125
        if mode == "split-power":
            wanted[1024 + 512 * 3, [3, 9]] = .125 * np.sqrt(.5)
        error = float(np.max(np.abs(pcm - wanted)))
        report["bed"].append({"mode": mode, "max_error": error, "passes": error < 1e-7})
    events = [{"sample": 0, "xyz": [0, 0, 0], "size": 0},
              {"sample": 1537, "xyz": [-.4, .2, -.7], "size": .01},
              {"sample": 4096, "xyz": [.2, -.5, .3], "size": .2},
              {"sample": 8191, "xyz": [0, 0, 1], "size": 1},
              {"sample": 12288, "xyz": [0, 0, -1], "size": 0},
              {"sample": 16385, "xyz": [-.4, .7, .2], "size": .25}]
    signal = np.zeros((40000, 11))
    rng = np.random.default_rng(0x222092701)
    signal[:32000] = (rng.integers(0, 2, size=(32000, 11)) * 2 - 1) / 128
    directory = root / "size-motion"
    adm = make_probe(args.template, directory, "size-motion", events, signal)
    full, _ = render(adm, directory)
    repeat, _ = render(adm, directory, "repeat")
    crop, _ = render(adm, directory, "crop", ("--start", str(4801 / 48000), "--end", str(30017 / 48000)))
    report["state"] = {"repeat_exact": bool(np.array_equal(full, repeat)),
                       "crop_exact": bool(np.array_equal(crop, full[4801:30017])),
                       "input_frames": len(signal), "output_frames": len(full),
                       "lfe1_matches_source": bool(np.array_equal(full[:, 3], signal[:, 3])),
                       "lfe2_silent_in_direct": bool(not np.any(full[:, 9])),
                       "tail_finishes": bool(not np.any(full[34000:])),
                       "lower_layer_energy": np.sum(full[:, 21:].astype(float) ** 2, axis=0).tolist()}
    # An explicitly disabled size must match the same XYZ timeline authored at size zero.
    none, _ = render(adm, directory, "spread-none", ("--speaker-spread-mode", "none"))
    zero_directory = root / "point-motion"
    zero_adm = make_probe(args.template, zero_directory, "point-motion", [{**event, "size": 0} for event in events], signal)
    zero, _ = render(zero_adm, zero_directory)
    report["state"]["spread_none_matches_point"] = bool(np.array_equal(none, zero))
    report["spectral"] = spectral_checks(args.template, root)
    state = report["state"]
    report["passes"] = (all(row["passes"] for group in ("points", "bed") for row in report[group]) and
                        all(state[key] for key in ("repeat_exact", "crop_exact", "lfe1_matches_source", "lfe2_silent_in_direct",
                                                   "tail_finishes", "spread_none_matches_point")) and
                        state["input_frames"] == state["output_frames"] and all(v > 0 for v in state["lower_layer_energy"]) and
                        report["spectral"]["passes"])
    (root / "report.json").write_text(json.dumps(report, indent=2) + "\n")
    print(json.dumps({"passes": report["passes"], "points": len(report["points"]), "state": state}, indent=2))
    if not report["passes"]:
        raise SystemExit(1)


if __name__ == "__main__":
    main()
