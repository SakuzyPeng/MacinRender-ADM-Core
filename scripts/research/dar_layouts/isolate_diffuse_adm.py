#!/usr/bin/env python3
"""Create a new final ADM BWF that isolates extent from Dolby's diffuse flag.

The Atmos Conversion Tool sets diffuse=1 whenever the DAMF size is nonzero.
Its ADM output is therefore unsuitable for attributing decorrelation to size.
This controlled variant changes only the same-length AXML diffuse value; PCM,
chunk lengths, positions, extents and timings remain byte-identical.
"""

import argparse
import json
import subprocess
from pathlib import Path
from xml.etree import ElementTree

from measure_static_bank import file_sha256

HERE = Path(__file__).resolve().parent
RENDER = HERE.parents[2] / "build/release/mradm"
OLD = b"<diffuse>1.000000</diffuse>"
NEW = b"<diffuse>0.000000</diffuse>"


def read_xml(path: Path) -> ElementTree.Element:
    result = subprocess.run([str(RENDER), "inspect", "--xml", str(path)],
                            check=True, capture_output=True, text=True)
    return ElementTree.fromstring(result.stdout)


def block_fields(root: ElementTree.Element) -> list[dict]:
    rows = []
    for channel in root.iter():
        if not channel.tag.endswith("audioChannelFormat") or channel.attrib.get("typeDefinition") != "Objects":
            continue
        for block in channel:
            if not block.tag.endswith("audioBlockFormat"):
                continue
            fields = {"rtime": block.attrib.get("rtime"), "duration": block.attrib.get("duration")}
            for child in block:
                key = child.tag.rsplit("}", 1)[-1]
                if key == "position":
                    key += child.attrib["coordinate"]
                fields[key] = child.text
            rows.append(fields)
    return rows


def main() -> None:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--input", type=Path, required=True)
    parser.add_argument("--output-dir", type=Path, required=True)
    args = parser.parse_args()
    source = args.input.expanduser().resolve()
    output_dir = args.output_dir.expanduser().resolve()
    if output_dir.exists():
        parser.error(f"output directory exists: {output_dir}")
    raw = source.read_bytes()
    count = raw.count(OLD)
    if count == 0 or len(OLD) != len(NEW):
        raise ValueError("expected one or more fixed-length diffuse=1 AXML fields")
    output_dir.mkdir(parents=True)
    result = output_dir / "output.wav"
    result.write_bytes(raw.replace(OLD, NEW))
    before = block_fields(read_xml(source))
    after = block_fields(read_xml(result))
    if len(before) != len(after):
        raise ValueError("Objects block count changed")
    for old, new in zip(before, after):
        if any(value != new.get(key) for key, value in old.items() if key != "diffuse"):
            raise ValueError("a non-diffuse ADM field changed")
        if old.get("diffuse") == "1.000000" and new.get("diffuse") != "0.000000":
            raise ValueError("diffuse was not cleared")
    report = {"source": str(source), "source_sha256": file_sha256(source),
              "adm": str(result), "sha256": file_sha256(result),
              "changed_diffuse_fields": count, "object_blocks": len(before),
              "same_byte_length": result.stat().st_size == source.stat().st_size}
    (output_dir / "manifest.json").write_text(json.dumps(report, indent=2) + "\n", encoding="utf-8")
    print(output_dir / "manifest.json")


if __name__ == "__main__":
    main()
