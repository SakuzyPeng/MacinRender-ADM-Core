#!/usr/bin/env python3
"""Reproduce the Windows binaural timing and memory investigation.

Use --wait to time process completion independently from peak-memory sampling.
The suite and executable directories are explicit inputs; no build is performed.
"""
import argparse
import array
import hashlib
import math
import platform
import struct
import sys
import threading
import ctypes
import json
import os
import re
import statistics
import subprocess
import tempfile
import time
from pathlib import Path

def sha(path):
    return hashlib.sha256(path.read_bytes()).hexdigest()

def read_pcm(path):
    raw = path.read_bytes()
    magic, version, channels, rate, frames = struct.unpack('<4sIIIQ', raw[:24])
    assert magic == b'MRPB' and version == 1
    assert len(raw) == 24 + channels * frames * 4
    values = array.array('f', raw[24:])
    if sys.byteorder != 'little':
        values.byteswap()
    assert all((math.isfinite(v) for v in values))
    return ({'channels': channels, 'sample_rate': rate, 'frames': frames, 'sha256': hashlib.sha256(raw[24:]).hexdigest()}, raw[24:])

def windows_rss(process):

    class Counters(ctypes.Structure):
        _fields_ = [('cb', ctypes.c_ulong), ('faults', ctypes.c_ulong)] + [(name, ctypes.c_size_t) for name in ('peak', 'working', 'peak_paged', 'paged', 'peak_nonpaged', 'nonpaged', 'pagefile', 'peak_pagefile')]
    info = Counters()
    info.cb = ctypes.sizeof(info)
    fn = ctypes.windll.psapi.GetProcessMemoryInfo
    fn.argtypes = [ctypes.c_void_p, ctypes.c_void_p, ctypes.c_ulong]
    return int(info.peak) if fn(int(process._handle), ctypes.byref(info), info.cb) else 0

def context():
    result = {'platform': platform.platform(), 'machine': platform.machine(), 'logical_cores': os.cpu_count(), 'utc': time.strftime('%Y-%m-%dT%H:%M:%SZ', time.gmtime()), 'background_policy': 'Normal desktop applications/services retained; no affinity or power changes.'}
    if os.name == 'nt':

        class Power(ctypes.Structure):
            _fields_ = [('ac', ctypes.c_ubyte), ('battery_flags', ctypes.c_ubyte), ('battery_percent', ctypes.c_ubyte), ('reserved', ctypes.c_ubyte), ('remaining_seconds', ctypes.c_ulong), ('full_seconds', ctypes.c_ulong)]
        power = Power()
        if ctypes.windll.kernel32.GetSystemPowerStatus(ctypes.byref(power)):
            result['power'] = {k: getattr(power, k) for k, _ in power._fields_}

        def cpu_times():
            idle, kernel, user = (ctypes.c_uint64(), ctypes.c_uint64(), ctypes.c_uint64())
            ctypes.windll.kernel32.GetSystemTimes(ctypes.byref(idle), ctypes.byref(kernel), ctypes.byref(user))
            return (idle.value, kernel.value + user.value)
        first = cpu_times()
        time.sleep(0.25)
        last = cpu_times()
        result['cpu_busy_percent_sample'] = 100 * (1 - (last[0] - first[0]) / max(1, last[1] - first[1]))
    elif platform.system() == 'Darwin':
        result['power'] = subprocess.check_output(['pmset', '-g', 'batt'], text=True).strip()
        result['load_average'] = os.getloadavg()
    return result
p = argparse.ArgumentParser(description='Windows binaural investigation collector; --wait separates process timing from memory polling.')
p.add_argument('variants', nargs='+')
p.add_argument('--suite', type=Path, required=True)
p.add_argument('--binaries-root', type=Path, required=True)
p.add_argument('--reference-root', type=Path, required=True)
p.add_argument('--output-dir', type=Path, required=True)
p.add_argument('--cwd', type=Path, default=Path.cwd())
p.add_argument('--case', choices=['cloud', 'stream-controls'], default='cloud')
p.add_argument('--repeats', type=int, default=7)
p.add_argument('--schedule', default='')
p.add_argument('--label', default='')
p.add_argument('--wait', action='store_true')
a = p.parse_args()
if a.repeats < 1:
    p.error('--repeats must be positive')
r = a.cwd.resolve()
out = a.output_dir.resolve()
out.mkdir(parents=True, exist_ok=True)
old = a.reference_root.resolve()
if os.name != 'nt':
    p.error('This collector uses Windows process counters.')
fixtures = a.suite.resolve() / 'fixtures'
case = next((c for c in json.loads((a.suite / 'cases.json').read_text())['streams'] if c['id'] == 'stream-binaural-cloud-fixed'))
report = {'context': context(), 'case': a.case, 'schedule': a.schedule, 'collector': 'native-wait-with-sampler' if a.wait else 'poll-sleep-2ms', 'runs': {v: [] for v in a.variants}}
env = os.environ.copy()
env.pop('MR_ADM_BIN_THREADS', None)
env['MR_ADM_BIN_SCHEDULE'] = a.schedule

