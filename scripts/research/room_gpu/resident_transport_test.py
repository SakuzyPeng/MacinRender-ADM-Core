#!/usr/bin/env python3
"""Check a resident CUDA DLL against an analytical delayed FIR; no private resources or timing.

Use the kernel source matching the tested DLL. Fused DLLs require the archived
FFT-fusion source, while an unfused resident DLL can use session_kernels.cu.
"""
import argparse
import array
import ctypes as C
from concurrent.futures import ThreadPoolExecutor
import json
import math
import os
from pathlib import Path

from empty_graph_test import Frame, Job, Params, Range, Task, Term, Update


class BankDesc(C.Structure):
    _fields_ = [(name, C.c_uint32) for name in ['abi', 'size', 'rate', 'fft', 'rows']]


class Stats(C.Structure):
    _fields_ = [(name, C.c_uint64) for name in ['banks', 'engines', 'late_engines', 'device_bytes', 'pinned_bytes']]


def check_library(path, kernels, cross_thread=False):
    directory = os.add_dll_directory(str(path.parent.resolve()))
    dll = C.CDLL(str(path.resolve()))
    def bind(name, result, *arguments):
        fn = getattr(dll, 'mradm_room_cuda_' + name)
        fn.restype, fn.argtypes = result, list(arguments)
        return fn
    pointer, u32, size, floats = C.c_void_p, C.c_uint32, C.c_size_t, C.POINTER(C.c_float)
    error = bind('session_error', C.c_char_p)
    bank_create = bind('bank_create', pointer, C.POINTER(BankDesc), C.c_char_p, size)
    upload = bind('bank_upload', C.c_int, pointer, u32, size, floats, size)
    seal = bind('bank_seal', C.c_int, pointer)
    bank_destroy = bind('bank_destroy', None, pointer)
    create = bind('session_create', pointer, pointer, C.POINTER(Params))
    destroy = bind('session_destroy', None, pointer)
    begin = bind('session_begin', C.c_int, pointer, u32)
    voice = bind('session_voice', C.c_int, pointer, u32, C.POINTER(Frame))
    submit = bind('session_submit', C.c_int, pointer)
    native_finish = bind('session_finish', C.c_int, pointer, floats, size)
    stats = bind('stats', C.c_int, C.POINTER(Stats))
    def ok(value):
        assert value == 0, error().decode()
    def buffer(values):
        return (C.c_float * len(values)).from_buffer(values)
    delays, gains = [1900, 1856], [1., .5]
    bank_values = array.array('f', (value for delay, gain in zip(delays, gains) for k in range(2049)
                                  for value in [gain * math.cos(-math.tau * k * delay / 4096),
                                                0. if k in [0, 2048] else gain * math.sin(-math.tau * k * delay / 4096)]))
    ones = array.array('f', [1., 0.]) * (1025 * 2049)
    phases = array.array('f', (v for k in range(4096) for v in [math.cos(-math.tau * k / 4096),
                                                              math.sin(-math.tau * k / 4096)]))
    source = kernels.read_bytes()
    bank = bank_create(C.byref(BankDesc(1, C.sizeof(BankDesc), 96000, 4096, 2)), source, len(source))
    assert bank, error().decode()
    worker = ThreadPoolExecutor(max_workers=1) if cross_thread else None
    def finish(*args):
        def run():
            value = native_finish(*args)
            # The DLL error message belongs to the completion thread.
            assert value == 0, error().decode()
            return value
        return worker.submit(run).result() if worker else run()
    results = []
    try:
        for table, values in enumerate([bank_values, ones, phases]):
            ok(upload(bank, table, 0, buffer(values), len(values)))
        ok(seal(bank))
        parameters = Params(96000, 4096, 4096, 256, 1, 2, 1, 16, buffer(ones), 4098, buffer(ones), len(ones))
        for voices in [1, 32]:
            engine = create(bank, C.byref(parameters))
            assert engine, error().decode()
            epochs, energy, squared_error, maximum_error, peak = [], 0., 0., 0., 0.
            try:
                for epoch in range(3):
                    silent = epoch == 2
                    output = bytearray()
                    for block in range(32):
                        ok(begin(engine, voices))
                        have_task = block not in [9, 15]
                        have_update = block in [0, 11]
                        def amplitude(channel, index):
                            return (channel + 1) / 64. / (index + 1) if not silent and 0 <= index < 12 else 0.
                        for channel in range(voices):
                            spectrum = (C.c_float * 1028)()
                            for k in range(257):
                                gain = amplitude(channel, block - 1) + amplitude(channel, block) * (-1 if k % 2 else 1)
                                spectrum[k * 2] = gain * math.cos(-math.tau * k * 17 / 512)
                                spectrum[k * 2 + 1] = 0. if k in [0, 256] else gain * math.sin(-math.tau * k * 17 / 512)
                            job = Job()
                            job.counts, job.gain = 0x0101, 1.
                            job.terms[0], job.terms[6] = Term(0, 0, 0, 0., 1.), Term(1, 0, 0, 0., 1.)
                            ranges = Range(0, 1)
                            update = Update(0, 0, 0, 1, 8, 2048)
                            task = Task(0, 2**64 - 1, block * 256, 0, 0, 8, 0, 256, 1)
                            frame = Frame(1, int(block == 0), (u32 * 2)(block % 16, block % 16), spectrum,
                                          C.pointer(job), int(have_update), C.pointer(ranges), int(have_update),
                                          C.pointer(update), int(have_update), C.pointer(task), int(have_task))
                            ok(voice(engine, channel, C.byref(frame)))
                        ok(submit(engine))
                        pcm = (C.c_float * (voices * 1024))()
                        ok(finish(engine, pcm, len(pcm)))
                        assert all(math.isfinite(v) for v in pcm)
                        if silent or block >= 30 or not have_task:
                            assert all(v == 0. for v in pcm), 'Silent/empty output exposed stale samples'
                        for channel in range(voices):
                            for plane in range(4):
                                for i in range(256):
                                    sample = block * 256 + i - delays[plane % 2]
                                    expected = (amplitude(channel, sample // 256) * gains[plane]
                                                if have_task and plane < 2 and sample >= 0 and sample % 256 == 17 else 0.)
                                    actual = pcm[channel * 1024 + plane * 256 + i]
                                    difference = float(actual) - expected
                                    squared_error += difference * difference
                                    energy += expected * expected
                                    peak = max(peak, abs(expected))
                                    maximum_error = max(maximum_error, abs(difference))
                        output.extend(bytes(pcm))
                    epochs.append(bytes(output))
                    ok(begin(engine, 0)); ok(submit(engine)); ok(finish(engine, (C.c_float * 1)(), 0))
                assert epochs[0] == epochs[1], 'Reset changed the repeated epoch'
                l2, relative_peak = math.sqrt(squared_error / energy), maximum_error / peak
                assert l2 <= 2e-5 and relative_peak <= 2e-4, (l2, relative_peak)
                results.append({'voices': voices, 'epochs': 3, 'blocks_per_epoch': 32,
                                'relative_l2': l2, 'maximum_over_reference_peak': relative_peak,
                                'repeated_epoch_bit_identical': True, 'silence_and_empty_work': True})
                # Destruction owns the final submitted batch, even without a finish call.
                ok(begin(engine, 0)); ok(submit(engine))
            finally:
                destroy(engine)
    finally:
        bank_destroy(bank)
        if worker:
            worker.shutdown(wait=True)
    remaining = Stats(); ok(stats(C.byref(remaining)))
    assert not any(getattr(remaining, name) for name, _ in Stats._fields_)
    directory.close()
    return {'library': path.name, 'passed': True, 'cases': results, 'released_resources': True,
            'cross_thread_completion': cross_thread, 'destroy_with_pending_batch': True}


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--library', type=Path, action='append', required=True)
    parser.add_argument('--kernels', type=Path, default=Path(__file__).with_name('session_kernels.cu'),
                        help='kernel source matching the tested DLL configuration')
    parser.add_argument('--cross-thread', action='store_true', help='finish on a separate host thread')
    args = parser.parse_args()
    for library in args.library:
        print(json.dumps(check_library(library, args.kernels, args.cross_thread)), flush=True)


if __name__ == '__main__':
    main()
