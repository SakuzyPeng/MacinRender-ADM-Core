#!/usr/bin/env python3
"""Compare two Release CLIs on small deterministic ADM fixtures (macOS/Linux).

No large audio is retained. Runs alternate implementations and include warm-up;
the JSON keeps individual measurements, medians, binary hashes and arguments.
"""
import argparse
import hashlib
import json
import platform
import re
import statistics
import subprocess
import tempfile
import time
from pathlib import Path

CASES = [
    ('ear-extent', 'objects-extent', ['--renderer', 'ear', '--output-layout', '5.1']),
    ('vbap-point', 'objects-cartesian', ['--renderer', 'saf', '--output-layout', '5.1.4']),
    ('binaural-point', 'objects-point', ['--renderer', 'saf-binaural', '--output-layout', 'binaural']),
    ('binaural-cloud', 'objects-extent', ['--renderer', 'saf-binaural', '--output-layout', 'binaural', '--binaural-spread-mode', 'cloud']),
    ('binaural-spreader', 'objects-extent-multi', ['--renderer', 'saf-binaural', '--output-layout', 'binaural', '--binaural-spread-mode', 'saf-spreader']),
]


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--legacy', type=Path, required=True)
    parser.add_argument('--candidate', type=Path, required=True)
    parser.add_argument('--fixtures', type=Path, required=True)
    parser.add_argument('--output', type=Path, required=True)
    parser.add_argument('--repeats', type=int, default=5)
    args = parser.parse_args()
    if args.repeats < 3:
        parser.error('at least three measured repetitions are required')
    if platform.system() not in {'Darwin', 'Linux'}:
        parser.error('RSS collector currently supports macOS and Linux')
    args.output.parent.mkdir(parents=True, exist_ok=True)
    result = {'platform': platform.platform(), 'machine': platform.machine(), 'repeats': args.repeats,
              'binaries': {name: hashlib.sha256(path.read_bytes()).hexdigest()
                           for name, path in [('legacy', args.legacy), ('candidate', args.candidate)]}, 'cases': []}
    with tempfile.TemporaryDirectory(prefix='dsp-bench-', dir=args.output.parent) as scratch:
        scratch = Path(scratch)
        for case, fixture, options in CASES:
            samples = {'legacy': [], 'candidate': []}
            for run in range(args.repeats + 1):
                sequence = [('legacy', args.legacy), ('candidate', args.candidate)]
                if run % 2:
                    sequence.reverse()
                for name, binary in sequence:
                    wave = scratch / 'render.wav'
                    command = ['/usr/bin/time', '-l' if platform.system() == 'Darwin' else '-v',
                               str(binary.resolve()), 'render', '-i', str((args.fixtures / (fixture + '.wav')).resolve()),
                               '-o', str(wave.resolve()), '--output-bit-depth', 'f32', '--no-peak-limit', *options]
                    started = time.perf_counter()
                    completed = subprocess.run(command, capture_output=True, text=True, encoding='utf-8', errors='replace')
                    seconds = time.perf_counter() - started
                    if completed.returncode:
                        raise RuntimeError(f'{case}/{name}: {completed.stderr}')
                    if platform.system() == 'Darwin':
                        match = re.search(r'(\d+)\s+maximum resident set size', completed.stderr)
                        rss = int(match[1])
                    else:
                        match = re.search(r'Maximum resident set size \(kbytes\):\s*(\d+)', completed.stderr)
                        rss = int(match[1]) * 1024
                    if run:
                        samples[name].append({'seconds': seconds, 'rss_bytes': rss})
                    wave.unlink()
            row = {'case': case, 'fixture_sha256': hashlib.sha256((args.fixtures / (fixture + '.wav')).read_bytes()).hexdigest(),
                   'arguments': options, 'runs': samples}
            for name in samples:
                row[name] = {key: statistics.median(run[key] for run in samples[name]) for key in ['seconds', 'rss_bytes']}
            row['time_ratio'] = row['candidate']['seconds'] / row['legacy']['seconds']
            row['rss_ratio'] = row['candidate']['rss_bytes'] / row['legacy']['rss_bytes']
            result['cases'].append(row)
            print(f"{case}: time={row['time_ratio']:.3f}x RSS={row['rss_ratio']:.3f}x", flush=True)
    args.output.write_text(json.dumps(result, indent=2) + '\n')


if __name__ == '__main__':
    main()
