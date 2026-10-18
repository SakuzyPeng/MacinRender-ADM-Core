#!/usr/bin/env python3
"""Run Release room probes with a separate process and working-set peak per source count."""
import argparse
import ctypes as C
import hashlib
import json
from pathlib import Path
import platform
import subprocess
import sys
import time


class MemoryCounters(C.Structure):
    _fields_ = [('cb', C.c_uint32), ('faults', C.c_uint32)] + [
        (name, C.c_size_t) for name in ['peak_working_set', 'working_set', 'peak_paged', 'paged',
                                       'peak_nonpaged', 'nonpaged', 'pagefile', 'peak_pagefile', 'private']]


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--binary', required=True, type=Path)
    parser.add_argument('--room', required=True, type=Path)
    parser.add_argument('--hrir', required=True, type=Path)
    parser.add_argument('--output', required=True, type=Path)
    parser.add_argument('--calls', type=int, default=512)
    parser.add_argument('--frames', type=int, choices=[256, 512], default=512)
    parser.add_argument('--quality', choices=['exact', 'realtime'], default='realtime')
    parser.add_argument('--sources', type=int, nargs='+', choices=[1, 16, 32], default=[1, 16, 32])
    args = parser.parse_args()
    if sys.platform != 'win32':
        parser.error('This collector uses Windows process working-set counters')
    if not 1 <= args.calls <= 112500:
        parser.error('calls must be in 1..112500')
    args.output.mkdir(parents=True, exist_ok=False)
    psapi = C.WinDLL('psapi')
    psapi.GetProcessMemoryInfo.argtypes = [C.c_void_p, C.POINTER(MemoryCounters), C.c_uint32]
    psapi.GetProcessMemoryInfo.restype = C.c_int32
    report = dict(schema='mradm.room-windows-benchmark', version=1, platform=platform.platform(),
                  binary_sha256=hashlib.sha256(args.binary.read_bytes()).hexdigest(),
                  scope='Fresh process per source count. Peak working set includes preparation and all scenarios; software timing, no device measurement.',
                  cases=[])
    for sources in args.sources:
        command = [str(args.binary.resolve()), str(args.room.resolve()), str(args.calls), str(args.frames),
                   str(sources), '--quality', args.quality, '--hrir', str(args.hrir.resolve())]
        stdout, stderr = args.output / f'{sources}.jsonl', args.output / f'{sources}.stderr'
        peak, observations = 0, 0
        with stdout.open('wb') as out, stderr.open('wb') as error:
            process = subprocess.Popen(command, stdout=out, stderr=error)
            while process.poll() is None:
                counters = MemoryCounters(); counters.cb = C.sizeof(counters)
                if psapi.GetProcessMemoryInfo(int(process._handle), C.byref(counters), counters.cb):
                    peak = max(peak, counters.peak_working_set)
                    observations += 1
                time.sleep(.05)
        if process.returncode:
            raise RuntimeError(stderr.read_text(encoding='utf-8', errors='replace'))
        rows = [json.loads(line) for line in stdout.read_text(encoding='utf-8').splitlines()]
        if len(rows) != 5 or not observations or not peak or any(row['sources'] != sources for row in rows):
            raise RuntimeError('Incomplete benchmark or memory evidence')
        report['cases'].append(dict(sources=sources, peak_working_set_bytes=peak, observations=observations, results=rows))
        (args.output / 'report.json').write_text(json.dumps(report, indent=2) + '\n', encoding='utf-8')
        print(json.dumps(dict(sources=sources, peak_working_set_bytes=peak,
                              worst_p99_ms=max(row['p99_ms'] for row in rows))), flush=True)


if __name__ == '__main__':
    main()
