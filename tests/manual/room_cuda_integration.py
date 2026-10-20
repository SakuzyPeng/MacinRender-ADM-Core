#!/usr/bin/env python3
"""Opt-in Windows ROOM CUDA public-API lifecycle/numerical checks with explicit resources."""
import argparse
import array
import concurrent.futures
import ctypes as C
import importlib.util
import json
import math
import os
from pathlib import Path
import subprocess
import sys
import threading

ROOT = Path(__file__).resolve().parents[2]
spec = importlib.util.spec_from_file_location('room96', ROOT / 'tests/unit/room_96k_test.py')
room96 = importlib.util.module_from_spec(spec)
spec.loader.exec_module(room96)


class Stats(C.Structure):
    _fields_ = [(name, C.c_uint64) for name in ['banks', 'engines', 'late_engines', 'device_bytes', 'pinned_bytes']]


class Diagnostic(room96.Sized):
    _fields_ = [('struct_size', room96.U32), ('level', room96.I32), ('code', room96.I32),
                ('reserved', room96.U32), ('epoch', room96.U64), ('generation', room96.U64),
                ('element', room96.U64), ('fields', room96.U64), ('message', C.c_char_p)]


def child(args):
    api = room96.Api(args.library)
    handle = room96.PTR()
    api.ok(api.lib.adm_room_load(api.context, str(args.room).encode(), C.byref(handle)))
    room = room96.Room(handle=handle, hrir=str(args.hrir).encode(), metres=1.5)
    report = room96.PTR()
    quality = 0 if args.case == 'exact' else 1
    code = api.lib.adm_room_inspect_with_quality(api.context, C.byref(room), quality, C.byref(report))
    if args.case in ('bad-request', 'missing'):
        assert code != 0 and not report.value
        error = api.lib.adm_context_last_error_message(api.context).decode()
        api.lib.adm_room_destroy(handle)
        api.lib.adm_destroy_context(api.context)
        return {'case': args.case, 'error_code': code, 'message': error, 'passed': True}
    api.ok(code)
    info = json.loads(C.string_at(report).decode())
    api.lib.adm_free_string(report)
    api.lib.adm_room_destroy(handle)
    expected_cuda = args.case in ('cuda', 'concurrent', 'device-reset')
    assert (info.get('execution_backend') == 'cuda') == expected_cuda
    samples = [(i % 179) * .00001 - .00089 for i in range(3333)]
    if not expected_cuda:
        result = api.scene(args.room, args.hrir, samples, [512, 137, 375], quality=quality, epoch_twice=True)
        C.windll.kernel32.GetModuleHandleW.restype = C.c_void_p
        assert not C.windll.kernel32.GetModuleHandleW('mradm_room_cuda.dll')
        (args.output / (args.case + '.f32')).write_bytes(result)
        api.lib.adm_destroy_context(api.context)
        return {'case': args.case, 'frames': len(result) // 8, 'gpu_loaded': False, 'passed': True}

    plugin = C.CDLL(str(args.plugin))
    plugin.mradm_room_cuda_stats.argtypes = [C.POINTER(Stats)]
    plugin.mradm_room_cuda_stats.restype = C.c_int

    def snapshot():
        value = Stats()
        assert plugin.mradm_room_cuda_stats(C.byref(value)) == 0
        return {name: getattr(value, name) for name, _ in Stats._fields_}

    def cleared():
        value = snapshot()
        assert not any(value.values()), value
        return value

    observed = []
    if args.case == 'device-reset':
        # cudaDeviceReset affects this fresh subprocess's primary context, not
        # other applications or the physical device/driver.
        runtime = C.CDLL(str(next(args.plugin.parent.glob('cudart64*.dll'))))
        runtime.cudaDeviceReset.restype = C.c_int
        original = api.lib.adm_create_scene_stream_with_room_quality
        original_pull = api.lib.adm_scene_stream_pull
        api.lib.adm_scene_stream_last_error_message.argtypes = [room96.PTR]
        api.lib.adm_scene_stream_last_error_message.restype = C.c_char_p
        api.lib.adm_scene_stream_log_count.argtypes = [room96.PTR]
        api.lib.adm_scene_stream_log_count.restype = room96.U32
        api.lib.adm_scene_stream_log_entry.argtypes = [room96.PTR, room96.U32, C.POINTER(Diagnostic)]
        api.lib.adm_scene_stream_log_entry.restype = C.c_int
        failures = []
        def checked_pull(stream, pcm, count, result):
            code = original_pull(stream, pcm, count, result)
            if C.cast(result, C.POINTER(room96.Pull)).contents.flags & 8:
                for index in range(api.lib.adm_scene_stream_log_count(stream)):
                    diagnostic = Diagnostic()
                    assert api.lib.adm_scene_stream_log_entry(stream, index, C.byref(diagnostic))
                    if diagnostic.level == 3:
                        failures.append(diagnostic.message.decode())
            return code
        def reset_after_create(*values):
            code = original(*values)
            if code == 0:
                observed.append(snapshot())
                assert runtime.cudaDeviceReset() == 0
            return code
        api.lib.adm_create_scene_stream_with_room_quality = reset_after_create
        api.lib.adm_scene_stream_pull = checked_pull
        try:
            api.scene(args.room, args.hrir, samples, [512], quality=1)
            raise RuntimeError('GPU context reset was not reported')
        except AssertionError as error:
            message = failures[-1] if failures else str(error)
            assert 'ROOM CUDA' in message, message
        finally:
            api.lib.adm_create_scene_stream_with_room_quality = original
            api.lib.adm_scene_stream_pull = original_pull
        cleared()
        recovered = api.scene(args.room, args.hrir, samples, [512], quality=1)
        assert recovered == (args.output / 'cuda.f32').read_bytes()
        api.lib.adm_destroy_context(api.context)
        return {'case': args.case, 'error': message, 'recreated': True,
                'active_resources': observed, 'after_release': cleared(), 'passed': True}

    if args.case == 'cuda':
        original = api.ok
        def checked(code):
            original(code)
            value = snapshot()
            if value['engines'] and not observed:
                observed.append(value)
        api.ok = checked
        results = []
        for packets in [[512], [137, 375, 1, 511], [512]]:
            results.append(api.scene(args.room, args.hrir, samples, packets, quality=1, epoch_twice=True))
            cleared()
        assert observed and all(v == results[0] for v in results)
        (args.output / 'cuda.f32').write_bytes(results[0])
        api.lib.adm_destroy_context(api.context)
        return {'case': args.case, 'cycles': 3, 'epochs_per_cycle': 2, 'frames': len(results[0]) // 8,
                'active_resources': observed, 'after_release': cleared(), 'passed': True}

    barrier = threading.Barrier(2)
    def concurrent_scene(_):
        client = room96.Api(args.library)
        original = client.lib.adm_create_scene_stream_with_room_quality
        def created(*values):
            code = original(*values)
            if code == 0:
                barrier.wait(timeout=90)
                observed.append(snapshot())
            return code
        client.lib.adm_create_scene_stream_with_room_quality = created
        try:
            return client.scene(args.room, args.hrir, samples, [512, 137, 375], quality=1)
        finally:
            client.lib.adm_destroy_context(client.context)
    with concurrent.futures.ThreadPoolExecutor(max_workers=2) as executor:
        results = list(executor.map(concurrent_scene, range(2)))
    assert len(results) == 2 and results[0] == results[1] and observed
    assert max(v['banks'] for v in observed) == 1, observed
    api.lib.adm_destroy_context(api.context)
    return {'case': args.case, 'active_resources': observed, 'after_release': cleared(), 'passed': True}


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    for name in ['library', 'plugin', 'room', 'hrir', 'output']:
        parser.add_argument('--' + name, type=Path, required=True)
    parser.add_argument('--case', choices=['cpu', 'cuda', 'exact', 'bad-request', 'missing', 'concurrent', 'device-reset'])
    args = parser.parse_args()
    assert os.name == 'nt'
    if args.case:
        result = child(args)
        print(json.dumps(result), flush=True)
        return
    args.output.mkdir(parents=True, exist_ok=False)
    report = {'cases': []}
    for name, value in [('cpu', '0'), ('exact', 'invalid'), ('cuda', '1'), ('concurrent', '1'),
                        ('bad-request', 'invalid'), ('device-reset', '1')]:
        command = [sys.executable, str(Path(__file__).resolve())]
        for key in ['library', 'plugin', 'room', 'hrir', 'output']:
            command += ['--' + key, str(getattr(args, key).resolve())]
        command += ['--case', name]
        env = dict(os.environ, MR_ADM_ROOM_CUDA=value, MR_ADM_ROOM_WORKERS='8')
        print('lifecycle', name, flush=True)
        result = subprocess.run(command, env=env, capture_output=True, text=True, timeout=240)
        (args.output / (name + '.stdout')).write_text(result.stdout)
        (args.output / (name + '.stderr')).write_text(result.stderr)
        if result.returncode:
            raise RuntimeError(result.stdout + result.stderr)
        report['cases'].append(json.loads(result.stdout.strip().splitlines()[-1]))
        (args.output / 'report.json').write_text(json.dumps(report, indent=2))
    reference = array.array('f', (args.output / 'cpu.f32').read_bytes())
    actual = array.array('f', (args.output / 'cuda.f32').read_bytes())
    assert len(actual) == len(reference)
    assert all(math.isfinite(v) for v in actual)
    energy = sum(float(v) ** 2 for v in reference)
    error = sum((float(a) - float(b)) ** 2 for a, b in zip(reference, actual))
    peak = max(abs(v) for v in reference)
    maximum = max(abs(float(a) - float(b)) for a, b in zip(reference, actual))
    report['numerical'] = {'frames': len(actual) // 2, 'relative_l2': (error / energy) ** .5,
                           'maximum_over_reference_peak': maximum / peak}
    assert report['numerical']['relative_l2'] <= 2e-5 and maximum / peak <= 2e-4
    report['passed'] = True
    (args.output / 'report.json').write_text(json.dumps(report, indent=2))
    print(json.dumps(report), flush=True)


if __name__ == '__main__':
    main()
