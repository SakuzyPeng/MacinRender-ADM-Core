#!/usr/bin/env python3
"""Sample the RSS composition of `mradm render` (Linux).

`rss_split.py FIXTURE ROUNDS OUTPUT NAME=BINARY...` renders FIXTURE to VBAP 7.1.4 with each
binary per round and reverses the order every round. It samples /proc/<pid>/status with
0.5 ms sleeps and keeps the complete sample at the highest observed VmRSS, plus the
highest observed VmHWM. All values are in KiB.
"""
import json
import subprocess
import sys
import tempfile
import time
from pathlib import Path

RSS_KEYS = ('VmRSS', 'RssAnon', 'RssFile', 'RssShmem')
KEYS = (*RSS_KEYS, 'VmHWM')


def peaks(binary, fixture, output):
    command = [binary, 'render', '-i', fixture, '-o', output,
               '--renderer', 'saf', '--output-layout', '7.1.4']
    at_max_vmrss = None
    max_vmhwm = 0
    with subprocess.Popen(command, stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL) as process:
        while process.poll() is None:
            sample = {}
            try:
                with open(f'/proc/{process.pid}/status') as status:
                    for line in status:
                        key, _, value = line.partition(':')
                        if key in KEYS:
                            sample[key] = int(value.split()[0])
            except (FileNotFoundError, ProcessLookupError):
                break  # the process exited between poll() and the read
            if len(sample) == len(KEYS):
                max_vmhwm = max(max_vmhwm, sample['VmHWM'])
                if at_max_vmrss is None or sample['VmRSS'] > at_max_vmrss['VmRSS']:
                    at_max_vmrss = sample
            time.sleep(0.0005)
        if process.wait() != 0:
            raise SystemExit(f'{binary} exited {process.returncode}')
    if at_max_vmrss is None:
        raise SystemExit(f'{binary} produced no complete RSS samples')
    return {'at_max_vmrss': at_max_vmrss, 'max_vmhwm': max_vmhwm}


def main():
    fixture, rounds, output = sys.argv[1], int(sys.argv[2]), Path(sys.argv[3])
    builds = {name: str(Path(binary).resolve())
              for name, binary in (arg.split('=', 1) for arg in sys.argv[4:])}
    runs = {name: [] for name in builds}
    with tempfile.TemporaryDirectory() as scratch:
        for r in range(rounds):
            for name in (list(builds) if r % 2 == 0 else list(builds)[::-1]):
                runs[name].append(peaks(builds[name], fixture, str(Path(scratch) / 'out.wav')))
    for name, samples in runs.items():
        at_peak = [row['at_max_vmrss'] for row in samples]
        print(name, 'at highest sampled VmRSS',
              ' '.join(f'{k} {min(p[k] for p in at_peak) / 1024:.1f}-{max(p[k] for p in at_peak) / 1024:.1f}'
                       for k in RSS_KEYS), 'MiB')
    output.write_text(json.dumps({'fixture': Path(fixture).name, 'unit': 'KiB', 'runs': runs}, indent=1) + '\n')


if __name__ == '__main__':
    main()
