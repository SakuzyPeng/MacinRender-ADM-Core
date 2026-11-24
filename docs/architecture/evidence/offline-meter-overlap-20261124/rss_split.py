#!/usr/bin/env python3
"""Split the peak RSS of `mradm render` into anonymous and file-backed pages (Linux).

`rss_split.py FIXTURE ROUNDS OUTPUT NAME=BINARY...` renders FIXTURE to VBAP 7.1.4 with each
binary per round, reversing the order every round, and polls /proc/<pid>/status every 0.5 ms
for the peak RssAnon and RssFile and the last VmHWM (KiB).
"""
import json
import subprocess
import sys
import tempfile
import time
from pathlib import Path

KEYS = ('RssAnon', 'RssFile', 'VmHWM')


def peaks(binary, fixture, output):
    process = subprocess.Popen([binary, 'render', '-i', fixture, '-o', output, '--renderer', 'saf',
                                '--output-layout', '7.1.4'], stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
    peak = dict.fromkeys(KEYS, 0)
    while process.poll() is None:
        try:
            with open(f'/proc/{process.pid}/status') as status:
                for line in status:
                    key, _, value = line.partition(':')
                    if key in peak:
                        peak[key] = max(peak[key], int(value.split()[0]))
        except (OSError, ValueError):
            break  # the process exited between poll() and the read
        time.sleep(0.0005)
    if process.wait() != 0:
        raise SystemExit(f'{binary} exited {process.returncode}')
    return peak


def main():
    fixture, rounds, output = sys.argv[1], int(sys.argv[2]), Path(sys.argv[3])
    builds = dict(arg.split('=', 1) for arg in sys.argv[4:])
    runs = {name: [] for name in builds}
    with tempfile.TemporaryDirectory() as scratch:
        for r in range(rounds):
            for name in (list(builds) if r % 2 == 0 else list(builds)[::-1]):
                runs[name].append(peaks(builds[name], fixture, str(Path(scratch) / 'out.wav')))
    for name, peak in runs.items():
        print(name, ' '.join(f'{k} {min(p[k] for p in peak) / 1024:.1f}-{max(p[k] for p in peak) / 1024:.1f}'
                             for k in KEYS), 'MiB')
    output.write_text(json.dumps({'fixture': Path(fixture).name, 'unit': 'KiB', 'runs': runs}, indent=1) + '\n')


if __name__ == '__main__':
    main()
