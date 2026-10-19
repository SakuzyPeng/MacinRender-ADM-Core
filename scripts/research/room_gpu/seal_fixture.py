#!/usr/bin/env python3
"""Record hashes after the Release fixture exporter has closed every payload."""
import argparse
import hashlib
import json
from pathlib import Path


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('fixture', type=Path)
    args = parser.parse_args()
    path = args.fixture / 'manifest.json'
    manifest = json.loads(path.read_text(encoding='utf-8'))
    if manifest.get('schema') != 'mradm.room-gpu-fixture' or manifest.get('version') != 1:
        parser.error('Unsupported fixture manifest')
    files = {}
    for payload in sorted(args.fixture.iterdir()):
        if not payload.is_file() or payload.name == path.name:
            continue
        digest = hashlib.sha256()
        with payload.open('rb') as source:
            for chunk in iter(lambda: source.read(1024 * 1024), b''):
                digest.update(chunk)
        files[payload.name] = dict(bytes=payload.stat().st_size, sha256=digest.hexdigest())
    required = {'bank.bin', 'fraction.bin', 'propagation.bin', 'phase.bin', 'materials.bin',
                'jobs.bin', 'ranges.bin', 'reference.bin', 'history.bin', 'previous.bin',
                'audio-reference.bin', 'config.txt'}
    if set(files) != required:
        parser.error('Fixture payload set is incomplete or unexpected')
    manifest['files'] = files
    path.write_text(json.dumps(manifest, indent=2) + '\n', encoding='utf-8')
    print('Sealed', len(files), 'fixture payloads')


if __name__ == '__main__':
    main()
