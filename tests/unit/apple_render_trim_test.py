#!/usr/bin/env python3
"""Apple offline windows beyond frame 2^31 through the public C ABI.

Restores the Apple cases from render_trim_fixture_test.cpp before R3 C4. The
six-object PCM24 BW64 input has a sparse data chunk, so its 38 GB logical size
uses only a few filesystem blocks. Both speaker layouts must produce exactly
the requested 1024 frames.
"""

import argparse
import ctypes as C
from pathlib import Path
import struct
import tempfile
import xml.etree.ElementTree as ET

from room_96k_test import wav_read


RATE = 48000
CHANNELS = 6
FRAMES = (1 << 31) - 1 + 2 * RATE
WINDOW = 1024


def chunk(tag, payload):
    return tag + struct.pack('<I', len(payload)) + payload + b'\0' * (len(payload) % 2)


def sparse_input(path):
    root = ET.Element('ebuCoreMain')
    metadata = ET.SubElement(ET.SubElement(ET.SubElement(root, 'coreMetadata'), 'format'),
                             'audioFormatExtended')
    content = ET.SubElement(metadata, 'audioContent', audioContentID='ACO_1001', audioContentName='C')
    programme = ET.SubElement(metadata, 'audioProgramme', audioProgrammeID='APR_1001', audioProgrammeName='P')
    ET.SubElement(programme, 'audioContentIDRef').text = 'ACO_1001'
    audio_ids = []
    for index in range(1, CHANNELS + 1):
        code = f'0003{0x1000 + index:04x}'
        channel_id, pack_id = f'AC_{code}', f'AP_{code}'
        stream_id, track_id = f'AS_{code}', f'AT_{code}_01'
        uid, object_id = f'ATU_{index:08x}', f'AO_{0x1000 + index:04x}'
        channel = ET.SubElement(metadata, 'audioChannelFormat', audioChannelFormatID=channel_id,
                                audioChannelFormatName=f'CF{index}', typeLabel='0003')
        block = ET.SubElement(channel, 'audioBlockFormat', audioBlockFormatID=f'AB_{code}_00000001')
        ET.SubElement(block, 'position', coordinate='azimuth').text = str(-60 + (index - 1) * 24)
        ET.SubElement(block, 'position', coordinate='elevation').text = '0'
        ET.SubElement(block, 'gain').text = '0.5'
        pack = ET.SubElement(metadata, 'audioPackFormat', audioPackFormatID=pack_id,
                             audioPackFormatName=f'PF{index}', typeLabel='0003')
        ET.SubElement(pack, 'audioChannelFormatIDRef').text = channel_id
        stream = ET.SubElement(metadata, 'audioStreamFormat', audioStreamFormatID=stream_id,
                               audioStreamFormatName=f'SF{index}', formatLabel='0001')
        ET.SubElement(stream, 'audioChannelFormatIDRef').text = channel_id
        ET.SubElement(stream, 'audioTrackFormatIDRef').text = track_id
        track = ET.SubElement(metadata, 'audioTrackFormat', audioTrackFormatID=track_id,
                              audioTrackFormatName=f'TF{index}', formatLabel='0001')
        ET.SubElement(track, 'audioStreamFormatIDRef').text = stream_id
        track_uid = ET.SubElement(metadata, 'audioTrackUID', UID=uid)
        ET.SubElement(track_uid, 'audioTrackFormatIDRef').text = track_id
        ET.SubElement(track_uid, 'audioPackFormatIDRef').text = pack_id
        obj = ET.SubElement(metadata, 'audioObject', audioObjectID=object_id,
                            audioObjectName=f'Object{index}')
        ET.SubElement(obj, 'audioTrackUIDRef').text = uid
        ET.SubElement(content, 'audioObjectIDRef').text = object_id
        audio_ids.append(struct.pack('<H12s14s11sx', index, uid.encode(), track_id.encode(), pack_id.encode()))

    frame_bytes = CHANNELS * 3
    fmt = struct.pack('<HHIIHH', 1, CHANNELS, RATE, RATE * frame_bytes, frame_bytes, 24)
    chna = struct.pack('<HH', CHANNELS, CHANNELS) + b''.join(audio_ids)
    metadata_chunks = (chunk(b'fmt ', fmt) + chunk(b'axml', ET.tostring(root, encoding='utf-8'))
                       + chunk(b'chna', chna))
    data_bytes = FRAMES * frame_bytes
    header_size = 12 + 36 + len(metadata_chunks) + 8
    ds64 = struct.pack('<QQQI', header_size + data_bytes - 8, data_bytes, FRAMES, 0)
    header = (b'BW64\xff\xff\xff\xffWAVE' + chunk(b'ds64', ds64) + metadata_chunks
              + b'data\xff\xff\xff\xff')
    assert len(header) == header_size
    with path.open('wb') as output:
        output.write(header)
        output.truncate(header_size + data_bytes)


