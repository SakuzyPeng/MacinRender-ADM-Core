#!/usr/bin/env python3
"""Build a frozen unpatched FFT probe in place, or compare two Release probes.

Reference preparation temporarily removes only the RustFFT Cargo patch, restores
both manifests even on failure, then reconfigures the existing CMake build. The
next normal build produces the candidate; no checkout or dependency cache is added.
"""
import argparse
from contextlib import contextmanager
import hashlib
import json
import math
import os
from pathlib import Path
import re
import shutil
import statistics
import subprocess

from build_info import read_cache

ROOT = Path(__file__).resolve().parents[2]
PATCH = b'rustfft = { path = "vendor/rustfft" }'
LENGTHS = [128, 256, 512, 1024, 2048, 4096, 8192, 16384, 32768]


def sha(path):
    return hashlib.sha256(path.read_bytes()).hexdigest()


def save(path, value):
    path.parent.mkdir(parents=True, exist_ok=True)
    path.write_text(json.dumps(value, indent=2, allow_nan=False) + '\n', encoding='utf-8')


@contextmanager
def unpatched_manifest(root):
    manifest, lock = root / 'rust/Cargo.toml', root / 'rust/Cargo.lock'
    original, original_lock = manifest.read_bytes(), lock.read_bytes()
    if original.count(PATCH) != 1:
        raise ValueError('expected exactly one local RustFFT patch; refusing to change manifests')
    try:
        manifest.write_bytes(original.replace(PATCH, b''))
        yield
    finally:
        manifest.write_bytes(original)
        lock.write_bytes(original_lock)


def prepare(build, out):
    cache = read_cache(build / 'CMakeCache.txt')
    if cache.get('CMAKE_BUILD_TYPE') != 'Release' or cache.get('MR_ADM_CONSISTENCY_DIAGNOSTICS') != 'OFF':
        raise ValueError('FFT timing requires Release with diagnostics OFF')
    if out.exists() and any(out.iterdir()):
        raise ValueError('reference directory must be empty')
    out.mkdir(parents=True, exist_ok=True)
    executable = 'mr_adm_fft_benchmark' + ('.exe' if os.name == 'nt' else '')
    original_lock = (ROOT / 'rust/Cargo.lock').read_text(encoding='utf-8')
    try:
        with unpatched_manifest(ROOT):
            subprocess.run([cache['Rust_CARGO_CACHED'], 'metadata', '--manifest-path',
                            str(ROOT / 'rust/Cargo.toml'), '--format-version', '1', '--no-default-features'],
                           stdout=subprocess.DEVNULL, check=True)
            reference_lock = (ROOT / 'rust/Cargo.lock').read_text(encoding='utf-8')
            if other_packages(original_lock) != other_packages(reference_lock):
                raise ValueError('reference preparation changed a dependency other than RustFFT')
            subprocess.run(['cmake', '-S', str(ROOT), '-B', str(build)], check=True)
            subprocess.run(['cmake', '--build', str(build), '--target', 'mr_adm_fft_benchmark',
                            '--parallel', '4'], check=True)
            shutil.copy2(build / executable, out / executable)
            record = {'kind': 'unpatched-rustfft-6.4.1-scalar', 'executable': executable,
                      'sha256': sha(out / executable), 'cargo_lock_sha256': sha(ROOT / 'rust/Cargo.lock'),
                      'configuration': 'Release', 'diagnostics': False,
                      'rustc': subprocess.check_output([cache['Rust_COMPILER_CACHED'], '-vV'], text=True).strip()}
    finally:
        # Source and generated metadata must describe the candidate again, even
        # when the reference build failed. The caller must rebuild its targets.
        subprocess.run(['cmake', '-S', str(ROOT), '-B', str(build)], check=True)
    save(out / 'reference.json', record)


def other_packages(lock):
    """Cargo writes the lock; verify that unrelated packages stayed untouched."""
    return [block for block in re.split(r'(?m)^\[\[package\]\]\s*$', lock)
            if not re.search(r'(?m)^name = "rustfft"$', block)]


def measurements(report):
    rows = report.get('measurements', [])
    if report.get('schema') != 'mradm.fft.benchmark.v1' or [r.get('length') for r in rows] != LENGTHS:
        raise ValueError('invalid FFT benchmark schema or length inventory')
    for row in rows:
        if row.get('iterations') != 20000000 // row['length']:
            raise ValueError('invalid FFT iteration count')
        for field in ('forward_ns', 'inverse_ns'):
            value = row.get(field)
            if not isinstance(value, (float, int)) or not math.isfinite(value) or value <= 0:
                raise ValueError('invalid FFT duration')
        for field in ('spectrum_fnv1a64', 'inverse_fnv1a64'):
            value = row.get(field)
            if not isinstance(value, str) or not value.isascii() or not value.isdecimal() or int(value) >= 2**64:
                raise ValueError('invalid FFT bit fingerprint')
    return rows


def compare(reference, candidate, output, repeats):
    if repeats < 3:
        raise ValueError('at least three measured repetitions are required')
    record = json.loads((reference / 'reference.json').read_text(encoding='utf-8'))
    before = reference / record['executable']
    if record.get('kind') != 'unpatched-rustfft-6.4.1-scalar' or sha(before) != record['sha256']:
        raise ValueError('reference binary provenance mismatch')
    runs = {'reference': [], 'candidate': []}
    expected = None
    for trial in range(repeats + 1):
        sequence = [('reference', before), ('candidate', candidate)]
        if trial % 2:
            sequence.reverse()
        for name, binary in sequence:
            rows = measurements(json.loads(subprocess.check_output([str(binary.resolve())], text=True)))
            fingerprints = [(r['spectrum_fnv1a64'], r['inverse_fnv1a64']) for r in rows]
            if expected is None:
                expected = fingerprints
            if fingerprints != expected:
                raise ValueError('FFT bits changed across implementations or repetitions')
            if trial:
                runs[name].append(rows)
    summary = []
    for i, length in enumerate(LENGTHS):
        row = {'length': length}
        for direction in ('forward_ns', 'inverse_ns'):
            a = statistics.median(r[i][direction] for r in runs['reference'])
            b = statistics.median(r[i][direction] for r in runs['candidate'])
            row[direction] = {'reference': a, 'candidate': b, 'ratio': b / a}
        summary.append(row)
    save(output, {'schema': 'mradm.fft.performance.v1', 'reference': record,
                  'candidate_sha256': sha(candidate), 'warmup_passes': 1, 'measured_passes': repeats,
                  'bit_fingerprints_equal': True, 'summary': summary, 'runs': runs})
    for row in summary:
        print(f"FFT {row['length']}: forward {row['forward_ns']['ratio']:.3f}x, inverse {row['inverse_ns']['ratio']:.3f}x")


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    commands = parser.add_subparsers(dest='command', required=True)
    reference = commands.add_parser('prepare-reference')
    reference.add_argument('build', type=Path)
    reference.add_argument('output', type=Path)
    comparison = commands.add_parser('compare')
    comparison.add_argument('reference', type=Path)
    comparison.add_argument('candidate', type=Path)
    comparison.add_argument('output', type=Path)
    comparison.add_argument('--repeats', type=int, default=7)
    args = parser.parse_args()
    if args.command == 'prepare-reference':
        prepare(args.build.resolve(), args.output.resolve())
    else:
        compare(args.reference, args.candidate, args.output, args.repeats)


if __name__ == '__main__':
    main()
