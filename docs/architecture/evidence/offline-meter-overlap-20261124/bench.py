#!/usr/bin/env python3
"""Alternate two Release `mradm` runs on full CLI renders (Linux, GNU time).

Each side is `--build NAME=BINARY[,PRELOAD]`; a side with a PRELOAD runs under
LD_PRELOAD (e.g. no_sigint.so, which keeps the CLI from starting its Ctrl-C
watcher). Each case is warmed up once per side, then measured for --rounds
rounds with the side order reversed every round. Wall time comes from
perf_counter around /usr/bin/time, which reports the peak RSS. Afterwards each
side renders each case once more with -v: the final PCM bit images
(mr_adm_pcm_bits) and the measured loudness / true-peak log lines must be
identical.
"""
import argparse
import hashlib
import json
import os
import platform
import statistics
import subprocess
import tempfile
import time
from pathlib import Path

CASES = [
    ('vbap-714', 'objects-long', ['--renderer', 'saf', '--output-layout', '7.1.4']),
    ('ear-714', 'objects-long', ['--renderer', 'ear', '--output-layout', '7.1.4']),
    ('hoa3', 'objects-long', ['--renderer', 'hoa', '--output-layout', 'hoa3']),
    ('triple-balance-714', 'objects-room', ['--renderer', 'triple-balance', '--output-layout', '7.1.4']),
]


def sha256(path):
    digest = hashlib.sha256()
    with open(path, 'rb') as f:
        while block := f.read(1 << 20):
            digest.update(block)
    return digest.hexdigest()


def command(binary, preload, args):
    # Both sides go through env, so only LD_PRELOAD differs; time is never preloaded.
    return ['/usr/bin/env', f'LD_PRELOAD={preload.resolve() if preload else ""}', str(binary), *args]


def run(binary, preload, args, scratch):
    """Wall time around GNU time, which reports the CLI's peak RSS. A wait4 from this process
    would not: a vfork-spawned child takes over the parent's high-water RSS at exec."""
    usage = scratch / 'maxrss'
    # The CLI logs to stdout; a file rather than a pipe keeps a full pipe from stalling it.
    with tempfile.TemporaryFile() as log:
        start = time.perf_counter()
        status = subprocess.run(['/usr/bin/time', '-f', '%M', '-o', str(usage), *command(binary, preload, args)],
                                stdout=log, stderr=subprocess.STDOUT).returncode
        elapsed = time.perf_counter() - start
        if status != 0:
            log.seek(0)
            raise SystemExit(f'{binary} {args} exited {status}: {log.read().decode("utf-8", "replace")}')
    return elapsed * 1000.0, int(usage.read_text().split()[-1]) / 1024.0


def side(text):
    name, _, spec = text.partition('=')
    binary, _, preload = spec.partition(',')
    if not name or not binary:
        raise argparse.ArgumentTypeError('expected NAME=BINARY[,PRELOAD]')
    return name, Path(binary), Path(preload) if preload else None


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--build', type=side, action='append', required=True)
    parser.add_argument('--fixtures', type=Path, required=True)
    parser.add_argument('--pcm-tool', type=Path, required=True)
    parser.add_argument('--output', type=Path, required=True)
    parser.add_argument('--rounds', type=int, default=12)
    parser.add_argument('--case', action='append', choices=[c for c, _, _ in CASES])
    args = parser.parse_args()
    if len(args.build) != 2 or args.build[0][0] == args.build[1][0]:
        parser.error('give two --build sides with distinct names')
    names = [name for name, _, _ in args.build]
    binaries = {name: binary for name, binary, _ in args.build}
    preloads = {name: preload for name, _, preload in args.build}
    result = {'platform': platform.platform(), 'machine': platform.machine(), 'cpus': os.cpu_count(),
              'rounds': args.rounds,
              'sides': {name: {'binary_sha256': sha256(binary),
                               'preload': {'name': preload.name, 'sha256': sha256(preload)} if preload else None}
                        for name, binary, preload in args.build},
              'cases': []}
    with tempfile.TemporaryDirectory(prefix='meter-overlap-', dir=args.output.parent) as scratch:
        scratch = Path(scratch)
        for case, fixture, options in CASES:
            if args.case and case not in args.case:
                continue
            source = args.fixtures / f'{fixture}.wav'
            argv = {name: ['render', '-i', str(source), '-o', str(scratch / f'{case}-{name}.wav'), *options]
                    for name in names}
            for name in names:
                run(binaries[name], preloads[name], argv[name], scratch)
            rounds = []
            for r in range(args.rounds):
                order = names if r % 2 == 0 else names[::-1]
                row = {'order': order}
                for name in order:
                    ms, rss = run(binaries[name], preloads[name], argv[name], scratch)
                    row[name] = {'ms': round(ms, 3), 'rss_mib': round(rss, 2)}
                rounds.append(row)
            identity = {}
            for name in names:
                log = subprocess.run(command(binaries[name], preloads[name], ['render', '-v', *argv[name][1:]]),
                                     capture_output=True, text=True, check=True)
                meters = [line.split('] ', 2)[-1] for line in log.stdout.splitlines()
                          if 'measured loudness' in line or 'measured true peak' in line]
                if len(meters) != 2:
                    raise SystemExit(f'{case}: expected the loudness and true-peak lines, got {meters}')
                image = scratch / f'{case}-{name}.bits'
                subprocess.run([str(args.pcm_tool), 'extract', str(scratch / f'{case}-{name}.wav'), str(image)],
                               check=True, capture_output=True)
                identity[name] = {'pcm_bits_sha256': sha256(image), 'meters': meters}
            if identity[names[0]] != identity[names[1]]:
                raise SystemExit(f'{case}: outputs differ: {json.dumps(identity, indent=1)}')
            medians = {name: {key: statistics.median(row[name][key] for row in rounds) for key in ('ms', 'rss_mib')}
                       for name in names}
            ratio = medians[names[1]]['ms'] / medians[names[0]]['ms']
            result['cases'].append({'case': case, 'fixture': fixture, 'fixture_sha256': sha256(source),
                                    'options': options, 'medians': medians, 'time_ratio': round(ratio, 4),
                                    'identical': identity[names[0]], 'rounds': rounds})
            a, b = (medians[name] for name in names)
            print(f"{case}: {names[0]} {a['ms']:.1f} ms, {names[1]} {b['ms']:.1f} ms ({(ratio - 1) * 100:+.1f}%), "
                  f"RSS {a['rss_mib']:.1f} -> {b['rss_mib']:.1f} MiB; PCM and meters identical", flush=True)
    args.output.write_text(json.dumps(result, indent=1) + '\n')


if __name__ == '__main__':
    main()
