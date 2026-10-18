#!/usr/bin/env python3
"""Public CLI/C ABI integration at 96 kHz; no audio device or private data.

Invoke with a Release mradm and its C API shared library. The offline renderer
exercises the same file streaming process/seek path, including finite preroll.
"""
import array
import ctypes as C
import hashlib
import importlib.util
import json
import math
import os
from pathlib import Path
import struct
import subprocess
import sys
import tempfile
import time

ROOT = Path(__file__).resolve().parents[2]
U32, I32, U64, I64, F32, PTR = C.c_uint32, C.c_int32, C.c_uint64, C.c_int64, C.c_float, C.c_void_p


class Sized(C.Structure):
    def __init__(self, **values):
        super().__init__()
        self.struct_size = C.sizeof(self)
        for key, value in values.items():
            setattr(self, key, value)


class Renderer(Sized):
    _fields_ = [('struct_size', U32), ('renderer', I32), ('layout', C.c_char_p), ('sofa', C.c_char_p),
                ('geometry', I32), ('spread', I32), ('binaural_spread', I32), ('lfe', I32), ('smoothing', U32)]


class Config(Sized):
    _fields_ = [('struct_size', U32), ('rendering', Renderer), ('input_rate', U32), ('output_rate', U32),
                ('queue', U32), ('ring', U32), ('watermark', U32), ('reserved', U32), ('queue_bytes', U64)]


class Room(Sized):
    _fields_ = [('struct_size', U32), ('reserved', U32), ('handle', PTR), ('hrir', C.c_char_p), ('metres', C.c_double)]


class Element(Sized):
    _fields_ = [('struct_size', U32), ('role', I32), ('id', U64), ('label', C.c_char_p), ('flags', U64),
                ('position', I32), ('reserved', U32), ('x', F32), ('y', F32), ('z', F32), ('semantic', PTR)]


class State(Sized):
    _fields_ = [('struct_size', U32), ('reserved', U32), ('valid', U64), ('active', I32),
                ('gain', F32), ('x', F32), ('y', F32), ('z', F32), ('width', F32), ('height', F32), ('depth', F32),
                ('diffuse', F32), ('divergence', F32), ('lock', I32), ('screen', I32), ('head', I32),
                ('azimuth_range', F32), ('position_range', F32), ('has_max', I32), ('max_distance', F32)]


class Initial(Sized):
    _fields_ = [('struct_size', U32), ('reserved', U32), ('id', U64), ('state', State)]


class Plane(Sized):
    _fields_ = [('struct_size', U32), ('reserved', U32), ('id', U64), ('samples', C.POINTER(F32)),
                ('count', U32), ('stride', U32), ('signal', I32), ('reserved2', U32)]


class Frame(Sized):
    _fields_ = [('struct_size', U32), ('flags', U32), ('epoch', U64), ('generation', U64), ('start', I64),
                ('duration', U32), ('pcm_count', U32), ('pcm', C.POINTER(Plane)), ('initial_count', U32),
                ('update_count', U32), ('initial', C.POINTER(Initial)), ('updates', PTR)]


class Pull(Sized):
    _fields_ = [('struct_size', U32), ('flags', U32), ('epoch', U64), ('first', U64), ('frames', U32), ('requested', U32)]


def wav_write(path, samples, rate):
    pcm = struct.pack('<' + str(len(samples)) + 'f', *samples)
    fmt = struct.pack('<HHIIHH', 3, 1, rate, rate * 4, 4, 32)
    body = b'WAVEfmt ' + struct.pack('<I', len(fmt)) + fmt + b'data' + struct.pack('<I', len(pcm)) + pcm
    path.write_bytes(b'RIFF' + struct.pack('<I', len(body)) + body)


def wav_read(path):
    raw = path.read_bytes()
    assert raw[:4] in (b'RIFF', b'RF64', b'BW64'), raw[:12]
    data_size = None
    position, rate, channels, pcm = 12, None, None, None
    while position + 8 <= len(raw):
        tag, size = struct.unpack_from('<4sI', raw, position)
        if tag == b'ds64':
            data_size = struct.unpack_from('<Q', raw, position + 16)[0]
        if tag == b'data' and size == 0xffffffff:
            assert data_size is not None
            size = data_size
        payload = raw[position + 8:position + 8 + size]
        if tag == b'fmt ':
            fmt, channels, rate, _, _, bits = struct.unpack_from('<HHIIHH', payload)
            assert bits == 32 and (fmt == 3 or (fmt == 65534 and struct.unpack_from('<H', payload, 24)[0] == 3))
        if tag == b'data':
            pcm = payload
        position += 8 + size + size % 2
    assert pcm is not None
    return rate, channels, pcm


