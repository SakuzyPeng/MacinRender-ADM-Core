#!/usr/bin/env python3
"""Windows CUDA EQ tail lifecycle test using an independent sparse-FIR oracle."""
import argparse
import ctypes as c
import json
import math
import os

from pathlib import Path


class Command(c.Structure):
    _fields_ = [('head', c.c_uint32), ('flags', c.c_uint32)]


def run(library, fixture, kernels):
    dll = c.CDLL(str(library.resolve()))
    ptr = c.c_void_p
    floats = c.POINTER(c.c_float)
    dll.room_gpu_eq_create.argtypes = [c.c_char_p, c.c_char_p, c.c_uint32, floats, c.c_size_t]
    dll.room_gpu_eq_create.restype = ptr
    dll.room_gpu_eq_submit.argtypes = [ptr, c.POINTER(Command), c.c_size_t, floats, c.c_size_t]
    dll.room_gpu_eq_finish.argtypes = [ptr, floats, c.c_size_t]
    dll.room_gpu_eq_destroy.argtypes = [ptr]
    dll.room_gpu_eq_destroy.restype = None
    dll.room_gpu_session_error.restype = c.c_char_p
    parts = 3
    weights = [.5, .25, .125]
    filters = (c.c_float * (parts * 2049 * 2))()
    for part, weight in enumerate(weights):
        for k in range(2049):
            filters[(part * 2049 + k) * 2] = weight
    context = dll.room_gpu_eq_create(str(fixture.resolve()).encode(), str(kernels.resolve()).encode(),
                                      parts, filters, len(filters))
    if not context:
        raise RuntimeError(dll.room_gpu_session_error().decode())
    histories = [[[0.] * 2048 for _ in range(parts)] for _ in range(64)]
    heads = [parts - 1] * 64
    errors = []
    try:
        for step, count in enumerate([64, 64, 64, 64, 16, 2, 64, 64, 64]):
            flags = [0] * count
            for i in range(count):
                if step == 0:
                    flags[i] = 1 | 4
                elif step == 1:
                    flags[i] = 4 if i % 2 == 0 else 0
                elif step == 2:
                    flags[i] = 4 | (2 if i % 2 else 0)
                elif step == 3:
                    flags[i] = (1 | 4) if i == 0 else (2 | 4) if i == 1 else 4 if i % 4 == 0 else 0
                elif step == 4:
                    flags[i] = 4 if i % 3 == 0 else 0
                elif step == 5:
                    flags[i] = 1 if i == 1 else 0
                elif step == 7:
                    flags[i] = 2 if i % 2 else 0
                else:
                    flags[i] = 4
            for i, flag in enumerate(flags):
                if flag & 1:
                    histories[i] = [[0.] * 2048 for _ in range(parts)]
                    heads[i] = parts - 1
            # Fork from the old direct history, before either new row is written.
            for i in range(1, count, 2):
                if flags[i] & 2:
                    histories[i] = [row.copy() for row in histories[i - 1]]
                    heads[i] = heads[i - 1]
            windows = (c.c_float * (count * 4096))()
            commands = (Command * count)()
            expected = [0.] * (count * 2048)
            for i, flag in enumerate(flags):
                if flag & 4:
                    for k, value in enumerate(histories[i][heads[i]]):
                        windows[i * 4096 + k] = value
                    row = [((k + step * 7 + i * 13) % 19 - 9) / 64. + i / 4096. for k in range(2048)]
                    for k, value in enumerate(row):
                        windows[i * 4096 + 2048 + k] = value
                    heads[i] = (heads[i] + 1) % parts
                    histories[i][heads[i]] = row
                    for k in range(2048):
                        expected[i * 2048 + k] = sum(weights[j] * histories[i][(heads[i] - j) % parts][k]
                                                     for j in range(parts))
                commands[i] = Command(heads[i], flag)
            status = dll.room_gpu_eq_submit(context, commands, count, windows, len(windows))
            if status:
                raise RuntimeError(dll.room_gpu_session_error().decode())
            if step == 0:
                assert dll.room_gpu_eq_submit(context, commands, count, windows, len(windows)) != 0
            output = (c.c_float * (count * 2048))()
            if step == 0:
                assert dll.room_gpu_eq_finish(context, output, len(output) - 1) != 0
            assert dll.room_gpu_eq_finish(context, output, len(output)) == 0
            assert all(math.isfinite(v) for v in output)
            energy = math.fsum(v * v for v in expected)
            error = math.fsum((a - b) ** 2 for a, b in zip(expected, output))
            peak = max(map(abs, expected))
            maximum = max(abs(a - b) for a, b in zip(expected, output))
            if not peak:
                assert maximum == 0.
            else:
                assert math.sqrt(error / energy) <= 2e-5 and maximum / peak <= 2e-4
            errors.append({'step': step, 'channels': count, 'relative_l2': math.sqrt(error / energy) if energy else 0.,
                           'maximum_over_peak': maximum / peak if peak else 0.})
    finally:
        dll.room_gpu_eq_destroy(context)
    return {'passed': True, 'cases': errors, 'scope': 'Sparse complete tail FIR; state/FFT regression, no timing'}


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    for name in ('library', 'fixture', 'kernels', 'cuda-bin'):
        parser.add_argument('--' + name, type=Path, required=True)
    args = parser.parse_args()
    assert c.sizeof(Command) == 8
    with os.add_dll_directory(str(args.cuda_bin.resolve())):
        print(json.dumps(run(args.library, args.fixture, args.kernels)))


if __name__ == '__main__':
    main()
