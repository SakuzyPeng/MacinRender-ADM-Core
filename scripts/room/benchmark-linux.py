#!/usr/bin/env python3
"""Run room-only Release probes sequentially and retain Linux scheduling evidence."""
import argparse
import hashlib
import json
import os
from pathlib import Path
import platform
import resource
import subprocess
import time


def read(path):
    try:
        return Path(path).read_text()
    except OSError:
        return None


def counters():
    return {
        'cgroup_cpu_stat': read('/sys/fs/cgroup/cpu.stat'),
        'cpu_pressure': read('/proc/pressure/cpu'),
        'system_stat': read('/proc/stat'),
    }


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('binary', type=Path)
    parser.add_argument('room', type=Path)
    parser.add_argument('output', type=Path)
    parser.add_argument('--workers', type=int, nargs='+', default=[8, 1, 4, 16, 32])
    parser.add_argument('--repeats', type=int, default=3)
    parser.add_argument('--blocks', type=int, default=938)
    parser.add_argument('--frames', type=int, choices=[256, 512], default=512)
    parser.add_argument('--sources', type=int, choices=[1, 16, 32], default=32)
    parser.add_argument('--switch-period', type=int, default=0, help='Calls between repeated switches; 0 switches once')
    args = parser.parse_args()
    if platform.system() != 'Linux':
        parser.error('This collector requires Linux; use native desktop benchmarks elsewhere.')
    if (args.repeats < 1 or not 1 <= args.blocks <= 112500 or args.switch_period < 0
            or len(set(args.workers)) != len(args.workers)
            or any(n < 1 or n > 32 for n in args.workers)):
        parser.error('Invalid repeat, block or worker count')
    args.output.mkdir(parents=True, exist_ok=False)
    binary, room = args.binary.resolve(), args.room.resolve()
    environment = {
        'schema': 'mradm.room-linux-benchmark', 'version': 1,
        'uname': list(platform.uname()), 'cpuinfo': read('/proc/cpuinfo'),
        'cpu_max': read('/sys/fs/cgroup/cpu.max'),
        'cpuset': read('/sys/fs/cgroup/cpuset.cpus.effective'),
        'affinity': sorted(os.sched_getaffinity(0)),
        'binary_sha256': hashlib.sha256(binary.read_bytes()).hexdigest(),
        'room_manifest_sha256': hashlib.sha256(room.read_bytes()).hexdigest(),
        'config': {k: str(v) if isinstance(v, Path) else v for k, v in vars(args).items()},
        'note': 'Child rusage is a per-run delta; proc/cgroup counters also include other jobs. Stage wall times overlap.',
    }
    (args.output / 'environment.json').write_text(json.dumps(environment, indent=2) + '\n')
    for workers in args.workers:
        for repeat in range(args.repeats):
            name = f'workers-{workers}-run-{repeat+1}'
            print('Starting ' + name, flush=True)
            before = counters()
            usage = resource.getrusage(resource.RUSAGE_CHILDREN)
            start = time.monotonic()
            result = subprocess.run(
                [str(binary), str(room), str(args.blocks), str(args.frames), str(args.sources), str(args.switch_period)],
                env={**os.environ, 'MR_ADM_ROOM_WORKERS': str(workers)},
                capture_output=True, text=True, check=False,
            )
            elapsed = time.monotonic() - start
            after = resource.getrusage(resource.RUSAGE_CHILDREN)
            record = {
                'workers': workers, 'repeat': repeat+1, 'returncode': result.returncode,
                'wall_seconds': elapsed, 'before': before, 'after': counters(),
                'rusage_delta': {key: getattr(after, key)-getattr(usage, key) for key in
                                 ('ru_utime', 'ru_stime', 'ru_minflt', 'ru_majflt', 'ru_nvcsw', 'ru_nivcsw')},
            }
            (args.output / (name + '.jsonl')).write_text(result.stdout)
            (args.output / (name + '.stderr')).write_text(result.stderr)
            (args.output / (name + '.environment.json')).write_text(json.dumps(record, indent=2) + '\n')
            if result.returncode:
                raise SystemExit(f'{name} failed: see saved stderr')
            rows = [json.loads(line) for line in result.stdout.splitlines()]
            if any(row.get('profiling') is not False for row in rows):
                raise SystemExit('Capacity runs require rebuilding room_bench without room-profile')
            if len(rows) != 4 or any(row['sources'] != args.sources for row in rows):
                raise SystemExit('Unexpected benchmark schema; use the current room_bench executable')
            print(name + ': ' + ', '.join(f"P99={row['p99_ms']:.3f}ms misses={row['deadline_misses']}" for row in rows), flush=True)


if __name__ == '__main__':
    main()