def bind_api(library):
    lib = C.CDLL(str(library))
    pointer, text, integer = C.c_void_p, C.c_char_p, C.c_int
    signatures = {
        'adm_create_context': (pointer, []),
        'adm_destroy_context': (None, [pointer]),
        'adm_create_render_options': (pointer, []),
        'adm_destroy_render_options': (None, [pointer]),
        'adm_render_options_set_renderer': (integer, [pointer, integer]),
        'adm_render_options_set_output_layout': (integer, [pointer, text]),
        'adm_render_options_set_output_bit_depth': (integer, [pointer, integer]),
        'adm_render_options_set_peak_limit': (None, [pointer, integer]),
        'adm_render_options_set_render_start_sec': (integer, [pointer, C.c_double]),
        'adm_render_options_set_render_end_sec': (integer, [pointer, C.c_double]),
        'adm_render_file_ex': (integer, [pointer, text, text, pointer, pointer, pointer, C.POINTER(pointer)]),
        'adm_render_result_message': (text, [pointer]),
        'adm_destroy_render_result': (None, [pointer]),
    }
    for name, (result, args) in signatures.items():
        function = getattr(lib, name)
        function.restype = result
        function.argtypes = args
    return lib


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('library', type=Path)
    args = parser.parse_args()
    lib = bind_api(args.library.resolve())
    context = lib.adm_create_context()
    options = lib.adm_create_render_options()
    try:
        assert context and options
        start = FRAMES - 2 * WINDOW
        assert start > (1 << 31) - 1
        assert lib.adm_render_options_set_renderer(options, 4) == 0  # ADM_RENDERER_APPLE
        assert lib.adm_render_options_set_output_bit_depth(options, 0) == 0  # ADM_BIT_DEPTH_F32
        lib.adm_render_options_set_peak_limit(options, 0)
        assert lib.adm_render_options_set_render_start_sec(options, start / RATE) == 0
        assert lib.adm_render_options_set_render_end_sec(options, (start + WINDOW) / RATE) == 0
        with tempfile.TemporaryDirectory(prefix='mradm-apple-large-window-') as temporary:
            root = Path(temporary)
            source = root / 'sparse.wav'
            sparse_input(source)
            for layout, channels in [('4+7+0', 12), ('9+10+3', 24)]:
                assert lib.adm_render_options_set_output_layout(options, layout.encode()) == 0
                output = root / f'{layout}.wav'
                result = C.c_void_p()
                try:
                    code = lib.adm_render_file_ex(context, str(source).encode(), str(output).encode(),
                                                  options, None, None, C.byref(result))
                    assert code == 0 and result, (layout, code, lib.adm_render_result_message(result))
                    rate, actual_channels, pcm = wav_read(output)
                    assert (rate, actual_channels, len(pcm)) == (RATE, channels, WINDOW * channels * 4), layout
                finally:
                    lib.adm_destroy_render_result(result)
                print(f'Apple {layout} window beyond frame 2^31 passed ({channels} channels, {WINDOW} frames)')
    finally:
        lib.adm_destroy_render_options(options)
        lib.adm_destroy_context(context)


if __name__ == '__main__':
    main()
