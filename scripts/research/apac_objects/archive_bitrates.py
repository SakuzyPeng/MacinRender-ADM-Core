#!/usr/bin/env python3
"""Archive validated bitrate evidence and synthetic compressed samples, without PCM caches."""

import argparse
import hashlib
import json
from pathlib import Path
from zipfile import ZIP_DEFLATED, ZipFile


def archive(root, destination):
    checked = json.loads((root / "verification.json").read_text())
    if not checked["all_checks_passed"]:
        raise ValueError("bitrate evidence has not passed verification")
    repository = Path(__file__).resolve().parents[3]
    files = {}
    for path in Path(__file__).resolve().parent.iterdir():
        if path.suffix in (".py", ".cpp", ".m", ".md"):
            files[str(path.relative_to(repository))] = path.read_bytes()
    for path in (repository / "docs/architecture").glob("APAC_OBJECT*.md"):
        files[str(path.relative_to(repository))] = path.read_bytes()
    for path in root.glob("*.json"):
        files["results/" + path.name] = path.read_bytes()
    names = [row["case"] for row in json.loads((root / "bitrates.json").read_text())]
    selected = {name + suffix for name in names for suffix in ("-encode", "-decode")}
    selected.update(path.name for pattern in ("allocation-*", "positions-*", "unobserved-*", "build-bitrate-probe-*")
                    for path in (root / "cases").glob(pattern))
    selected.update(("mp4-wrap", "caf-import", "mp4-import"))
    for name in selected:
        for path in (root / "cases" / name).glob("*"):
            if path.is_file() and (path.name in ("result.json", "stdout.txt", "stderr.txt") or path.name.startswith("stream.")):
                files["results/" + str(path.relative_to(root))] = path.read_bytes()
    for pattern in ("inputs/*/expected.json", "inputs/*/settings.plist", "settings/*/settings.plist", "fixtures/*"):
        for path in root.glob(pattern):
            if path.is_file():
                files["results/" + str(path.relative_to(root))] = path.read_bytes()
    manifest = {"scope": "APAC bitrate experiments; system-default decoder; no profile-table modification",
                "excluded": ["system binaries", "build products", "analysis databases", "regenerable raw PCM"],
                "files": [{"path": name, "bytes": len(data), "sha256": hashlib.sha256(data).hexdigest()}
                          for name, data in sorted(files.items())]}
    files["MANIFEST.json"] = (json.dumps(manifest, indent=2) + "\n").encode()
    with ZipFile(destination, "x", compression=ZIP_DEFLATED) as file:
        for name, data in sorted(files.items()):
            file.writestr(name, data)
    with ZipFile(destination) as file:
        if file.testzip() is not None:
            raise ValueError("ZIP CRC verification failed")
        for entry in manifest["files"]:
            data = file.read(entry["path"])
            if len(data) != entry["bytes"] or hashlib.sha256(data).hexdigest() != entry["sha256"]:
                raise ValueError("manifest verification failed")
    digest = hashlib.sha256(destination.read_bytes()).hexdigest()
    destination.with_suffix(destination.suffix + ".sha256").write_text(digest + "  " + destination.name + "\n")
    print(json.dumps({"archive": str(destination), "files": len(files), "bytes": destination.stat().st_size, "sha256": digest}))


if __name__ == "__main__":
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--results", type=Path, required=True)
    parser.add_argument("--output", type=Path, required=True)
    args = parser.parse_args()
    archive(args.results.resolve(), args.output.resolve())
