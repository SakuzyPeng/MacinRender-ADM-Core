#!/usr/bin/env python3
"""Drive the native Release room device probe with 100 Hz PoseBridge v3 OSC.

All timestamps describe host/software endpoints. No sensor-to-headphone latency
is inferred. Optional loopback captures cover only the selected audio route.
"""
from __future__ import annotations

import argparse
import ctypes
import os
import hashlib
import json
import math
from pathlib import Path
import socket
import struct
import subprocess
import threading
import time


def osc_string(value):
    data = value.encode('utf-8') + b'\0'
    return data + b'\0' * (-len(data) % 4)


def pose_packet(sequence, elapsed_ns):
    # Simulated sample time is the 100 Hz source clock. Host receive/send time
    # is a separate monotonic clock and may jitter without duplicating samples.
    t = (sequence - 1) / 100.
    values = [math.sin(t * .25) * 90, math.sin(t * .7) * 20, math.sin(t * .4) * 10]
    return (osc_string('/posebridge/euler') + osc_string(',ishhhhhhhhihhfff') + struct.pack('>i', 3)
            + osc_string('room-probe') + struct.pack('>8Q', 1, 1, sequence, sequence, 1, 1, elapsed_ns, 0)
            + struct.pack('>IQQ3f', 2, (sequence - 1) * 10, 1, *values))


