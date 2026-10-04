#!/usr/bin/env python3
"""Compare pre/post migration Release CLIs on deterministic binaural PCM.

Inputs are the fixtures produced by render-matrix.sh / mr_adm_make_fixture.
Temporary WAV/PCM files are removed; only numerical results and hashes remain.
Dynamic filter/metadata transitions are covered separately by the convolution,
continuity and stream fixture tests, not these stationary CLI cases.
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
    ('point', 'objects-point', []),
    ('point-head-pose', 'objects-point', ['--listener-yaw', '37', '--listener-pitch', '12']),
    ('cloud-diffuse', 'objects-extent', ['--binaural-spread-mode', 'cloud']),
    ('multi-cloud-diffuse', 'objects-extent-multi', ['--binaural-spread-mode', 'cloud']),
    ('multi-spreader-diffuse', 'objects-extent-multi', ['--binaural-spread-mode', 'saf-spreader']),
    ('cartesian-cloud', 'objects-cartesian', ['--binaural-spread-mode', 'cloud']),
    ('direct-speakers', 'directspeakers', []),
    ('cloud-window', 'objects-extent', ['--binaural-spread-mode', 'cloud', '--start', '0.037', '--end', '0.193']),
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
              'maximum_absolute_tolerance': 2e-6,
              'binary_sha256': {key: hashlib.sha256(getattr(args, key).read_bytes()).hexdigest()
                                for key in ['reference', 'candidate']}, 'cases': []}
    with tempfile.TemporaryDirectory(prefix='binaural-compare-', dir=args.output.parent) as temp:
        temp = Path(temp)
        for name, fixture, options in CASES:
            source = args.fixtures / (fixture + '.wav')
            rendered = []
            for binary in [args.reference, args.candidate]:
                run([binary.resolve(), 'render', '-i', source.resolve(), '-o', temp / 'audio.wav',
                     '--renderer', 'saf-binaural', '--output-layout', 'binaural',
                     '--output-bit-depth', 'f32', '--no-peak-limit', *options])
                run([args.pcm_bits.resolve(), 'extract', temp / 'audio.wav', temp / 'audio.pcmbits'])
                rendered.append(pcm(temp / 'audio.pcmbits'))
            (shape_a, bits_a, a), (shape_b, bits_b, b) = rendered
            if shape_a != shape_b or not a:
                raise ValueError(f'{name}: changed or empty audio format/length')
            errors = [x - y for x, y in zip(a, b)]
            maximum = max(abs(value) for value in errors)
            rms = math.sqrt(math.fsum(value * value for value in errors) / len(errors))
            report['cases'].append({'case': name, 'fixture': fixture, 'arguments': options,
                                    'fixture_sha256': hashlib.sha256(source.read_bytes()).hexdigest(),
                                    'channels': shape_a[0], 'sample_rate': shape_a[1], 'frames': shape_a[2],
                                    'max_absolute_error': maximum, 'rms_error': rms,
                                    'bit_identical': bits_a == bits_b,
                                    'reference_pcm_sha256': hashlib.sha256(bits_a).hexdigest(),
                                    'candidate_pcm_sha256': hashlib.sha256(bits_b).hexdigest(),
                                    'passed': maximum <= report['maximum_absolute_tolerance']})
            print(f'{name}: max={maximum:.9g}, rms={rms:.9g}, identical={bits_a == bits_b}', flush=True)
    report['passed'] = all(case['passed'] for case in report['cases'])
    args.output.write_text(json.dumps(report, indent=2) + '\n')
    return 0 if report['passed'] else 1


if __name__ == '__main__':
    sys.exit(main())
