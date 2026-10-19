#!/usr/bin/env python3
"""Collect independent Release room runs and the strict Windows deadline gate."""
import argparse
import ctypes as C
import hashlib
import json
import os
from pathlib import Path
import platform
import subprocess
import sys
import time

SCENARIOS = ['static', 'pose', 'switch', 'ramps', 'distance-ramps']


class MemoryCounters(C.Structure):
    _fields_ = [('cb', C.c_uint32), ('faults', C.c_uint32)] + [
        (name, C.c_size_t) for name in ['peak_working_set', 'working_set', 'peak_paged', 'paged',
                                       'peak_nonpaged', 'nonpaged', 'pagefile', 'peak_pagefile', 'private']]


def host_snapshot():
    """Record background load and power state without imposing an idle gate."""
    kernel = C.WinDLL('kernel32', use_last_error=True)
    kernel.GetSystemTimes.argtypes = [C.POINTER(C.c_uint64)] * 3
    kernel.GetSystemTimes.restype = C.c_int32

    def times():
        idle, system, user = C.c_uint64(), C.c_uint64(), C.c_uint64()
        if not kernel.GetSystemTimes(C.byref(idle), C.byref(system), C.byref(user)):
            raise C.WinError(C.get_last_error())
        return idle.value, system.value + user.value

    loads = []
    before = times()
    for _ in range(3):
        time.sleep(1.)
        after = times()
        total = after[1] - before[1]
        if total <= 0:
            raise RuntimeError('Invalid system CPU clock')
        loads.append(100. * (1. - (after[0] - before[0]) / total))
        before = after

    class PowerStatus(C.Structure):
        _fields_ = [(name, C.c_ubyte) for name in ['ac', 'battery_flags', 'battery_percent', 'saver']] + [
            ('remaining_seconds', C.c_uint32), ('full_seconds', C.c_uint32)]
    power = PowerStatus()
    kernel.GetSystemPowerStatus.argtypes = [C.POINTER(PowerStatus)]
    kernel.GetSystemPowerStatus.restype = C.c_int32
    if not kernel.GetSystemPowerStatus(C.byref(power)):
        raise C.WinError(C.get_last_error())
    return dict(utc=time.strftime('%Y-%m-%dT%H:%M:%SZ', time.gmtime()),
                background_cpu_percent=loads, ac=power.ac,
                battery_percent=power.battery_percent, saver=power.saver)


# Compatibility for existing research collectors; the snapshot is informational.
idle_snapshot = host_snapshot