class SenderTimer:
    """Per-thread Windows high-resolution timer; no global timer changes."""
    def __init__(self):
        self.handle = None
        if os.name == 'nt':
            from ctypes import wintypes
            self.kernel = ctypes.WinDLL('kernel32', use_last_error=True)
            self.kernel.CreateWaitableTimerExW.argtypes = [ctypes.c_void_p, wintypes.LPCWSTR, wintypes.DWORD, wintypes.DWORD]
            self.kernel.CreateWaitableTimerExW.restype = wintypes.HANDLE
            self.kernel.SetWaitableTimer.argtypes = [wintypes.HANDLE, ctypes.POINTER(ctypes.c_longlong), wintypes.LONG,
                                                     ctypes.c_void_p, ctypes.c_void_p, wintypes.BOOL]
            self.kernel.SetWaitableTimer.restype = wintypes.BOOL
            self.kernel.WaitForSingleObject.argtypes = [wintypes.HANDLE, wintypes.DWORD]
            self.kernel.WaitForSingleObject.restype = wintypes.DWORD
            self.kernel.CloseHandle.argtypes = [wintypes.HANDLE]
            self.kernel.CloseHandle.restype = wintypes.BOOL
            self.handle = self.kernel.CreateWaitableTimerExW(None, None, 2, 0x00100002)
            if not self.handle:
                raise ctypes.WinError(ctypes.get_last_error())

    def wait_until(self, deadline, stop):
        remaining = deadline - time.perf_counter_ns()
        if remaining <= 0 or stop.is_set():
            return
        if self.handle:
            due = ctypes.c_longlong(-max(1, (remaining + 99) // 100))
            if not self.kernel.SetWaitableTimer(self.handle, ctypes.byref(due), 0, None, None, False):
                raise ctypes.WinError(ctypes.get_last_error())
            if self.kernel.WaitForSingleObject(self.handle, 1000) != 0:
                raise RuntimeError('High resolution OSC sender wait failed')
        else:
            stop.wait(remaining / 1e9)

    def close(self):
        if self.handle:
            self.kernel.CloseHandle(self.handle)
            self.handle = None


def summarize_memory(rows):
    # Exclude setup and inspect the latter half. RSS is an OS observation, not a
    # live-allocation count; retain the full samples so plateaus remain visible.
    if len(rows) < 20:
        return {'evaluated': False, 'reason': 'at least 20 one-second samples required'}
    latter_half = memory_window(rows[len(rows) // 2:])
    latter_half['final_windows'] = {f'{minutes}_minutes': memory_window(
        [r for r in rows if r['seconds'] >= rows[-1]['seconds'] - minutes * 60])
        for minutes in (5, 10)}
    return latter_half


def memory_window(rows):
    times = [r['seconds'] for r in rows]
    values = [r['rss_bytes'] for r in rows]
    mt, mv = sum(times) / len(times), sum(values) / len(values)
    denominator = sum((t - mt) ** 2 for t in times)
    slope = sum((t - mt) * (v - mv) for t, v in zip(times, values)) / denominator if denominator else 0.
    return {'evaluated': True, 'observation_seconds': times[-1] - times[0],
            'rss_slope_bytes_per_minute': slope * 60,
            'rss_first_bytes': values[0], 'rss_last_bytes': values[-1],
            'rss_min_bytes': min(values), 'rss_max_bytes': max(values),
            'scope': 'resident memory in the latter half; inspect the time series alongside zero-allocation tests'}


def control_acceptance(report):
    osc = report.get('osc') or {}
    received = int(osc.get('pose_packets', 0))
    applied = report.get('poses_applied', 0)
    minimum = math.ceil(report['requested_seconds'] * 100 * .99)
    control = {
        'nominal_source_hz': 100,
        'minimum_received_packets': minimum,
        'received_packets': received,
        'applied_poses': applied,
        'application_fraction': applied / received if received else 0,
        'rejected_packets': int(osc.get('rejected_packets', 0)),
        'missing_tx_packets': int(osc.get('missing_tx_packets', 0)),
        'scope': '100 Hz simulated samples, including the tail; require at least 99% of the material-duration packet budget and application count.',
    }
    control['passed'] = (received >= minimum and control['rejected_packets'] == 0
                         and control['missing_tx_packets'] == 0 and control['application_fraction'] >= .99)
    return control


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--probe', type=Path, required=True)
    parser.add_argument('--room', type=Path, required=True)
    parser.add_argument('--other-room', type=Path, required=True)
    parser.add_argument('--hrir', type=Path, help='normal SOFA/HRIR/collection override')
    parser.add_argument('--device', required=True, help='exact output name or current opaque ID; "null" for a dry run')
    parser.add_argument('--seconds', type=int, default=1800)
    parser.add_argument('--sources', type=int, choices=[1, 16, 32], default=32)
    parser.add_argument('--quality', choices=['exact', 'realtime'], default='exact')
    parser.add_argument('--source-ramps', action='store_true', help='move each source at a distinct 512-frame event offset')
    parser.add_argument('--single-room-switch', action='store_true', help='switch once near the start, then keep the room fixed')
    parser.add_argument('--output', type=Path, required=True, help='new evidence directory')
    parser.add_argument('--volume', type=float, default=.1, help='session-only linear output gain')
    group = parser.add_mutually_exclusive_group()
    group.add_argument('--capture', help='exact virtual input name, for example BlackHole 16ch')
    group.add_argument('--capture-loopback', action='store_true', help='native WASAPI playback-endpoint loopback')
    args = parser.parse_args()
    if not 5 <= args.seconds <= 3600 or not 0 < args.volume <= 1:
        parser.error('seconds must be 5..3600 and volume must be (0,1]')
    args.output.mkdir(parents=True, exist_ok=False)
    mode = 'stream' if args.single_room_switch else ('stream-cycles' if args.device == 'null' else 'stream-device')
    command = [str(args.probe.resolve()), str(args.room.resolve()), str(args.other_room.resolve()),
               str(args.seconds), mode, str(args.sources),
               '--osc-port', '0', '--volume', str(args.volume), '--quality', args.quality]
    if args.hrir:
        command += ['--hrir', str(args.hrir.resolve())]
    if args.source_ramps:
        command += ['--source-ramps']
    if args.device != 'null':
        command += ['--device', args.device]
    if args.capture or args.capture_loopback:
        command += ['--recordings', str((args.output / 'recordings').resolve())]
        command += ['--capture', args.capture] if args.capture else ['--capture-loopback']
    metadata = {'schema': 'mradm.room-device-probe', 'version': 1, 'command': command,
                'probe_sha256': hashlib.sha256(args.probe.read_bytes()).hexdigest(),
                'room_manifest_sha256': hashlib.sha256(args.room.read_bytes()).hexdigest(),
                'other_manifest_sha256': hashlib.sha256(args.other_room.read_bytes()).hexdigest(),
                'pose_protocol': 3, 'pose_target_hz': 100, 'quality': args.quality,
                'source_ramps': args.source_ramps, 'single_room_switch': args.single_room_switch,
                'latency_scope': 'host software endpoints only'}
    (args.output / 'environment.json').write_text(json.dumps(metadata, indent=2) + '\n', encoding='utf-8')
    stop = threading.Event()
    sender = None
    sent = {'packets': 0, 'scheduling_max_lateness_ms': 0., 'error': None}

    def send(port):
        timer = None
        try:
            timer = SenderTimer()
            with socket.socket(socket.AF_INET, socket.SOCK_DGRAM) as sock:
                origin = time.perf_counter_ns()
                tick = 0
                while not stop.is_set():
                    now = time.perf_counter_ns()
                    sent['scheduling_max_lateness_ms'] = max(sent['scheduling_max_lateness_ms'],
                                                            (now - origin - tick * 10_000_000) / 1e6)
                    sock.sendto(pose_packet(tick + 1, now - origin), ('127.0.0.1', port))
                    sent['packets'] += 1
                    tick += 1
                    timer.wait_until(origin + tick * 10_000_000, stop)
        except Exception as exc:
            sent['error'] = str(exc)
        finally:
            if timer:
                timer.close()

    report = None
    process = None
    try:
        with (args.output / 'native.jsonl').open('w', encoding='utf-8') as stdout, (args.output / 'native.stderr').open('w', encoding='utf-8') as stderr:
            process = subprocess.Popen(command, stdout=subprocess.PIPE, stderr=stderr, text=True, encoding='utf-8', bufsize=1)
            for line in process.stdout:
                stdout.write(line)
                stdout.flush()
                row = json.loads(line)
                if row.get('case') == 'probe_ready':
                    sender = threading.Thread(target=send, args=(row['osc_port'],), daemon=True)
                    sender.start()
                    print(json.dumps(row), flush=True)
                elif row.get('case') == 'device_progress':
                    if int(row['seconds']) % 30 == 0:
                        print(json.dumps(row), flush=True)
                else:
                    report = row
            process.wait()
    finally:
        stop.set()
        if sender:
            sender.join(timeout=2)
        if process is not None and process.poll() is None:
            process.terminate()
            try:
                process.wait(timeout=5)
            except subprocess.TimeoutExpired:
                process.kill()
                process.wait()
    if report is None:
        raise SystemExit(f'Native probe failed; inspect {args.output / "native.stderr"}')
    report['driver'] = sent
    report['native_returncode'] = process.returncode
    report['memory_analysis'] = summarize_memory(report.get('memory_samples', []))
    control = report['control_acceptance'] = control_acceptance(report)
    report['passed'] = process.returncode == 0 and sent['error'] is None and control['passed']
    (args.output / 'report.json').write_text(json.dumps(report, indent=2, allow_nan=False) + '\n', encoding='utf-8')
    print(json.dumps({k: report.get(k) for k in ['passed', 'requested_seconds', 'presented_frames',
                                               'underruns', 'applied_revision', 'control_acceptance', 'memory_analysis']}), flush=True)
    if not report['passed']:
        raise SystemExit(1)


if __name__ == '__main__':
    main()