class Api:
    def __init__(self, library):
        # Use the native CLI's system C++ runtime before adding the configured
        # fmt directory. PATH can also contain older JDK/Conda MSVC runtimes.
        self.dll_directories = []
        self.runtime_libraries = []
        if os.name == 'nt':
            for name in ['msvcp140.dll', 'MSVCP140_ATOMIC_WAIT.dll']:
                self.runtime_libraries.append(C.WinDLL(str(Path(os.environ['SystemRoot']) / 'System32' / name)))
            self.dll_directories.append(os.add_dll_directory(str(library.parent)))
            for directory in os.environ.get('PATH', '').split(os.pathsep):
                if directory and (Path(directory) / 'fmt.dll').is_file():
                    self.dll_directories.append(os.add_dll_directory(directory))
                    break
        self.lib = C.CDLL(str(library))
        def bind(name, result, *args):
            fn = getattr(self.lib, name); fn.restype = result; fn.argtypes = list(args)
        bind('adm_create_context', PTR)
        bind('adm_destroy_context', None, PTR)
        bind('adm_context_last_error_message', C.c_char_p, PTR)
        bind('adm_room_load', I32, PTR, C.c_char_p, C.POINTER(PTR))
        bind('adm_room_destroy', None, PTR)
        bind('adm_room_inspect_with_quality', I32, PTR, C.POINTER(Room), I32, C.POINTER(PTR))
        bind('adm_free_string', None, PTR)
        bind('adm_create_scene_stream_with_room_quality', I32, PTR, C.POINTER(Config), C.POINTER(Room), I32, C.c_char_p, C.POINTER(PTR))
        bind('adm_destroy_scene_stream', None, PTR)
        bind('adm_scene_stream_begin_epoch', I32, PTR, U64, I64)
        bind('adm_scene_stream_configure_generation', I32, PTR, U64, U64, C.POINTER(Element), U32)
        bind('adm_scene_stream_submit_frame', I32, PTR, C.POINTER(Frame), U32, C.POINTER(I32))
        bind('adm_scene_stream_signal_end', I32, PTR, U64, I64)
        bind('adm_scene_stream_pull', I32, PTR, C.POINTER(F32), U32, C.POINTER(Pull))
        self.context = self.lib.adm_create_context()
        assert self.context

    def ok(self, code):
        assert code == 0, (code, self.lib.adm_context_last_error_message(self.context))

    def scene(self, room_file, hrir, samples, chunks, output_rate=96000, quality=0, epoch_twice=False):
        room_handle, stream = PTR(), PTR()
        self.ok(self.lib.adm_room_load(self.context, str(room_file).encode(), C.byref(room_handle)))
        room = Room(handle=room_handle, hrir=str(hrir).encode(), metres=1.5)
        report = PTR()
        self.ok(self.lib.adm_room_inspect_with_quality(self.context, C.byref(room), quality, C.byref(report)))
        info = json.loads(C.string_at(report).decode()); self.lib.adm_free_string(report)
        assert info['sample_rate'] == 96000
        assert info['reflection_update_frames'] == (2048 if quality else 0)
        config = Config(rendering=Renderer(renderer=8, layout=b'binaural'), input_rate=96000,
                        output_rate=output_rate, ring=32768, watermark=1)
        self.ok(self.lib.adm_create_scene_stream_with_room_quality(self.context, C.byref(config), C.byref(room), quality, None, C.byref(stream)))
        self.lib.adm_room_destroy(room_handle)
        try:
            outputs = []
            for epoch in range(1, 3 if epoch_twice else 2):
                self.ok(self.lib.adm_scene_stream_begin_epoch(stream, epoch, 0))
                element = Element(id=7, role=1, label=b'M+000', position=1, y=1)
                self.ok(self.lib.adm_scene_stream_configure_generation(stream, epoch, 1, C.byref(element), 1))
                initial = Initial(id=7, state=State(valid=7, active=1, gain=1, y=1, azimuth_range=45))
                position, packet = 0, 0
                while position < len(samples):
                    count = min(chunks[packet % len(chunks)], len(samples) - position)
                    pcm = (F32 * count)(*samples[position:position + count])
                    plane = Plane(id=7, samples=pcm, count=count, stride=1, signal=1)
                    frame = Frame(flags=1, epoch=epoch, generation=1, start=position,
                                  duration=count, pcm_count=1, pcm=C.pointer(plane),
                                  initial_count=1 if position == 0 else 0,
                                  initial=C.pointer(initial) if position == 0 else None)
                    submitted = I32()
                    self.ok(self.lib.adm_scene_stream_submit_frame(stream, C.byref(frame), 1000, C.byref(submitted)))
                    assert submitted.value == 0
                    position += count; packet += 1
                self.ok(self.lib.adm_scene_stream_signal_end(stream, epoch, len(samples)))
                output, deadline = bytearray(), time.monotonic() + 30
                while True:
                    pcm = (F32 * 514)(); result = Pull()
                    self.ok(self.lib.adm_scene_stream_pull(stream, pcm, 257, C.byref(result)))
                    assert not result.flags & 8
                    output += bytes(pcm)[:result.frames * 8]
                    if result.flags & 4:
                        break
                    assert time.monotonic() < deadline, 'Scene output timeout'
                    if not result.frames:
                        time.sleep(.001)
                expected = ((len(samples) + info['tail_frames']) * output_rate + 95999) // 96000
                assert len(output) // 8 == expected, (len(output) // 8, expected)
                outputs.append(bytes(output))
            assert all(output == outputs[0] for output in outputs)
            return outputs[0]
        finally:
            self.lib.adm_destroy_scene_stream(stream)


