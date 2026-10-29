#!/usr/bin/env python3
"""Black-box stereo HpTF ABI contracts; headless Scene plus silent hardware Monitor smoke."""
import argparse
import ctypes as C
import gc
import json
from pathlib import Path
import struct
import tempfile
import time

from room_96k_test import Api, Config, Renderer, Sized, Element, Plane, Frame, U32, I32, U64, I64, F32, PTR

F64 = C.c_double


class Band(Sized):
    _fields_ = [('struct_size', U32), ('kind', I32), ('enabled', I32), ('reserved', U32),
                ('frequency', F64), ('gain', F64), ('q', F64)]


class Parameters(Sized):
    _fields_ = [('struct_size', U32), ('mode', I32), ('left_count', U32), ('right_count', U32),
                ('left', C.POINTER(Band)), ('right', C.POINTER(Band)),
                ('left_preamp', F64), ('right_preamp', F64), ('revision', U64)]


class Files(Sized):
    _fields_ = [('struct_size', U32), ('mode', I32), ('left', C.c_char_p), ('right', C.c_char_p), ('revision', U64)]


class Info(Sized):
    _fields_ = [('struct_size', U32), ('enabled', I32), ('bands', U32), ('rate', U32),
                ('preamp', F32), ('trim', F32), ('peak', F32), ('reserved', U32), ('revision', U64)]


class StereoInfo(Sized):
    _fields_ = [('struct_size', U32), ('reserved', U32), ('left', Info), ('right', Info)]


class Legacy(Sized):
    _fields_ = [('struct_size', U32), ('path', C.c_char_p), ('mode', I32), ('reserved', U32), ('revision', U64)]


class Device(Sized):
    _fields_ = [('struct_size', U32), ('kind', I32), ('layout', C.c_char_p), ('id', C.c_char_p),
                ('geometry', I32), ('reserved', U32)]


class HptfApi(Api):
    def __init__(self, library):
        super().__init__(library)
        def bind(name, result, *args):
            fn = getattr(self.lib, name)
            fn.restype, fn.argtypes = result, list(args)
        for prefix in ['adm_monitor', 'adm_scene_output']:
            for suffix in ['set_hptf', 'set_hptf_stereo', 'set_hptf_stereo_parameters',
                           'get_hptf_info', 'get_hptf_stereo_info']:
                bind(prefix + '_' + suffix, I32, PTR, PTR)
            bind(prefix + '_last_error_message', C.c_char_p, PTR)
            bind(prefix + '_play', I32, PTR)
            bind(prefix + '_pause', I32, PTR)
        bind('adm_scene_output_set_hptf_stereo_ex', I32, PTR, PTR, C.POINTER(PTR))
        bind('adm_create_scene_stream', I32, PTR, C.POINTER(Config), C.POINTER(PTR))
        bind('adm_create_scene_output', I32, PTR, PTR, C.POINTER(Device), C.POINTER(PTR))
        bind('adm_destroy_scene_output', None, PTR)
        bind('adm_scene_output_begin_epoch', I32, PTR, U64, I64)
        bind('adm_create_render_options', PTR)
        bind('adm_destroy_render_options', None, PTR)
        bind('adm_render_options_set_renderer', I32, PTR, I32)
        bind('adm_render_options_set_output_layout', I32, PTR, C.c_char_p)
        bind('adm_create_monitor', I32, PTR, C.c_char_p, PTR, C.POINTER(PTR))
        bind('adm_destroy_monitor', None, PTR)
        bind('adm_monitor_seek', I32, PTR, F64)
        bind('adm_monitor_set_loop', I32, PTR, F64, F64)
        bind('adm_monitor_switch_backend', I32, PTR, PTR)
        bind('adm_monitor_set_output_device', I32, PTR, C.c_char_p)
        assert self.lib.adm_api_version_major() == 1 and self.lib.adm_api_version_minor() >= 50

    def info(self, handle, monitor=False):
        result = StereoInfo()
        prefix = 'adm_monitor' if monitor else 'adm_scene_output'
        self.ok(getattr(self.lib, prefix + '_get_hptf_stereo_info')(handle, C.byref(result)))
        assert result.left.revision == result.right.revision
        assert result.left.struct_size == result.right.struct_size == C.sizeof(Info)
        return result


