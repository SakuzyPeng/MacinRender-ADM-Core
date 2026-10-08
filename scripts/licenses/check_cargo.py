#!/usr/bin/env python3
"""Check Cargo.lock coverage and the provenance of committed DSP resources.

Uses only the standard library (including on the Windows Python 3.9 host).
The lockfile is Cargo-generated; only canonical package name/version fields
are read here. Cargo itself validates the full TOML during every build.
"""
import hashlib
import json
import re
import sys
from pathlib import Path
import _common


def main():
    root = _common.repo_root()
    lock = (root / 'rust/Cargo.lock').read_text()
    expected = set()
    for block in re.split(r'^\[\[package\]\]\s*$', lock, flags=re.M)[1:]:
        name = re.search(r'^name = "([^"]+)"$', block, re.M).group(1)
        version = re.search(r'^version = "([^"]+)"$', block, re.M).group(1)
        if name not in {'mradm-dsp', 'mradm-adm', 'mradm-wav', 'mradm-ffi', 'mradm-ear', 'mradm-math', 'mradm-osc'}:
            expected.add((name, version))
    actual = {(c['source']['package'], c['version']) for c in _common.load_components()
              if c['source']['type'] == 'cargo'}
    errors = []
    if actual != expected:
        errors.append(f'Cargo manifest mismatch: missing={sorted(expected-actual)}, stale={sorted(actual-expected)}')
    assets = root / 'rust/crates/mradm-dsp/assets'
    manifest = json.loads((assets / 'manifest.json').read_text())
    for entry in manifest['assets']:
        data = (assets / entry['file']).read_bytes()
        count = 1
        for dimension in entry['shape']:
            count *= dimension
        if len(data) != count * 4 or hashlib.sha256(data).hexdigest() != entry['sha256']:
            errors.append('DSP asset size/hash mismatch: ' + entry['file'])
    adm_assets = root / 'rust/crates/mradm-adm/assets'
    adm_manifest = json.loads((adm_assets / 'manifest.json').read_text())
    if hashlib.sha256((adm_assets / adm_manifest['file']).read_bytes()).hexdigest() != adm_manifest['sha256']:
        errors.append('ADM common definitions hash mismatch')
    ear_root = root / 'rust/crates/mradm-ear'
    ear_manifest = json.loads((ear_root / 'PROVENANCE.json').read_text())
    for name, expected_hash in ear_manifest['assets'].items():
        if hashlib.sha256((ear_root / name).read_bytes()).hexdigest() != expected_hash:
            errors.append('EAR asset hash mismatch: ' + name)
    if errors:
        print('\n'.join(errors), file=sys.stderr)
        return 1
    print(f'[INFO] Cargo.lock 覆盖完整（{len(expected)} 项）；DSP 与 ADM 资源哈希正确')
    return 0


if __name__ == '__main__':
    raise SystemExit(main())
