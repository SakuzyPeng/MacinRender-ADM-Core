#!/usr/bin/env python3
"""Extract the small, immutable SAF v1.3.4 data subset used by Rust DSP.

This maintenance tool is not part of the build. It does not fetch SAF. Supply an
existing checkout; the generated little-endian binary32 files are committed.
Algorithm code is not copied by this script. See assets/NOTICE.txt for credits.
"""
import argparse
import hashlib
import json
import math
import re
import struct
from pathlib import Path


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("saf_root", type=Path)
    parser.add_argument("--output", type=Path,
                        default=Path(__file__).resolve().parents[2] / "rust/crates/mradm-dsp/assets")
    args = parser.parse_args()
    source = args.saf_root / "framework"
    selected = [
        ("modules/saf_hrir/saf_default_hrirs.c", "__default_hrirs", "kemar_hrirs"),
        ("modules/saf_hrir/saf_default_hrirs.c", "__default_hrir_dirs_deg", "kemar_directions"),
        ("resources/afSTFT/afSTFT_protoFilter.h", "__afSTFT_protoFilter1024", "afstft_prototype"),
    ]
    for order in (20, 15, 6):
        selected.append(("modules/saf_utilities/saf_utility_latticeCoeffs.c",
                         f"__lattice_coeffs_o{order}", f"lattice_o{order}"))
    args.output.mkdir(parents=True, exist_ok=True)
    records = []
    for relative, symbol, name in selected:
        text = (source / relative).read_text()
        match = re.search(r"const float\s+" + re.escape(symbol) + r"((?:\[\d+\])+)[\s=]+\{(.*?)\};", text, re.S)
        if match is None:
            raise RuntimeError(f"Missing array {symbol}")
        shape = [int(value) for value in re.findall(r"\d+", match[1])]
        values = [float(value.rstrip("fF")) for value in re.findall(r"[-+]?(?:\d+\.?\d*|\.\d+)(?:[eE][-+]?\d+)?[fF]?", match[2])]
        if len(values) != math.prod(shape) or not all(map(math.isfinite, values)):
            raise RuntimeError(f"Invalid array {symbol}: {len(values)} vs {shape}")
        # Only two output ears use the first two lattice filters (lookupOffset=0).
        if name.startswith("lattice_"):
            values = values[:2 * shape[1]]
            shape[0] = 2
        data = struct.pack("<" + "f" * len(values), *values)
        filename = name + ".f32le"
        (args.output / filename).write_bytes(data)
        records.append(dict(file=filename, shape=shape, source=relative, symbol=symbol,
                            sha256=hashlib.sha256(data).hexdigest()))
    matrix = args.output / "hoa3_714_sn3d.f32le"
    if matrix.exists():
        data = matrix.read_bytes()
        if len(data) != 11 * 16 * 4:
            raise RuntimeError("Invalid HOA matrix")
        records.append(dict(file=matrix.name, shape=[11, 16],
                            source="tests/tools/export_saf_hoa_matrix.cpp",
                            sha256=hashlib.sha256(data).hexdigest()))
    (args.output / "manifest.json").write_text(json.dumps(dict(
        source_version="Spatial_Audio_Framework v1.3.4", format="IEEE754 binary32 little endian",
        kemar_sample_rate=48000, assets=records), indent=2) + "\n")


if __name__ == "__main__":
    main()