class Scene:
    def __init__(self, api, layout=b'binaural', renderer=6, rate=48000):
        self.api, self.stream, self.output, self.epoch = api, PTR(), PTR(), 0
        cfg = Config(rendering=Renderer(renderer=renderer, layout=layout), input_rate=rate,
                     output_rate=rate, queue=96000, watermark=1)
        api.ok(api.lib.adm_create_scene_stream(api.context, C.byref(cfg), C.byref(self.stream)))
        try:
            api.ok(api.lib.adm_create_scene_output(api.context, self.stream, C.byref(Device(kind=2)), C.byref(self.output)))
        except BaseException:
            api.lib.adm_destroy_scene_stream(self.stream)
            raise

    def activate(self):
        self.epoch += 1
        self.api.ok(self.api.lib.adm_scene_output_begin_epoch(self.output, self.epoch, 0))
        return self.api.info(self.output)

    def play(self):
        # LFE needs no object-state metadata and generates a stereo device signal.
        element = Element(id=1, role=2)
        api = self.api
        api.ok(api.lib.adm_scene_stream_configure_generation(self.stream, self.epoch, 1, C.byref(element), 1))
        samples = (F32 * 48000)(*([0.001] * 48000))
        plane = Plane(id=1, samples=samples, count=len(samples), stride=1, signal=1)
        frame = Frame(flags=1, epoch=self.epoch, generation=1, duration=len(samples), pcm_count=1, pcm=C.pointer(plane))
        accepted = I32(-1)
        api.ok(api.lib.adm_scene_stream_submit_frame(self.stream, C.byref(frame), 1000, C.byref(accepted)))
        assert accepted.value == 0
        api.ok(api.lib.adm_scene_stream_signal_end(self.stream, self.epoch, len(samples)))
        api.ok(api.lib.adm_scene_output_play(self.output))

    def close(self):
        self.api.lib.adm_destroy_scene_output(self.output)
        self.api.lib.adm_destroy_scene_stream(self.stream)


def wait_revision(api, handle, revision, monitor=False):
    deadline = time.monotonic() + 5
    while True:
        info = api.info(handle, monitor)
        if info.left.revision == revision:
            return info
        assert time.monotonic() < deadline, ('revision timeout', revision, info.left.revision)
        time.sleep(.005)


def bands():
    return (Band * 1)(Band(kind=0, enabled=1, frequency=1000, gain=6, q=1.5))


def request(revision=1):
    # Deliberately allow all caller-owned storage to expire after the setter returns.
    left = bands()
    return Parameters(left_count=1, left=left, left_preamp=-3, right_preamp=-9, revision=revision)


