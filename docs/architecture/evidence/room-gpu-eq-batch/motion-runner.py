"""Run the frozen EQ candidate's missing motion gate, without switching rooms."""
import ctypes as C
import hashlib
import importlib.util
import json
import os
from pathlib import Path
import subprocess
import sys
import time

ROOT = Path(__file__).resolve().parent
ENV = os.environ.copy()
ENV.update(json.loads((ROOT / 'env.json').read_text()))
ENV['MR_ADM_ROOM_WORKERS'] = '8'
MODE = sys.argv[1]
assert MODE in ('smoke', 'short', 'long')
PROOF = json.loads((ROOT / 'provenance.json').read_text())
for name, expected in PROOF['payload_sha256'].items():
    assert hashlib.sha256((ROOT / name).read_bytes()).hexdigest() == expected, name
assert PROOF['previous_numerical_passed'] and PROOF['fixture_identical']
spec = importlib.util.spec_from_file_location('host_monitor', ROOT / 'host-monitor.py')
monitor = importlib.util.module_from_spec(spec)
spec.loader.exec_module(monitor)
api = C.WinDLL('psapi')
api.GetProcessMemoryInfo.argtypes = [C.c_void_p, C.POINTER(monitor.MemoryCounters), C.c_uint32]
api.GetProcessMemoryInfo.restype = C.c_int32
GPU_QUERY = ['nvidia-smi', '--query-gpu=name,driver_version,pstate,utilization.gpu,clocks.current.sm,power.draw,memory.used', '--format=csv']
REPORT_PATH = ROOT / 'runs' / (MODE + '-summary.json')
assert not REPORT_PATH.exists(), 'Do not overwrite a previous result'
report = dict(schema='mradm.room-gpu-eq-motion', version=1, mode=MODE, workers=8,
              scope='Fresh process per run; uninstrumented Release Session; 32 warmup calls excluded; no device callback.',
              host_policy='Normal desktop applications retained; no concurrent task build or benchmark. All candidate motion misses count.',
              binary_sha256=PROOF['payload_sha256'], cases=[], passed=False)


def save():
    REPORT_PATH.write_text(json.dumps(report, indent=2) + '\n', encoding='utf-8')


def run(variant, scenario, repetition, calls):
    name = f'{MODE}-{variant}-{scenario}-{repetition}'
    folder = ROOT / variant
    command = [str(folder / 'room_bench.exe'), str(ROOT / 'assets/room/room.json'), str(calls), '512', '32',
               '--hrir', str(ROOT / 'assets/collection'), '--quality', 'realtime', '--scenario', scenario,
               '--gpu-dll', str(folder / 'room_gpu_session.dll'), '--gpu-fixture', str(ROOT / 'fixture'),
               '--gpu-kernels', str(folder / 'session_kernels.cu')]
    before = monitor.host_snapshot()
    gpu_before = subprocess.check_output(GPU_QUERY, text=True)
    record = dict(variant=variant, scenario=scenario, repetition=repetition, calls=calls,
                  command=command, host_before=before, gpu_before=gpu_before)
    (ROOT / 'runs' / (name + '.start.json')).write_text(json.dumps(record, indent=2), encoding='utf-8')
    print('start', name, calls, flush=True)
    peak = observations = 0
    start = last_progress = time.monotonic()
    with (ROOT / 'runs' / (name + '.jsonl')).open('w') as out, (ROOT / 'runs' / (name + '.stderr')).open('w') as err:
        process = subprocess.Popen(command, env=ENV, stdout=out, stderr=err)
        while process.poll() is None:
            memory = monitor.MemoryCounters()
            memory.cb = C.sizeof(memory)
            if api.GetProcessMemoryInfo(int(process._handle), C.byref(memory), memory.cb):
                peak = max(peak, memory.peak_working_set)
                observations += 1
            now = time.monotonic()
            if now - last_progress >= 30:
                print(json.dumps(dict(progress=name, elapsed_seconds=round(now-start, 1))), flush=True)
                last_progress = now
            time.sleep(.05)
    if process.returncode:
        raise RuntimeError((ROOT / 'runs' / (name + '.stderr')).read_text(errors='replace'))
    row = json.loads((ROOT / 'runs' / (name + '.jsonl')).read_text())
    assert (row['sources'], row['scenario'], row['blocks'], row['call_frames'], row['quality'], row['sample_rate']) == (
        32, scenario, calls, 512, 'realtime', 96000)
    assert not row['profiling'] and row['experiment'] == dict(
        kind='room-gpu-session', cpu_reference=False, control='production Session', finite_fade=True)
    assert observations and peak
    record.update(result=row, peak_working_set_bytes=peak, memory_observations=observations,
                  process_seconds=time.monotonic()-start, host_after=monitor.host_snapshot(),
                  gpu_after=subprocess.check_output(GPU_QUERY, text=True))
    report['cases'].append(record)
    save()
    print(json.dumps(dict(run=name, **{k: row[k] for k in ['mean_ms', 'p99_ms', 'maximum_ms', 'deadline_misses', 'blocks']},
                          peak_working_set_bytes=peak)), flush=True)
    return row['deadline_misses']


def candidate_failed(misses):
    if misses:
        report['stop_reason'] = 'Candidate motion deadline miss; no later screen or long run.'
        save()
        print(report['stop_reason'], flush=True)
        raise SystemExit(2)


if MODE == 'smoke':
    for variant in ('baseline', 'candidate'):
        run(variant, 'ramps', 1, 1)
    report['passed'] = True
elif MODE == 'short':
    assert json.loads((ROOT / 'runs/smoke-summary.json').read_text())['passed']
    for index, scenario in enumerate(('ramps', 'distance-ramps')):
        order = ('baseline', 'candidate') if index == 0 else ('candidate', 'baseline')
        for variant in order:
            misses = run(variant, scenario, 1, 1024)
            if variant == 'candidate':
                candidate_failed(misses)
    report['passed'] = True
elif MODE == 'long':
    short = json.loads((ROOT / 'runs/short-summary.json').read_text())
    assert short['passed'] and short['binary_sha256'] == report['binary_sha256']
    for repetition in range(1, 4):
        for index, scenario in enumerate(('ramps', 'distance-ramps')):
            order = ('baseline', 'candidate') if (repetition + index) % 2 else ('candidate', 'baseline')
            for variant in order:
                misses = run(variant, scenario, repetition, 22500 if variant == 'candidate' else 1024)
                if variant == 'candidate':
                    candidate_failed(misses)
    candidates = [c for c in report['cases'] if c['variant'] == 'candidate']
    assert len(candidates) == 6 and sum(c['result']['blocks'] for c in candidates) == 135000
    report['passed'] = True
save()
print(MODE, 'passed', flush=True)