def acceptance(cases):
    candidates = [case for case in cases if case['variant'] == 'candidate']
    identities = {(case['scenario'], case['repetition']) for case in candidates}
    complete = len(candidates) == 6 and identities == {(s, r) for s in SCENARIOS[-2:] for r in range(1, 4)}
    calls = sum(case['result']['blocks'] for case in candidates)
    misses = sum(case['result']['deadline_misses'] for case in candidates)
    return dict(complete=complete, measured_calls=calls, deadline_misses=misses,
                passed=complete and calls == 135000 and misses == 0)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--binary', required=True, type=Path)
    parser.add_argument('--baseline-binary', type=Path)
    parser.add_argument('--baseline-calls', type=int, help='optional shorter baseline runs; recorded separately')
    parser.add_argument('--room', required=True, type=Path)
    parser.add_argument('--hrir', required=True, type=Path)
    parser.add_argument('--output', required=True, type=Path)
    parser.add_argument('--calls', type=int)
    parser.add_argument('--frames', type=int, choices=[256, 512], default=512)
    parser.add_argument('--quality', choices=['exact', 'realtime'], default='realtime')
    parser.add_argument('--sources', type=int, nargs='+', choices=[1, 16, 32])
    parser.add_argument('--scenarios', nargs='+', choices=SCENARIOS)
    parser.add_argument('--workers', type=int, default=8)
    parser.add_argument('--repetitions', type=int)
    parser.add_argument('--acceptance', action='store_true', help='96 kHz / 512 / 32 / 8, two scenarios, 3 x 22500 calls')
    parser.add_argument('--host-note', '--idle-note', dest='host_note',
                        help='describe the host workload; background CPU is recorded, not gated')
    args = parser.parse_args()
    if sys.platform != 'win32':
        parser.error('This collector uses Windows process working-set counters')
    args.calls = args.calls if args.calls is not None else (22500 if args.acceptance else 512)
    args.sources = args.sources or ([32] if args.acceptance else [1, 16, 32])
    args.scenarios = args.scenarios or (SCENARIOS[-2:] if args.acceptance else SCENARIOS)
    args.repetitions = args.repetitions if args.repetitions is not None else (3 if args.acceptance else 1)
    if not 1 <= args.calls <= 112500 or args.workers < 1 or args.repetitions < 1:
        parser.error('invalid calls, workers or repetitions')
    if args.baseline_calls is not None and (not args.baseline_binary or not 1 <= args.baseline_calls <= 112500):
        parser.error('baseline-calls requires a baseline binary and 1..112500 calls')
    if args.acceptance and (args.calls != 22500 or args.sources != [32] or args.frames != 512
                           or args.workers != 8 or args.quality != 'realtime' or args.repetitions != 3
                           or args.scenarios != SCENARIOS[-2:] or not args.host_note):
        parser.error('acceptance requires the fixed matrix and an explicit host-note')
    args.output.mkdir(parents=True, exist_ok=False)
    psapi = C.WinDLL('psapi')
    psapi.GetProcessMemoryInfo.argtypes = [C.c_void_p, C.POINTER(MemoryCounters), C.c_uint32]
    psapi.GetProcessMemoryInfo.restype = C.c_int32
    binaries = [('candidate', args.binary)]
    if args.baseline_binary:
        binaries.insert(0, ('baseline', args.baseline_binary))
    report = dict(schema='mradm.room-windows-benchmark', version=3, platform=platform.platform(),
                  processor=platform.processor(), logical_processors=os.cpu_count(), workers=args.workers,
                  host_note=args.host_note, acceptance_requested=args.acceptance,
                  host_load_policy='Informational only; ordinary desktop background load does not invalidate deadline results.',
                  binaries={name: hashlib.sha256(path.read_bytes()).hexdigest() for name, path in binaries},
                  scope='Fresh process per run, no profile feature. Working-set peak includes preparation. Calls exclude 32 warmup calls. Software timing only.',
                  cases=[])
    env = os.environ.copy()
    env['MR_ADM_ROOM_WORKERS'] = str(args.workers)
    for repetition in range(1, args.repetitions + 1):
        for sources in args.sources:
            for scenario in args.scenarios:
                for variant, binary in binaries:
                    calls = args.baseline_calls if variant == 'baseline' and args.baseline_calls else args.calls
                    name = f'{variant}-{sources}-{scenario}-{repetition}'
                    host = host_snapshot() if args.acceptance else None
                    command = [str(binary.resolve()), str(args.room.resolve()), str(calls), str(args.frames),
                               str(sources), '--quality', args.quality, '--hrir', str(args.hrir.resolve()), '--scenario', scenario]
                    (args.output / f'{name}.start.json').write_text(json.dumps(
                        dict(variant=variant, scenario=scenario, repetition=repetition,
                             command=command, host_before=host, expected_calls=calls), indent=2) + '\n', encoding='utf-8')
                    stdout, stderr = args.output / f'{name}.jsonl', args.output / f'{name}.stderr'
                    peak, observations = 0, 0
                    start = time.monotonic()
                    last_progress = start
                    with stdout.open('wb') as out, stderr.open('wb') as error:
                        process = subprocess.Popen(command, env=env, stdout=out, stderr=error)
                        while process.poll() is None:
                            counters = MemoryCounters(); counters.cb = C.sizeof(counters)
                            if psapi.GetProcessMemoryInfo(int(process._handle), C.byref(counters), counters.cb):
                                peak = max(peak, counters.peak_working_set)
                                observations += 1
                            now = time.monotonic()
                            if now - last_progress >= 30:
                                print(json.dumps(dict(progress=name, elapsed_seconds=round(now-start, 1),
                                                      peak_working_set_bytes=peak)), flush=True)
                                last_progress = now
                            time.sleep(.05)
                    if process.returncode:
                        raise RuntimeError(stderr.read_text(encoding='utf-8', errors='replace'))
                    rows = [json.loads(line) for line in stdout.read_text(encoding='utf-8').splitlines()]
                    if len(rows) != 1 or not observations or not peak:
                        raise RuntimeError('Incomplete benchmark or memory evidence')
                    row = rows[0]
                    if (row['sources'], row['scenario'], row['blocks'], row['call_frames'], row['quality']) != (
                            sources, scenario, calls, args.frames, args.quality) or row['profiling'] or row.get('experiment'):
                        raise RuntimeError('Unexpected configuration or instrumented/experimental binary')
                    if args.acceptance and row['sample_rate'] != 96000:
                        raise RuntimeError('Acceptance requires native 96 kHz')
                    report['cases'].append(dict(variant=variant, sources=sources, scenario=scenario, repetition=repetition,
                                                command=command, host_before=host, process_seconds=time.monotonic()-start,
                                                peak_working_set_bytes=peak, observations=observations, result=row))
                    if args.acceptance:
                        report['acceptance'] = acceptance(report['cases'])
                    (args.output / 'report.json').write_text(json.dumps(report, indent=2) + '\n', encoding='utf-8')
                    print(json.dumps(dict(run=name, mean_ms=row['mean_ms'], p99_ms=row['p99_ms'],
                                          maximum_ms=row['maximum_ms'], misses=row['deadline_misses'], calls=calls,
                                          peak_working_set_bytes=peak, host_before=host)), flush=True)
    if args.acceptance and not report['acceptance']['passed']:
        raise SystemExit(1)


if __name__ == '__main__':
    main()