def scene_contracts(api, root):
    scene = Scene(api)
    lib, handle = api.lib, scene.output
    try:
        lib.adm_scene_output_set_hptf_stereo_parameters.restype = I32
        value = request()
        api.ok(lib.adm_scene_output_set_hptf_stereo_parameters(handle, C.byref(value)))
        value.left[0].gain = 99
        value.right_preamp = 3
        del value
        gc.collect()
        info = scene.activate()
        assert (info.left.revision, info.left.bands, info.right.bands) == (1, 1, 0)
        assert (info.left.preamp, info.right.preamp) == (-3, -9)
        original = bytes(info)
        old = Info()
        C.memset(C.byref(old), 0xA5, C.sizeof(old))
        old.struct_size = C.sizeof(old)
        before = bytes(old)
        assert lib.adm_scene_output_get_hptf_info(handle, C.byref(old)) == 2
        assert bytes(old) == before

        # Invalid right ear must not publish a valid left ear or change the stored revision.
        bad = request(2)
        bad.right = bands()
        bad.right_count = 1
        bad.right[0].q = float('nan')
        assert lib.adm_scene_output_set_hptf_stereo_parameters(handle, C.byref(bad)) == 1
        assert '右耳' in lib.adm_scene_output_last_error_message(handle).decode()
        assert bytes(scene.activate()) == original
        for field, invalid in [('mode', 7), ('struct_size', 4), ('left_preamp', float('inf'))]:
            bad = request(2)
            setattr(bad, field, invalid)
            assert lib.adm_scene_output_set_hptf_stereo_parameters(handle, C.byref(bad)) == 1
            assert bytes(scene.activate()) == original
        bad = request(2)
        bad.left[0].reserved = 1
        assert lib.adm_scene_output_set_hptf_stereo_parameters(handle, C.byref(bad)) == 1

        # Future arrays carry an extended stride; each element must advertise the same size.
        class FutureBand(C.Structure):
            _fields_ = [('value', Band), ('tail', U64)]
        future = (FutureBand * 2)()
        for index, item in enumerate(future):
            item.value = Band(kind=0, enabled=1, frequency=500 + index * 1500, gain=-1, q=1)
            item.value.struct_size = C.sizeof(FutureBand)
            item.tail = 0x123456789ABCDEF0
        value = Parameters(left_count=2, left=C.cast(future, C.POINTER(Band)), revision=3)
        api.ok(lib.adm_scene_output_set_hptf_stereo_parameters(handle, C.byref(value)))
        assert scene.activate().left.bands == 2
        future[1].value.struct_size = C.sizeof(Band)
        assert lib.adm_scene_output_set_hptf_stereo_parameters(handle, C.byref(value)) == 1

        # Each ear has its own 32-enabled-band limit; disabled bands are still validated.
        many = (Band * 33)(*[Band(kind=0, enabled=1, frequency=200 + n * 100, gain=-.1, q=1) for n in range(33)])
        value = Parameters(left_count=32, right_count=32, left=many, right=many, revision=4)
        api.ok(lib.adm_scene_output_set_hptf_stereo_parameters(handle, C.byref(value)))
        info = scene.activate()
        assert info.left.bands == info.right.bands == 32
        value.right_count = 33
        assert lib.adm_scene_output_set_hptf_stereo_parameters(handle, C.byref(value)) == 1
        many[32].enabled = 0
        api.ok(lib.adm_scene_output_set_hptf_stereo_parameters(handle, C.byref(value)))
        many[32].q = 0
        assert lib.adm_scene_output_set_hptf_stereo_parameters(handle, C.byref(value)) == 1

        # Equivalent file and memory input, including a UTF-8 pathname.
        left, right = root / '左耳.txt', root / '右耳.txt'
        left.write_text('Preamp: -3 dB\nFilter 1: ON PK Fc 1000 Hz Gain 6 dB Q 1.5\n', encoding='utf-8')
        right.write_text('Preamp: -9 dB\n', encoding='utf-8')
        config = Files(left=str(left).encode(), right=str(right).encode(), revision=5)
        error = PTR()
        api.ok(lib.adm_scene_output_set_hptf_stereo_ex(handle, C.byref(config), C.byref(error)))
        assert not error
        from_file = bytes(scene.activate())
        value = request(5)
        api.ok(lib.adm_scene_output_set_hptf_stereo_parameters(handle, C.byref(value)))
        assert bytes(scene.activate()) == from_file
        config.right = str(root / 'missing.txt').encode()
        assert lib.adm_scene_output_set_hptf_stereo_ex(handle, C.byref(config), C.byref(error)) == 3
        assert error and '右耳' in C.string_at(error).decode()
        lib.adm_free_string(error)
        assert bytes(scene.activate()) == from_file
        config.right = None
        config.mode, config.revision = 1, 6
        api.ok(lib.adm_scene_output_set_hptf_stereo(handle, C.byref(config)))
        info = scene.activate()
        assert info.right.bands == 0 and info.right.enabled and info.right.preamp == 0
        assert info.left.peak <= 0 and info.right.peak <= 0
        assert abs(info.left.trim - info.right.trim) < 4e-6

        # Future/short output sizes preserve all bytes outside the advertised writable prefix.
        class FutureInfo(C.Structure):
            _fields_ = [('value', StereoInfo), ('tail', U64)]
        extended = FutureInfo(StereoInfo(), 0x123456789ABCDEF0)
        extended.value.struct_size = C.sizeof(extended)
        api.ok(lib.adm_scene_output_get_hptf_stereo_info(handle, C.byref(extended)))
        assert extended.tail == 0x123456789ABCDEF0
        partial = (C.c_ubyte * 96)(*([0xA5] * 96))
        C.cast(partial, C.POINTER(U32))[0] = 8
        api.ok(lib.adm_scene_output_get_hptf_stereo_info(handle, partial))
        assert bytes(partial)[8:] == b'\xA5' * 88

        # The legacy setter restores shared state, including legacy getter availability.
        config_old = Legacy(path=str(left).encode(), revision=7)
        api.ok(lib.adm_scene_output_set_hptf(handle, C.byref(config_old)))
        info = scene.activate()
        assert bytes(info.left) == bytes(info.right)
        api.ok(lib.adm_scene_output_get_hptf_info(handle, C.byref(old)))
        assert old.revision == 7
        value = Parameters(revision=8)
        api.ok(lib.adm_scene_output_set_hptf_stereo_parameters(handle, C.byref(value)))
        info = scene.activate()
        assert not info.left.enabled and not info.right.enabled

        scene.play()
        for revision in range(9, 32):
            value = request(revision)
            value.left_preamp = -revision / 4
            value.right_preamp = -revision / 2
            api.ok(lib.adm_scene_output_set_hptf_stereo_parameters(handle, C.byref(value)))
            applied = api.info(handle)
            if applied.left.revision >= 9:
                assert applied.left.preamp == -applied.left.revision / 4
                assert applied.right.preamp == -applied.left.revision / 2
        info = wait_revision(api, handle, 31)
        assert info.right.preamp == -15.5
        api.ok(lib.adm_scene_output_pause(handle))
        assert scene.activate().left.revision == 31
    finally:
        scene.close()
    unsupported = Scene(api, b'4+7+0', 2)
    try:
        assert lib.adm_scene_output_set_hptf_stereo_parameters(unsupported.output, C.byref(Parameters())) == 2
    finally:
        unsupported.close()