def cpu(p):
    values = [ctypes.c_uint64() for _ in range(4)]
    fn = ctypes.windll.kernel32.GetProcessTimes
    fn.argtypes = [ctypes.c_void_p, *[ctypes.c_void_p] * 4]
    if fn(int(p._handle), *[ctypes.byref(v) for v in values]):
        return {'kernel_ms': values[2].value / 10000, 'user_ms': values[3].value / 10000, 'process_seconds': (values[1].value - values[0].value) / 10000000}
    return {}
expected_pcm = None
with tempfile.TemporaryDirectory(prefix='probe-', dir=out) as td:
    temp = Path(td)
    stream_spec = temp / 'spec.json'
    stream_spec.write_text(json.dumps(case))
    for repeat in range(a.repeats + 1):
        for variant in a.variants if repeat % 2 == 0 else list(reversed(a.variants)):
            binary = a.binaries_root.resolve() / variant
            if not binary.exists():
                binary = old / variant
            if a.case == 'cloud':
                audio = temp / 'audio.wav'
                cmd = [binary / 'mradm.exe', 'render', '-i', fixtures / 'objects-extent.wav', '-o', audio, '--renderer', 'saf-binaural', '--binaural-spread-mode', 'cloud', '--output-bit-depth', 'f32', '--no-peak-limit']
            else:
                cmd = [binary / 'mr_adm_phase2_stream.exe', stream_spec, fixtures / 'objects-extent-multi.wav', temp / 'stream']
            began = time.perf_counter()
            peak = 0
            with (temp / 'log').open('wb') as f:
                proc = subprocess.Popen([str(x) for x in cmd], cwd=r, env=env, stdout=f, stderr=subprocess.STDOUT)
                if a.wait:
                    finished = threading.Event()
                    samples = []

                    def sample():
                        while not finished.is_set():
                            samples.append(windows_rss(proc))
                            finished.wait(0.002)
                    sampler = threading.Thread(target=sample)
                    sampler.start()
                    try:
                        proc.wait()
                        ended = time.perf_counter()
                    finally:
                        finished.set()
                        sampler.join()
                    peak = max(samples, default=0)
                else:
                    while proc.poll() is None:
                        peak = max(peak, windows_rss(proc))
                        time.sleep(0.002)
                    ended = time.perf_counter()
                peak = max(peak, windows_rss(proc))
            elapsed = ended - began
            text = (temp / 'log').read_text(errors='replace')
            assert proc.returncode == 0, text[-4000:]
            lines = [line for line in text.splitlines() if line.startswith(('BINPROBE', 'BINMEM'))]
            if a.case == 'cloud':
                bits = temp / 'audio.pcmbits'
                subprocess.run([str(binary / 'mr_adm_pcm_bits.exe'), 'extract', str(audio), str(bits)], check=True, stdout=subprocess.DEVNULL)
                pcm = read_pcm(bits)[0]
                audio.unlink()
                bits.unlink()
            else:
                pcms = [read_pcm(temp / f'stream/pass-{v}.pcmbits')[0] for v in [1, 2]]
                assert pcms[0] == pcms[1]
                pcm = pcms[0]
                for f in (temp / 'stream').iterdir():
                    f.unlink()
                (temp / 'stream').rmdir()
            if expected_pcm is None:
                expected_pcm = pcm
            assert pcm == expected_pcm, f'{variant}: output differs from the reference'
            row = {'seconds': elapsed, 'rss_bytes': peak, 'cpu': cpu(proc), 'probe': lines, 'pcm': pcm}
            if repeat:
                report['runs'][variant].append(row)
    for variant, rows in report['runs'].items():
        print(variant, 'median_s', statistics.median((x['seconds'] for x in rows)), 'median_rss', statistics.median((x['rss_bytes'] for x in rows)), 'cpu', {k: statistics.median((x['cpu'][k] for x in rows)) for k in rows[0]['cpu']}, flush=True)
        stages = {}
        for row in rows:
            totals = {}
            for line in row['probe']:
                if line.startswith('BINPROBE'):
                    _, key, value, *_ = line.split()
                    totals[key] = totals.get(key, 0) + float(value)
            for key, value in totals.items():
                stages.setdefault(key, []).append(value)
        print('stage medians', json.dumps({k: statistics.median(v) for k, v in stages.items()}), flush=True)
        if a.case != 'cloud':
            print('\n'.join(rows[0]['probe']), flush=True)
label = a.label or '-'.join(a.variants) + '-' + a.case
report['instrumented'] = any((row['probe'] for rows in report['runs'].values() for row in rows))
report['pcm_all_identical'] = True
report['binaries'] = {v: {n: sha((a.binaries_root / v if (a.binaries_root / v).exists() else old / v) / n) for n in ['mradm.exe', 'mr_adm_pcm_bits.exe', 'mr_adm_phase2_stream.exe']} for v in a.variants}
report['context_after'] = context()
(out / (label + '.json')).write_text(json.dumps(report, indent=2))
print('saved', label, flush=True)
