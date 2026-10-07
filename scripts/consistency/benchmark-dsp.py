#!/usr/bin/env python3
"""Compare two Release CLIs on small deterministic ADM fixtures (macOS/Linux).

No large audio is retained. Runs alternate implementations and include warm-up;
the JSON keeps individual measurements, medians, binary hashes and arguments.
"""
import argparse
import hashlib
import json
import os
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
    parser.add_argument('--legacy-runtime', type=Path, help='directory of frozen shared libraries for the old CLI')
    parser.add_argument('--candidate-runtime', type=Path, help='directory of shared libraries for the new CLI')
    parser.add_argument('--pcm-tool', type=Path, help='Release mr_adm_pcm_bits; records canonical PCM hashes')
    parser.add_argument('--require-identical-pcm', action='store_true')
    args = parser.parse_args()
    if args.repeats < 3:
        parser.error('at least three measured repetitions are required')
    if args.require_identical_pcm and not args.pcm_tool:
        parser.error('--require-identical-pcm requires --pcm-tool')
    if platform.system() not in {'Darwin', 'Linux'}:
        parser.error('RSS collector currently supports macOS and Linux')
    args.output.parent.mkdir(parents=True, exist_ok=True)
    result = {'platform': platform.platform(), 'machine': platform.machine(), 'repeats': args.repeats,
              'binaries': {name: hashlib.sha256(path.read_bytes()).hexdigest()
                           for name, path in [('legacy', args.legacy), ('candidate', args.candidate)]},
              'runtime_libraries': {}, 'cases': []}
    environments = {}
    launch_prefixes = {}
    for name, directory in [('legacy', args.legacy_runtime), ('candidate', args.candidate_runtime)]:
        env = os.environ.copy()
        if args.legacy_runtime or args.candidate_runtime:
            env.pop('DYLD_LIBRARY_PATH', None)
            env.pop('LD_LIBRARY_PATH', None)
        if directory:
            if not directory.is_dir():
                parser.error(f'runtime directory does not exist: {directory}')
            variable = 'DYLD_LIBRARY_PATH' if platform.system() == 'Darwin' else 'LD_LIBRARY_PATH'
            # macOS /usr/bin/time removes DYLD_* from its inherited environment.
            # Apply the override after time starts, as explicit arguments to env.
            launch_prefixes[name] = ['/usr/bin/env', variable + '=' + str(directory.resolve())]
            result['runtime_libraries'][name] = {
                p.name: hashlib.sha256(p.read_bytes()).hexdigest()
                for p in sorted(directory.iterdir()) if p.is_file() and ('.so' in p.name or p.suffix == '.dylib')}
        environments[name] = env
    with tempfile.TemporaryDirectory(prefix='dsp-bench-', dir=args.output.parent) as scratch:
        scratch = Path(scratch)
        for case, fixture, options in CASES:
            samples = {'legacy': [], 'candidate': []}
            pcm_hashes = {}
            for run in range(args.repeats + 1):
                sequence = [('legacy', args.legacy), ('candidate', args.candidate)]
                if run % 2:
                    sequence.reverse()
                for name, binary in sequence:
                    wave = scratch / 'render.wav'
                    command = ['/usr/bin/time', '-l' if platform.system() == 'Darwin' else '-v',
                               *launch_prefixes.get(name, []),
                               str(binary.resolve()), 'render', '-i', str((args.fixtures / (fixture + '.wav')).resolve()),
                               '-o', str(wave.resolve()), '--output-bit-depth', 'f32', '--no-peak-limit', *options]
                    started = time.perf_counter()
                    completed = subprocess.run(command, capture_output=True, text=True, encoding='utf-8',
                                               errors='replace', env=environments[name])
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
                    if args.pcm_tool:
                        pcm = scratch / 'render.pcmbits'
                        subprocess.run([str(args.pcm_tool.resolve()), 'extract', str(wave), str(pcm)],
                                       check=True, capture_output=True)
                        pcm_hash = hashlib.sha256(pcm.read_bytes()).hexdigest()
                        if name in pcm_hashes and pcm_hashes[name] != pcm_hash:
                            raise RuntimeError(f'{case}/{name}: PCM changed between repetitions')
                        pcm_hashes[name] = pcm_hash
                        pcm.unlink()
                    wave.unlink()
            if args.require_identical_pcm and pcm_hashes['legacy'] != pcm_hashes['candidate']:
                raise RuntimeError(f'{case}: optimized PCM differs from baseline')
            row = {'case': case, 'fixture_sha256': hashlib.sha256((args.fixtures / (fixture + '.wav')).read_bytes()).hexdigest(),
                   'arguments': options, 'runs': samples, 'pcm_sha256': pcm_hashes}
            for name in samples:
                row[name] = {key: statistics.median(run[key] for run in samples[name]) for key in ['seconds', 'rss_bytes']}
            row['time_ratio'] = row['candidate']['seconds'] / row['legacy']['seconds']
            row['rss_ratio'] = row['candidate']['rss_bytes'] / row['legacy']['rss_bytes']
            result['cases'].append(row)
            print(f"{case}: time={row['time_ratio']:.3f}x RSS={row['rss_ratio']:.3f}x", flush=True)
    args.output.write_text(json.dumps(result, indent=2) + '\n')


if __name__ == '__main__':
    main()