def silent_adm(path):
    xml = (Path(__file__).parents[1] / 'fixtures/adm/objects-stream-only.xml').read_bytes()
    def chunk(tag, payload):
        return tag + struct.pack('<I', len(payload)) + payload + (b'\0' if len(payload) % 2 else b'')
    chna = struct.pack('<HH', 2, 2) + b''.join(struct.pack('<H12s14s11sx', i, f'ATU_{i:08x}'.encode(), b'', b'') for i in [1, 2])
    fmt = struct.pack('<HHIIHH', 3, 2, 48000, 48000 * 8, 8, 32)
    body = b'WAVE' + chunk(b'fmt ', fmt) + chunk(b'axml', xml) + chunk(b'chna', chna) + chunk(b'data', b'\0' * (48000 * 8))
    path.write_bytes(b'RIFF' + struct.pack('<I', len(body)) + body)


def monitor_contracts(api, root, require_device):
    path = root / 'silent-adm.wav'
    silent_adm(path)
    lib, monitor, options = api.lib, PTR(), api.lib.adm_create_render_options()
    try:
        api.ok(lib.adm_render_options_set_renderer(options, 6))
        api.ok(lib.adm_render_options_set_output_layout(options, b'binaural'))
        result = lib.adm_create_monitor(api.context, str(path).encode(), options, C.byref(monitor))
        if result != 0:
            message = lib.adm_context_last_error_message(api.context).decode()
            assert not require_device and result in [2, 6], (result, message)
            return {'skipped': message}
        value = request(50)
        api.ok(lib.adm_monitor_set_hptf_stereo_parameters(monitor, C.byref(value)))
        del value
        gc.collect()
        api.ok(lib.adm_monitor_set_loop(monitor, 0, 1))
        api.ok(lib.adm_monitor_play(monitor))
        info = wait_revision(api, monitor, 50, True)
        assert (info.left.preamp, info.right.preamp) == (-3, -9)
        before = bytes(info)
        api.ok(lib.adm_monitor_pause(monitor))
        api.ok(lib.adm_monitor_seek(monitor, .2))
        assert bytes(api.info(monitor, True)) == before
        api.ok(lib.adm_render_options_set_renderer(options, 6))
        api.ok(lib.adm_render_options_set_output_layout(options, b'binaural'))
        api.ok(lib.adm_monitor_switch_backend(monitor, options))
        api.ok(lib.adm_monitor_set_output_device(monitor, b'hptf-test-missing-device'))
        api.ok(lib.adm_monitor_play(monitor))
        assert bytes(wait_revision(api, monitor, 50, True)) == before
        bad = request(51)
        bad.right_preamp = float('nan')
        assert lib.adm_monitor_set_hptf_stereo_parameters(monitor, C.byref(bad)) == 1
        assert bytes(api.info(monitor, True)) == before
        old = Info()
        assert lib.adm_monitor_get_hptf_info(monitor, C.byref(old)) == 2
        bypass = Files(revision=52)
        api.ok(lib.adm_monitor_set_hptf_stereo(monitor, C.byref(bypass)))
        info = wait_revision(api, monitor, 52, True)
        assert not info.left.enabled and not info.right.enabled
        api.ok(lib.adm_monitor_get_hptf_info(monitor, C.byref(old)))
        return {'passed': True, 'device_rebuild': True, 'backend_switch': True}
    finally:
        if monitor:
            lib.adm_destroy_monitor(monitor)
        lib.adm_destroy_render_options(options)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('library', type=Path)
    parser.add_argument('--require-device', action='store_true')
    args = parser.parse_args()
    assert [C.sizeof(t) for t in [Band, Parameters, Files, Info, StereoInfo]] == [40, 56, 32, 40, 88]
    api = HptfApi(args.library.resolve())
    try:
        with tempfile.TemporaryDirectory(prefix='mradm-hptf-stereo-') as directory:
            root = Path(directory)
            scene_contracts(api, root)
            monitor = monitor_contracts(api, root, args.require_device)
        print(json.dumps({'scene': 'passed', 'monitor': monitor, 'abi_minor': 50}, ensure_ascii=False))
    finally:
        api.lib.adm_destroy_context(api.context)


if __name__ == '__main__':
    main()
