#!/usr/bin/env python3
"""Independent integer PCM/FLAC fixtures for the strict reader and PCM tool."""
import argparse
import hashlib
import json
from pathlib import Path
import struct
import subprocess
import tempfile


def crc(data, width, polynomial):
    value = 0
    mask = (1 << width) - 1
    for byte in data:
        value ^= byte << (width - 8)
        for _ in range(8):
            value = ((value << 1) ^ (polynomial if value & (1 << (width - 1)) else 0)) & mask
    return value


class Bits:
    def __init__(self):
        self.data, self.value, self.count = bytearray(), 0, 0

    def put(self, value, count):
        self.value = (self.value << count) | (value & ((1 << count) - 1))
        self.count += count
        while self.count >= 8:
            self.count -= 8
            self.data.append((self.value >> self.count) & 255)
        self.value &= (1 << self.count) - 1

    def finish(self):
        if self.count:
            self.put(0, 8 - self.count)
        return bytes(self.data)


def fixture(bits=24, channels=2, frames=2053, rate=48000):
    half = 1 << (bits - 1)
    values = [(-half, half - 1, 0, -1, 1, half // 3, -half // 3)[i % 7]
              for i in range(frames * channels)]
    md5_pcm = b''.join(value.to_bytes((bits + 7) // 8, 'little', signed=True) for value in values)
    packed = (rate << 44) | ((channels - 1) << 41) | ((bits - 1) << 36) | frames
    streaminfo = struct.pack('>HH', 1024, 1024) + bytes(6) + packed.to_bytes(8, 'big') + hashlib.md5(md5_pcm).digest()
    header = b'fLaC\x80\0\0\x22' + streaminfo
    blocks = []
    for number, start in enumerate(range(0, frames, 1024)):
        count = min(1024, frames - start)
        # Fixed-block stream, explicit 16-bit block length, format from STREAMINFO.
        frame_header = bytes([255, 248, 0x70, (channels - 1) << 4, number]) + (count - 1).to_bytes(2, 'big')
        frame_header += bytes([crc(frame_header, 8, 7)])
        payload = Bits()
        for channel in range(channels):
            payload.put(2, 8)  # Verbatim subframe; no wasted bits.
            for frame in range(start, start + count):
                payload.put(values[frame * channels + channel], bits)
        block = frame_header + payload.finish()
        blocks.append(block + crc(block, 16, 0x8005).to_bytes(2, 'big'))
    expected = b''.join(struct.pack('<f', value / half) for value in values)
    return header, blocks, expected


def unknown(data, no_md5=False):
    result = bytearray(data)
    result[18:26] = (int.from_bytes(result[18:26], 'big') & ~((1 << 36) - 1)).to_bytes(8, 'big')
    if no_md5:
        result[26:42] = bytes(16)
    return bytes(result)


def ogg_page(body, sequence, flags, granule):
    lacing = bytes([255] * (len(body) // 255) + [len(body) % 255])
    page = bytearray(b'OggS\0' + bytes([flags]) + struct.pack('<QII', granule, 42, sequence) + bytes(4))
    page += bytes([len(lacing)]) + lacing + body
    page[22:26] = crc(page, 32, 0x04c11db7).to_bytes(4, 'little')
    return bytes(page)


def ogg(header, blocks, frames=2053):
    streaminfo = bytearray(header)
    streaminfo[4] = 0
    pages = [ogg_page(b'\x7fFLAC\x01\x00\x00\x01' + streaminfo, 0, 2, 0),
             ogg_page(b'\x84\0\0\x08' + bytes(8), 1, 0, 0)]
    for i, block in enumerate(blocks):
        final = i == len(blocks) - 1
        pages.append(ogg_page(block, i + 2, 4 if final else 0, frames if final else (i + 1) * 1024))
    return pages


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--pcm-tool', required=True, type=Path)
    parser.add_argument('--report', type=Path)
    args = parser.parse_args()
    tool = args.pcm_tool.resolve()
    results = []
    with tempfile.TemporaryDirectory(prefix='mradm-flac-') as directory:
        work = Path(directory)

        def check(name, data, expected=None, channels=2, frames=2053, rate=48000):
            source, output = work / (name + '.flac'), work / 'output.pcmbits'
            source.write_bytes(data)
            output.write_bytes(b'unchanged')
            result = subprocess.run([str(tool), 'extract', str(source), str(output)], capture_output=True, timeout=30)
            actual = output.read_bytes()
            if expected is None:
                ok = result.returncode == 2 and actual == b'unchanged' and b'error:' in result.stderr
            else:
                canonical = b'MRPB' + struct.pack('<IIIQ', 1, channels, rate, frames) + expected
                ok = result.returncode == 0 and actual == canonical
            results.append({'id': name, 'passed': ok, 'exit': result.returncode,
                            'stderr': result.stderr.decode('utf-8', errors='replace'),
                            'input_sha256': hashlib.sha256(data).hexdigest()})
            if not ok:
                raise AssertionError(json.dumps(results[-1], ensure_ascii=False))

        for bits in (4, 8, 12, 16, 20, 24, 32):
            for channels in (1, 2, 6, 8):
                header, blocks, expected = fixture(bits, channels)
                check(f'pcm{bits}-{channels}ch', header + b''.join(blocks), expected, channels)
        for channels in (3, 4, 5, 7):
            header, blocks, expected = fixture(channels=channels, rate=96000)
            check(f'pcm24-{channels}ch-96k', header + b''.join(blocks), expected, channels, rate=96000)
        header, blocks, expected = fixture()
        data = header + b''.join(blocks)
        check('unknown-length', unknown(data), expected)
        check('unknown-length-no-md5', unknown(data, True), expected)
        check('empty', fixture(frames=0)[0], b'', frames=0)
        check('complete-short-prefix', unknown(header + blocks[0], True), expected[:1024 * 8], frames=1024)
        check('中文 空格', data, expected)
        pages = ogg(header, blocks)
        check('ogg', b''.join(pages), expected)
        check('ogg-unknown', b''.join(ogg(unknown(header, True), blocks)), expected)
        for cut in (0, 1, 4, 20, 41, 42, 43, len(data) - 1, len(header) + len(blocks[0]) + 12):
            check(f'truncated-{cut}', data[:cut])
        for i, block in enumerate(blocks):
            damaged = bytearray(block)
            damaged[-1] ^= 1
            broken = blocks[:i] + [bytes(damaged)] + blocks[i + 1:]
            check(f'crc-{i}', header + b''.join(broken))
            check(f'crc-unknown-{i}', unknown(header, True) + b''.join(broken))
        check('missing-frame', unknown(header, True) + blocks[0] + blocks[2])
        check('repeated-frame', unknown(header, True) + blocks[0] + blocks[0] + b''.join(blocks[1:]))
        check('unknown-truncated', unknown(data[:-1], True))
        check('unknown-partial-frame', unknown(header + blocks[0] + blocks[1][:12], True))
        check('unknown-partial-header', unknown(header + blocks[0] + b'\xff', True))
        check('trailing-garbage', data + b'garbage')
        wrong_md5 = bytearray(data)
        wrong_md5[26] ^= 1
        check('wrong-md5', wrong_md5)
        for total in (1, 2052, 2054, (1 << 36) - 1):
            wrong = bytearray(unknown(data))
            wrong[18:26] = (int.from_bytes(wrong[18:26], 'big') | total).to_bytes(8, 'big')
            check(f'wrong-length-{total}', wrong)
        check('ogg-missing-page', b''.join(pages[:3] + pages[4:]))
        check('ogg-missing-end', b''.join(pages[:-1]))
        check('ogg-truncated', b''.join(pages)[:-1])
        check('ogg-trailing', b''.join(pages) + b'garbage')
        damaged = bytearray(pages[-1])
        damaged[-1] ^= 1
        check('ogg-crc', b''.join(pages[:-1]) + damaged)
        check('ogg-wrong-length', b''.join(pages[:-1]) + ogg_page(blocks[-1], len(pages) - 1, 4, 2054))
    report = {'cases': results, 'passed': len(results)}
    if args.report:
        args.report.parent.mkdir(parents=True, exist_ok=True)
        args.report.write_text(json.dumps(report, ensure_ascii=False, indent=2) + '\n', encoding='utf-8')
    print(f"FLAC reader: {len(results)} cases passed")


if __name__ == '__main__':
    main()