def main():
    assert C.sizeof(Room) == 32 and C.sizeof(Renderer) == 48 and C.sizeof(Config) == 88
    cli, library = (Path(p).resolve() for p in sys.argv[1:3])
    spec = importlib.util.spec_from_file_location('room_fixture', Path(__file__).with_name('room_fixture.py'))
    fixture = importlib.util.module_from_spec(spec); spec.loader.exec_module(fixture)
    with tempfile.TemporaryDirectory(prefix='mradm-room96-') as directory:
        root = Path(directory); fixture.fixture(root)
        for name in ['room.json', 'hrir.json']:
            p = root / name; value = json.loads(p.read_text()); value['sample_rate'] = 96000
            p.write_text(json.dumps(value))
        room = root / 'room.json'
        bank = ROOT / 'tests/fixtures/sofa/distance-96'
        # Scene has an existing 10 ms output startup fade; silence isolates
        # the shared room PCM contract from that presentation envelope.
        samples = [0.] * 1024 + [float(.02 * math.sin(2 * math.pi * 30000 * i / 96000)
                         + .01 * math.sin(2 * math.pi * 40000 * i / 96000)) for i in range(8193)]
        samples = list(struct.unpack('<' + str(len(samples)) + 'f', struct.pack('<' + str(len(samples)) + 'f', *samples)))
        source, full, trim = root / 'input.wav', root / 'full.wav', root / 'trim.wav'
        wav_write(source, samples, 96000)
        command = [str(cli), 'render', '-i', str(source), '--input-channels', 'C', '--renderer', 'room-binaural',
                   '--room', str(room), '--room-hrir', str(bank), '--no-peak-limit']
        subprocess.run(command + ['-o', str(full)], check=True, capture_output=True)
        rate, channels, pcm = wav_read(full); assert (rate, channels) == (96000, 2)
        subprocess.run(command + ['-o', str(trim), '--start', '.02', '--end', '.06'], check=True, capture_output=True)
        _, _, window = wav_read(trim)
        assert window == pcm[1920 * 8:5760 * 8], '96k file streaming seek/crop changed PCM'
        api = Api(library)
        try:
            fixed = api.scene(room, bank, samples, [256], epoch_twice=True)
            fragmented = api.scene(room, bank, samples, [1, 173, 733, 19, 257])
            assert fixed == fragmented, '96k Scene packetization changes PCM'
            # Both paths retain the same declared FIFO and finite tail.
            if fixed != pcm:
                a = array.array('f'); a.frombytes(fixed)
                b = array.array('f'); b.frombytes(pcm)
                error = sum((x-y)**2 for x,y in zip(a,b))
                energy = sum(x*x for x in b)
                raise AssertionError(dict(reason='file / Scene PCM mismatch', relative_l2=(error/energy)**.5,
                                          first_scene=next(i for i,v in enumerate(a) if abs(v)>1e-6),
                                          first_file=next(i for i,v in enumerate(b) if abs(v)>1e-6),
                                          scene_peak=max(abs(v) for v in a), file_peak=max(abs(v) for v in b),
                                          first_diff=next(i for i,(x,y) in enumerate(zip(a,b)) if abs(x-y)>1e-7),
                                          last_diff=max(i for i,(x,y) in enumerate(zip(a,b)) if abs(x-y)>1e-7)))
            realtime = api.scene(room, bank, samples, [256], quality=1)
            assert realtime == api.scene(room, bank, samples, [113, 7, 1024], quality=1)
            resampled = api.scene(room, bank, samples, [256], output_rate=48000)
            mismatch = json.loads(room.read_text()); mismatch['sample_rate'] = 48000
            wrong = root / 'wrong-rate.json'; wrong.write_text(json.dumps(mismatch))
            failed = subprocess.run([str(cli), 'render', '-i', str(source), '--input-channels', 'C', '-o', str(root/'invalid.wav'),
                                     '--renderer', 'room-binaural', '--room', str(wrong), '--room-hrir', str(bank)], capture_output=True)
            assert failed.returncode != 0
            print(json.dumps(dict(passed=True, sample_rate=96000, file_frames=len(pcm)//8,
                                  scene_frames=len(fixed)//8, output_48k_frames=len(resampled)//8,
                                  file_pcm_sha256=hashlib.sha256(pcm).hexdigest(),
                                  fragmented_identical=True, epoch_reset_identical=True, file_seek_identical=True)))
        finally:
            api.lib.adm_destroy_context(api.context)


if __name__ == '__main__':
    main()
