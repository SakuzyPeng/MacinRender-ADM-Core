#!/usr/bin/env python3
"""Native history/update lifecycle regression; correctness only, never a benchmark."""
import argparse
import ctypes as c
import json
import math
import os
from pathlib import Path


U = c.c_uint32
F = c.c_float
P = c.POINTER(F)


class Params(c.Structure):
    _fields_ = [(n, U) for n in ('rate', 'fft', 'taps', 'stride', 'buckets', 'rows', 'images', 'parts')] + [
        ('materials', P), ('material_floats', c.c_size_t), ('propagation', P), ('propagation_floats', c.c_size_t)]


class Term(c.Structure):
    _fields_ = [('row', U), ('fraction', U), ('shift', U), ('alpha', F), ('weight', F)]


class Job(c.Structure):
    _fields_ = [('material', U), ('fraction', U), ('shift', U), ('counts', U),
                ('alpha', F), ('gain', F), ('terms', Term * 12)]


class Range(c.Structure):
    _fields_ = [('first', U), ('count', U)]


class Update(c.Structure):
    _fields_ = [(n, U) for n in ('slot', 'branch', 'first', 'count', 'parts', 'support')]


class Task(c.Structure):
    _fields_ = [(n, c.c_uint64) for n in ('start', 'end', 'block_start')] + [
        (n, U) for n in ('slot', 'branch', 'parts', 'begin', 'frames', 'first')]


class Frame(c.Structure):
    _fields_ = [('active', U), ('reset', U), ('heads', U * 2), ('frequency', P),
                ('jobs', c.POINTER(Job)), ('job_count', c.c_size_t),
                ('ranges', c.POINTER(Range)), ('range_count', c.c_size_t),
                ('updates', c.POINTER(Update)), ('update_count', c.c_size_t),
                ('tasks', c.POINTER(Task)), ('task_count', c.c_size_t)]


def render(library, fixture, kernels):
    dll = c.CDLL(str(library.resolve()))
    dll.room_gpu_session_create.argtypes = [c.c_char_p, c.c_char_p, c.POINTER(Params)]
    dll.room_gpu_session_create.restype = c.c_void_p
    dll.room_gpu_session_error.restype = c.c_char_p
    dll.room_gpu_session_destroy.argtypes = [c.c_void_p]
    dll.room_gpu_session_destroy.restype = None
    dll.room_gpu_session_begin.argtypes = [c.c_void_p, U]
    dll.room_gpu_session_voice.argtypes = [c.c_void_p, U, c.POINTER(Frame)]
    dll.room_gpu_session_submit.argtypes = [c.c_void_p]
    dll.room_gpu_session_finish.argtypes = [c.c_void_p, P, c.c_size_t]
    _, rate, _, fft, taps, stride, buckets, _, _, rows, images = map(
        int, (fixture / 'config.txt').read_text().split())
    data = [(fixture / name).read_bytes() for name in ('materials.bin', 'propagation.bin')]
    buffers = [(F * (len(v) // 4)).from_buffer_copy(v) for v in data]
    parts = (taps + 255) // 256
    params = Params(rate, fft, taps, stride, buckets, rows, images, parts,
                    buffers[0], len(buffers[0]), buffers[1], len(buffers[1]))
    context = dll.room_gpu_session_create(str(fixture.resolve()).encode(), str(kernels.resolve()).encode(),
                                          c.byref(params))
    if not context:
        raise RuntimeError(dll.room_gpu_session_error().decode())

    def check(code):
        if code:
            raise RuntimeError(dll.room_gpu_session_error().decode())

    def block(head, impulse=0., reset=False, update=False, task=False, active=True):
        spectrum = (F * 1028)()
        # A 512-point FFT of an impulse in the new half of the overlap-save window.
        for branch in range(2):
            for k in range(257):
                spectrum[(branch * 257 + k) * 2] = impulse * (-1 if k % 2 else 1)
        job = Job()
        job.counts = 0x0101
        job.gain = 1
        job.terms[0] = Term(0, 0, 0, 0., 1.)
        job.terms[6] = Term(1, 0, 0, 0., 1.)
        ranges = (Range * buckets)()
        ranges[0] = Range(0, 1)
        target = Update(0, 0, 0, buckets, 8, 2048)
        work = Task(0, 2**64 - 1, 0, 0, 0, 8, 0, 256, 1)
        frame = Frame(int(active), int(reset), (U * 2)(head, head), spectrum,
                      c.pointer(job), int(update), ranges, buckets if update else 0,
                      c.pointer(target), int(update), c.pointer(work), int(task))
        output = (F * 1024)(*[float('nan')] * 1024)
        check(dll.room_gpu_session_begin(context, 1))
        check(dll.room_gpu_session_voice(context, 0, c.byref(frame)))
        check(dll.room_gpu_session_submit(context))
        check(dll.room_gpu_session_finish(context, output, len(output)))
        if not all(math.isfinite(v) for v in output):
            raise AssertionError('Nonfinite native output')
        return bytes(output), max(map(abs, output))

    results = []
    try:
        for i in range(8):
            payload, peak = block(i, impulse=1. / (i + 1), reset=i == 0)
            assert peak == 0., 'History-only work must return silence'
            results.append(payload)
        payload, peak = block(8, update=True, task=True)
        assert peak > 1e-9, 'Target must see input uploaded during history-only work'
        results.append(payload)
        payload, peak = block(9, impulse=.25, task=True)
        assert peak > 1e-9
        results.append(payload)
        prior = payload
        payload, peak = block(9, active=False)
        assert peak == 0.
        results.append(payload)
        payload, _ = block(9, active=False, task=True)
        assert payload == prior, 'Empty output must not clear retained history'
        results.append(payload)
        payload, peak = block(9, active=False, reset=True)
        assert peak == 0.
        results.append(payload)
        payload, peak = block(9, active=False, task=True)
        assert peak == 0., 'History-only reset must discard the previous epoch'
        results.append(payload)
        for i in range(10, 18):
            payload, peak = block(i % parts, impulse=.125)
            assert peak == 0.
            results.append(payload)
        payload, peak = block(18 % parts, task=True)
        assert peak > 1e-9, 'Warm input after reset must remain available'
        results.append(payload)
        check(dll.room_gpu_session_begin(context, 0))
        check(dll.room_gpu_session_submit(context))
        check(dll.room_gpu_session_finish(context, (F * 1)(), 0))
    finally:
        dll.room_gpu_session_destroy(context)
    return results


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    for name in ('baseline', 'candidate', 'fixture', 'kernels', 'cuda-bin'):
        parser.add_argument('--' + name, required=True, type=Path)
    args = parser.parse_args()
    assert (c.sizeof(Params), c.sizeof(Job), c.sizeof(Update), c.sizeof(Task), c.sizeof(Frame)) == (64, 264, 24, 48, 88)
    with os.add_dll_directory(str(args.cuda_bin.resolve())):
        baseline = render(args.baseline, args.fixture, args.kernels)
        candidate = render(args.candidate, args.fixture, args.kernels)
    assert baseline == candidate, 'Native history/update lifecycle differs from the reference DLL'
    print(json.dumps({'passed': True, 'compared_blocks': len(baseline), 'bit_identical': True,
                      'empty_voice_batch': True, 'scope': 'Native lifecycle correctness; no timing evidence'}))


if __name__ == '__main__':
    main()
