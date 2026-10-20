#!/usr/bin/env python3
"""Opt-in ROOM CUDA offline rendering, file streaming seek and reusable preview checks.

Use Release binaries and explicit 96 kHz resources. Comparisons include the
complete output and tail; only the separate seek contract uses a fixed window.
"""
import argparse
import array
import ctypes as C
import hashlib
import importlib.util
import json
import math
import os
from pathlib import Path
import subprocess
import sys

ROOT = Path(__file__).resolve().parents[2]
spec = importlib.util.spec_from_file_location('room96', ROOT / 'tests/unit/room_96k_test.py')
room96 = importlib.util.module_from_spec(spec)
spec.loader.exec_module(room96)


def preview(args):
    api = room96.Api(args.library)
    def bind(name, result, *parameters):
        function = getattr(api.lib, name)
        function.restype, function.argtypes = result, list(parameters)
        return function
    ptr, text, integer = C.c_void_p, C.c_char_p, C.c_int
    create_options = bind('adm_create_render_options', ptr)
    destroy_options = bind('adm_destroy_render_options', None, ptr)
    set_renderer = bind('adm_render_options_set_renderer', integer, ptr, integer)
    set_labels = bind('adm_render_options_set_input_channel_labels', integer, ptr, text)
    set_quality = bind('adm_render_options_set_room_quality', integer, ptr, integer)
    set_room = bind('adm_render_options_set_room', integer, ptr, C.POINTER(room96.Room))
    set_limit = bind('adm_render_options_set_peak_limit', None, ptr, integer)
    create = bind('adm_create_preview_session', integer, ptr, text, ptr, C.POINTER(ptr))
    destroy = bind('adm_destroy_preview_session', None, ptr)
    render = bind('adm_preview_render_window', integer, ptr, C.c_double, C.c_double, text, ptr, ptr, ptr)
    options, handle, session = create_options(), ptr(), ptr()
    assert options
    try:
        api.ok(api.lib.adm_room_load(api.context, str(args.room).encode(), C.byref(handle)))
        room = room96.Room(handle=handle, hrir=str(args.hrir).encode(), metres=1.5)
        api.ok(set_renderer(options, 8))
        api.ok(set_labels(options, b'C'))
        api.ok(set_quality(options, 1))
        api.ok(set_room(options, C.byref(room)))
        set_limit(options, 0)
        api.ok(create(api.context, str(args.output / 'input.wav').encode(), options, C.byref(session)))
        for name, start, end in [('full', 0., 0.), ('seek', .02, .06), ('repeat', 0., 0.)]:
            path = args.output / ('preview-' + name + '.wav')
            api.ok(render(session, start, end, str(path).encode(), None, None, None))
        full = room96.wav_read(args.output / 'preview-full.wav')[2]
        assert full == room96.wav_read(args.output / 'cuda.wav')[2], 'preview / offline PCM differs'
        assert full == room96.wav_read(args.output / 'preview-repeat.wav')[2], 'preview reuse changed PCM'
        assert full[1920 * 8:5760 * 8] == room96.wav_read(args.output / 'preview-seek.wav')[2]
        print(json.dumps({'case': 'preview', 'frames': len(full) // 8, 'passed': True}))
    finally:
        if session.value:
            destroy(session)
        if handle.value:
            api.lib.adm_room_destroy(handle)
        destroy_options(options)
        api.lib.adm_destroy_context(api.context)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    for name in ['cli', 'library', 'room', 'hrir', 'output']:
        parser.add_argument('--' + name, type=lambda p: Path(p).resolve(), required=True)
    parser.add_argument('--baseline-cli', type=lambda p: Path(p).resolve())
    parser.add_argument('--case', choices=['preview'])
    args = parser.parse_args()
    if args.case:
        preview(args)
        return
    args.output.mkdir(parents=True, exist_ok=False)
    samples = [0.] * 1024 + [.02 * math.sin(2 * math.pi * 30000 * i / 96000)
                            + .01 * math.sin(2 * math.pi * 40000 * i / 96000) for i in range(8193)]
    source = args.output / 'input.wav'
    room96.wav_write(source, samples, 96000)
    outputs, report = {}, {'commands': [], 'cases': {}}
    cases = [('cpu', 'realtime', '0'), ('cuda', 'realtime', '1'),
             ('exact', 'exact', '0'), ('exact-opt-in', 'exact', 'invalid'),
             ('seek', 'realtime', '1'), ('silence', 'realtime', '1')]
    if args.baseline_cli:
        cases += [('baseline-exact', 'exact', '0'), ('baseline-cpu', 'realtime', '0')]
    for name, quality, selection in cases:
        input_path = source
        if name == 'silence':
            input_path = args.output / 'silence-input.wav'
            room96.wav_write(input_path, [0.] * len(samples), 96000)
        target = args.output / (name + '.wav')
        cli = args.baseline_cli if name.startswith('baseline-') else args.cli
        command = [str(cli), 'render', '-i', str(input_path), '-o', str(target),
                   '--input-channels', 'C', '--renderer', 'room-binaural', '--room', str(args.room),
                   '--room-hrir', str(args.hrir), '--room-metres-per-unit', '1.5',
                   '--room-quality', quality, '--no-peak-limit']
        if name == 'seek':
            command += ['--start', '.02', '--end', '.06']
        print('offline / file streaming', name, flush=True)
        result = subprocess.run(command, env=dict(os.environ, MR_ADM_ROOM_CUDA=selection,
                                                 MR_ADM_ROOM_WORKERS='8'), capture_output=True, timeout=240)
        (args.output / (name + '.stdout')).write_bytes(result.stdout)
        (args.output / (name + '.stderr')).write_bytes(result.stderr)
        assert result.returncode == 0, result.stderr.decode(errors='replace')
        rate, channels, pcm = room96.wav_read(target)
        assert (rate, channels) == (96000, 2)
        assert all(math.isfinite(v) for v in array.array('f', pcm))
        outputs[name] = pcm
        report['commands'].append({'selection': selection, 'arguments': command})
        report['cases'][name] = {'frames': len(pcm) // 8, 'sha256': hashlib.sha256(pcm).hexdigest()}
        (args.output / 'report.json').write_text(json.dumps(report, indent=2))
    assert outputs['exact'] == outputs['exact-opt-in'], 'exact observed the CUDA opt-in'
    if args.baseline_cli:
        assert outputs['exact'] == outputs['baseline-exact'], 'exact differs from the frozen baseline'
        assert outputs['cpu'] == outputs['baseline-cpu'], 'CPU realtime differs from the frozen baseline'
    assert outputs['seek'] == outputs['cuda'][1920 * 8:5760 * 8], 'file streaming seek changed PCM'
    assert all(v == 0 for v in array.array('f', outputs['silence']))
    assert len(outputs['cpu']) == len(outputs['cuda']) == len(outputs['silence'])
    reference, actual = (array.array('f', outputs[name]) for name in ['cpu', 'cuda'])
    error = math.fsum((float(a) - float(b)) ** 2 for a, b in zip(reference, actual))
    energy = math.fsum(float(v) ** 2 for v in reference)
    peak_error = max(abs(float(a) - float(b)) for a, b in zip(reference, actual)) / max(abs(v) for v in reference)
    report['relative_l2'] = (error / energy) ** .5
    report['maximum_over_reference_peak'] = peak_error
    assert report['relative_l2'] <= 2e-5 and peak_error <= 2e-4
    command = [sys.executable, str(Path(__file__).resolve()), '--case', 'preview']
    for key in ['cli', 'library', 'room', 'hrir', 'output']:
        command += ['--' + key, str(getattr(args, key))]
    result = subprocess.run(command, env=dict(os.environ, MR_ADM_ROOM_CUDA='1', MR_ADM_ROOM_WORKERS='8'),
                            capture_output=True, timeout=240)
    (args.output / 'preview.stdout').write_bytes(result.stdout)
    (args.output / 'preview.stderr').write_bytes(result.stderr)
    assert result.returncode == 0, result.stdout.decode(errors='replace') + result.stderr.decode(errors='replace')
    report['preview'] = json.loads(result.stdout.decode().strip().splitlines()[-1])
    report['passed'] = True
    (args.output / 'report.json').write_text(json.dumps(report, indent=2))
    print(json.dumps(report), flush=True)


if __name__ == '__main__':
    main()
