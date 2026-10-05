#!/usr/bin/env python3
"""Compare pre/post PCM mixer Release CLIs using deterministic ADM fixtures.

Only summaries and hashes are retained. Dynamic metadata, gain precision,
checkpoint/seek and mixed-row ordering are covered by the native probes/fixtures.
"""
import argparse
import array
import hashlib
import json
import math
import platform
import struct
import subprocess
import sys
import tempfile
from pathlib import Path

CASES = [
    ('ear-point', 'objects-point', ['--renderer', 'ear', '--output-layout', '7.1.4']),
    ('ear-extent', 'objects-extent-multi', ['--renderer', 'ear', '--output-layout', '9.1.6']),
    ('ear-cartesian', 'objects-cartesian', ['--renderer', 'ear', '--output-layout', '5.1']),
    ('ear-bed', 'directspeakers', ['--renderer', 'ear', '--output-layout', '7.1.4']),
    ('ear-hoa', 'hoa', ['--renderer', 'ear', '--output-layout', '7.1.4']),
    ('ear-window', 'objects-extent', ['--renderer', 'ear', '--output-layout', '7.1.4', '--object-smoothing-frames', '2048', '--start', '0.037', '--end', '0.193']),
    ('vbap-point', 'objects-point', ['--renderer', 'saf', '--output-layout', '7.1.4']),
    ('vbap-extent', 'objects-extent-multi', ['--renderer', 'saf', '--output-layout', '9.1.6']),
    ('vbap-bed', 'directspeakers', ['--renderer', 'saf', '--output-layout', '22.2']),
    ('vbap-window', 'objects-cartesian', ['--renderer', 'saf', '--output-layout', '5.1', '--start', '0.037', '--end', '0.193']),
    ('triple-point', 'objects-cartesian', ['--renderer', 'triple-balance', '--output-layout', '7.1.4', '--speaker-spread-mode', 'none']),
    ('triple-window', 'objects-cartesian', ['--renderer', 'triple-balance', '--output-layout', '9.1.6', '--speaker-spread-mode', 'none', '--start', '0.037', '--end', '0.193']),
    ('triple-222', 'objects-cartesian', ['--renderer', 'triple-balance', '--output-layout', '22.2', '--speaker-spread-mode', 'none']),
]



def run(arguments):
    result = subprocess.run([str(arg) for arg in arguments], capture_output=True, text=True, encoding='utf-8', errors='replace')
    if result.returncode:
        raise RuntimeError(f'{arguments[0]} failed: {result.stdout}\n{result.stderr}')


def pcm(path):
    data = path.read_bytes()
    magic, version, channels, rate, frames = struct.unpack('<4sIIIQ', data[:24])
    if magic != b'MRPB' or version != 1 or not channels or len(data) != 24 + frames * channels * 4:
        raise ValueError('Invalid canonical PCM image')
    values = array.array('f', data[24:])
    if sys.byteorder != 'little':
        values.byteswap()
    if not all(math.isfinite(value) for value in values):
        raise ValueError('Non-finite rendered sample')
    return (channels, rate, frames), data[24:], values


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    for name in ['reference', 'candidate', 'pcm-bits', 'fixtures', 'output']:
        parser.add_argument('--' + name, type=Path, required=True)
    args = parser.parse_args()
    args.output.parent.mkdir(parents=True, exist_ok=True)
    report = {'platform': platform.platform(), 'machine': platform.machine(),
              'tolerance': '2e-6 + 2e-6 * abs(reference)',
              'binary_sha256': {key: hashlib.sha256(getattr(args, key).read_bytes()).hexdigest()
                                for key in ['reference', 'candidate']}, 'cases': []}
    with tempfile.TemporaryDirectory(prefix='pcm-mix-compare-', dir=args.output.parent) as temp:
        temp = Path(temp)
        for name, fixture, options in CASES:
            source = args.fixtures / (fixture + '.wav')
            rendered = []
            for binary in [args.reference, args.candidate]:
                run([binary.resolve(), 'render', '-i', source.resolve(), '-o', temp / 'audio.wav',
                     '--output-bit-depth', 'f32', '--no-peak-limit', *options])
                run([args.pcm_bits.resolve(), 'extract', temp / 'audio.wav', temp / 'audio.pcmbits'])
                rendered.append(pcm(temp / 'audio.pcmbits'))
            (shape_a, bits_a, a), (shape_b, bits_b, b) = rendered
            if shape_a != shape_b or not a:
                raise ValueError(f'{name}: changed or empty audio format/length')
            errors = [x - y for x, y in zip(a, b)]
            maximum = max(abs(value) for value in errors)
            normalized = max(abs(x-y)/(1.0+abs(x)) for x,y in zip(a,b))
            rms = math.sqrt(math.fsum(value * value for value in errors) / len(errors))
            report['cases'].append({'case': name, 'fixture': fixture, 'arguments': options,
                                    'fixture_sha256': hashlib.sha256(source.read_bytes()).hexdigest(),
                                    'channels': shape_a[0], 'sample_rate': shape_a[1], 'frames': shape_a[2],
                                    'max_absolute_error': maximum, 'max_normalized_error': normalized, 'rms_error': rms,
                                    'bit_identical': bits_a == bits_b,
                                    'reference_pcm_sha256': hashlib.sha256(bits_a).hexdigest(),
                                    'candidate_pcm_sha256': hashlib.sha256(bits_b).hexdigest(),
                                    'passed': normalized <= 2e-6})
            print(f'{name}: max={maximum:.9g}, rms={rms:.9g}, identical={bits_a == bits_b}', flush=True)
    report['passed'] = all(case['passed'] for case in report['cases'])
    args.output.write_text(json.dumps(report, indent=2) + '\n')
    return 0 if report['passed'] else 1


if __name__ == '__main__':
    sys.exit(main())
