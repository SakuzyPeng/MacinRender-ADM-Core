#!/usr/bin/env python3
"""Reproducible, opt-in FLAC decoder selection experiment; no production build changes."""
import argparse
import hashlib
import json
import os
from pathlib import Path
import platform
import re
import struct
import subprocess
import tempfile

HERE = Path(__file__).resolve().parent
ROOT = HERE.parents[1]
FRAMES = 8193


def run(args, **kwargs):
    proc = subprocess.run([str(a) for a in args], capture_output=True, text=True, timeout=180, **kwargs)
    if proc.returncode != 0:
        raise RuntimeError(f'Command failed: {args}\n{proc.stdout}\n{proc.stderr}')
    return proc.stdout


def sha(data):
    return hashlib.sha256(data).hexdigest()


def samples(bits, channels):
    """Integer-only fixture oracle: extrema, noise, constant, ramp, correlated stereo, wasted bits."""
    values = []
    half = 1 << (bits - 1)
    state = 0x12345678
    for frame in range(FRAMES):
        for channel in range(channels):
            state = (1664525 * state + 1013904223) & 0xffffffff
            block = frame // 1024
            if frame < 16:
                value = [-half, half - 1, 0, -1, 1, half // 2, -half // 2][(frame + channel) % 7]
            elif block == 1:
                value = half // 3 - channel
            elif block == 2:
                value = ((frame * (channel + 1) % 127) - 63) * max(1, half // 1024)
            elif block == 3:
                value = ((frame % 257) - 128) * max(1, half // 512)
            elif block == 4:
                value = (-half if (frame + channel) % 2 else half - 1)
            elif block == 6:
                value = -1
            else:
                value = (state >> (32 - bits)) - half
            values.append(max(-half, min(half - 1, value)))
    return values


def oracle(values, bits):
    return b''.join(struct.pack('<if', value * (1 << (32 - bits)), value / (1 << (bits - 1)))
                    for value in values)


def metadata_blocks(data):
    assert data[:4] == b'fLaC'
    offset = 4
    blocks = []
    while True:
        kind = data[offset]
        size = int.from_bytes(data[offset + 1:offset + 4], 'big')
        blocks.append((kind & 127, data[offset + 4:offset + 4 + size]))
        offset += 4 + size
        if kind & 128:
            return blocks, data[offset:]


def with_metadata(data):
    blocks, audio = metadata_blocks(data)
    comments = ['TITLE=FLAC 迁移校验', 'WAVEFORMATEXTENSIBLE_CHANNEL_MASK=0x3',
                'UNRECOGNIZED_FIELD=preserve-me']
    vendor = b'mradm-flac-eval'
    vc = struct.pack('<I', len(vendor)) + vendor + struct.pack('<I', len(comments))
    for comment in comments:
        raw = comment.encode()
        vc += struct.pack('<I', len(raw)) + raw
    # APPLICATION, Vorbis comment, padding, and an unknown/reserved metadata type.
    blocks = [(kind, payload) for kind, payload in blocks if kind != 4]
    blocks += [(2, b'MRADopaque-application'), (4, vc), (1, bytes(1024 * 1024)), (10, b'opaque')]
    return b'fLaC' + b''.join(bytes([kind | (128 if i == len(blocks) - 1 else 0)]) +
                              len(payload).to_bytes(3, 'big') + payload
                              for i, (kind, payload) in enumerate(blocks)) + audio


def clear_length(data, md5=False):
    data = bytearray(data)
    data[18:26] = (int.from_bytes(data[18:26], 'big') & ~((1 << 36) - 1)).to_bytes(8, 'big')
    if md5:
        data[26:42] = bytes(16)
    return bytes(data)


def inspect_result(command, output, expected):
    output.unlink(missing_ok=True)
    try:
        proc = subprocess.run([str(a) for a in command], capture_output=True, text=True, timeout=20)
        result = dict(line.split('=', 1) for line in proc.stdout.splitlines() if '=' in line)
        result['exit_code'] = proc.returncode
        if proc.stderr:
            result['stderr'] = proc.stderr[:1500]
    except subprocess.TimeoutExpired:
        result = {'status': 'timeout'}
    data = output.read_bytes() if output.exists() else b''
    result['output_sha256'] = sha(data)
    result['output_bytes'] = len(data)
    result['pcm_exact'] = data == expected
    if len(data) % 8 == 0:
        result['integer_sha256'] = sha(b''.join(data[i:i + 4] for i in range(0, len(data), 8)))
        result['float_sha256'] = sha(b''.join(data[i + 4:i + 8] for i in range(0, len(data), 8)))
    if data != expected:
        difference = next(
            (i // 8 for i in range(0, min(len(data), len(expected)), 8)
             if data[i:i + 8] != expected[i:i + 8]), min(len(data), len(expected)) // 8)
        result['first_different_sample'] = difference
        if difference * 8 + 8 <= min(len(data), len(expected)):
            result['first_difference'] = {
                'expected_i32_f32bits': struct.unpack_from('<iI', expected, difference * 8),
                'actual_i32_f32bits': struct.unpack_from('<iI', data, difference * 8)}
    return result


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--build-dir', type=Path, default=ROOT / 'build/release')
    parser.add_argument('--dr-libs', type=Path, default=ROOT / '.fc-cache/dr_libs-src')
    parser.add_argument('--flac-source', type=Path, default=ROOT / '.fc-cache/flac-src')
    parser.add_argument('--cxx', default=os.environ.get('CXX', 'clang++'))
    parser.add_argument('--flac', default='flac', help='FLAC CLI, used only to validate/author test fixtures')
    parser.add_argument('--output', type=Path, default=ROOT / 'local/flac-eval/selection.json')
    args = parser.parse_args()
    args.output.parent.mkdir(parents=True, exist_ok=True)
    cache_hash = hashlib.sha1(str((ROOT / 'rust/Cargo.toml').resolve()).encode()).hexdigest()[:5]
    target_dir = ROOT / 'build/rust/cargo' / ('rust_' + cache_hash)
    rust_version = run(['rustc', '+1.98.0', '-vV'])
    target = next(line[6:] for line in rust_version.splitlines() if line.startswith('host: '))
    env = dict(os.environ, CARGO_INCREMENTAL='0', RUSTFLAGS='-Crelocation-model=pic')
    run(['cargo', '+1.98.0', 'build', '--locked', '--release', '--manifest-path', HERE / 'Cargo.toml',
         '--target-dir', target_dir, '--target', target], env=env)
    rust_probe = target_dir / target / 'release/mradm-flac-eval'
    # Reuse the configured release library, without reconfiguring CMake or creating another cache.
    library = args.build_dir / '_deps/flac-build/src/libFLAC/libFLAC.a'
    if not library.is_file():
        raise RuntimeError(f'Build vendored Release FLAC first; missing {library}')
    report = {'schema_version': 1, 'stage': 'selection-only', 'platform': platform.platform(),
              'source_revision': run(['git', '-C', ROOT, 'rev-parse', 'HEAD']).strip(),
              'rustc': rust_version, 'cxx': run([args.cxx, '--version']).splitlines()[0],
              'flac_cli': run([args.flac, '--version']).strip(),
              'profiles': {'rust': 'release', 'cpp': '-O2'},
              'versions': {'symphonia': '0.6.1', 'claxon': '0.4.3'},
              'dr_flac_header_sha256': sha((args.dr_libs / 'dr_flac.h').read_bytes()),
              'probe_sources': {str(p.relative_to(ROOT)): sha(p.read_bytes()) for p in
                                [HERE / 'run.py', HERE / 'reference.cpp', HERE / 'src/main.rs',
                                 HERE / 'Cargo.toml', HERE / 'Cargo.lock']},
              'cases': [], 'seeks': []}
    with tempfile.TemporaryDirectory(prefix='fixtures-', dir=args.output.parent) as temp:
        work = Path(temp)
        cpp_probe = work / 'flac-reference'
        run([args.cxx, '-std=c++20', '-O2', '-I', args.dr_libs, '-I', args.flac_source / 'include',
             HERE / 'reference.cpp', library, '-o', cpp_probe])
        fixtures = []
        settings = [(bits, ch) for bits in (4, 8, 12, 16, 20, 24, 32) for ch in (1, 2, 6, 8)]
        settings += [(24, ch) for ch in (3, 4, 5, 7)]
        for bits, channels in settings:
            name = f'pcm{bits}-{channels}ch'
            path = work / (name + '.flac')
            pcm = samples(bits, channels)
            raw = work / 'input.s32'
            raw.write_bytes(struct.pack('<' + 'i' * len(pcm), *pcm))
            rate = 48000 if (bits, channels) == (24, 2) else (8000, 44100, 96000, 192000)[channels % 4]
            run([cpp_probe, 'encode', raw, path, bits, channels, rate])
            run([args.flac, '-t', '-s', path])
            fixtures.append((name, path, oracle(pcm, bits), channels, 'valid'))
        base = next(f for f in fixtures if f[0] == 'pcm24-2ch')
        original = base[1].read_bytes()

        def add(name, data, category='valid', expected=base[2]):
            path = work / (name + '.flac')
            path.write_bytes(data)
            if category == 'valid':
                run([args.flac, '-t', '-s', *(['--ogg'] if data[:4] == b'OggS' else []), path])
            fixtures.append((name, path, expected, 2, category))

        add('unknown-total', clear_length(original))
        add('unknown-total-no-md5', clear_length(original, md5=True))
        add('metadata-unicode-padding-application-unknown', with_metadata(original))
        ogg = work / 'ogg.flac'
        run([args.flac, '-s', '--ogg', '--serial-number=1', '--no-padding', '-f', '-o', ogg, base[1]])
        add('ogg-flac', ogg.read_bytes())
        indexed = work / 'indexed.flac'
        run([args.flac, '-s', '--no-padding', '-S', '1024', '-S', '4096', '-f', '-o', indexed, base[1]])
        add('seektable', indexed.read_bytes())

        analysis = work / 'frames.txt'
        run([args.flac, '-s', '-a', '-f', '-o', analysis, base[1]])
        offsets = [int(s) for s in re.findall(r'frame=\d+\s+offset=(\d+)', analysis.read_text())]
        if len(offsets) < 5:
            raise RuntimeError('FLAC analysis did not provide expected frame boundaries')
        add('truncated-header', original[:20], 'damaged')
        add('truncated-last-byte', original[:-1], 'damaged')
        add('truncated-mid-frame', original[:offsets[4] + 12], 'damaged')
        for name, pos in [('crc-first', offsets[1] - 1), ('crc-middle', offsets[4] - 1),
                          ('crc-last', len(original) - 1), ('wrong-md5', 26)]:
            mutated = bytearray(original)
            mutated[pos] ^= 1
            add(name, bytes(mutated), 'damaged')
        add('missing-frame-no-total-no-md5', clear_length(original[:offsets[3]] + original[offsets[4]:], True),
            'damaged')
        add('truncated-packet-no-total-no-md5', clear_length(original[:offsets[4] + 12], True), 'damaged')
        # An intact shorter stream without declared length/checksum cannot prove a missing tail.
        add('prefix-no-total-no-md5', clear_length(original[:offsets[4]], True), 'ambiguous-prefix',
            base[2][:4096 * 2 * 8])
        add('not-flac', b'not a FLAC file\n', 'damaged')
        # Probe prefix handling separately from the existing product's supported format contract.
        add('id3v2-prefix', b'ID3\x04\0\0\0\0\0\0' + original)
        pcm_out = work / 'decoded.bin'
        for name, path, expected, channels, category in fixtures:
            results = {}
            for decoder in ('drflac', 'symphonia', 'claxon'):
                binary = cpp_probe if decoder == 'drflac' else rust_probe
                results[decoder] = inspect_result([binary, decoder, path, pcm_out], pcm_out, expected)
            report['cases'].append({'id': name, 'category': category, 'channels': channels,
                                    'input_sha256': sha(path.read_bytes()), 'expected_sha256': sha(expected),
                                    'decoders': results})
            print(name + ': ' + ', '.join(f'{k}={v.get("status")} exact={v["pcm_exact"]}'
                                         for k, v in results.items()), flush=True)
        for name in ('pcm24-2ch', 'pcm32-6ch', 'unknown-total', 'seektable', 'ogg-flac'):
            _, path, expected, channels, _ = next(f for f in fixtures if f[0] == name)
            for target_frame in (0, 1, 1023, 1024, 1234, 8192, 8193, 10000):
                wanted = expected[min(target_frame, FRAMES) * channels * 8:]
                results = {}
                for decoder in ('drflac', 'symphonia'):
                    binary = cpp_probe if decoder == 'drflac' else rust_probe
                    results[decoder] = inspect_result([binary, decoder, path, pcm_out, target_frame], pcm_out, wanted)
                report['seeks'].append({'id': name, 'target_frame': target_frame, 'decoders': results})
    valid = [case for case in report['cases'] if case['category'] == 'valid']
    report['summary'] = {'valid_cases': len(valid), 'all_cases': len(report['cases']),
                         'seek_cases': len(report['seeks']), 'valid_pcm_exact': {
                             decoder: sum(c['decoders'][decoder]['pcm_exact'] for c in valid)
                             for decoder in ('drflac', 'symphonia', 'claxon')}}
    args.output.write_text(json.dumps(report, ensure_ascii=False, indent=2) + '\n')
    print(json.dumps(report['summary'], indent=2))
    print(f'Report: {args.output}')


if __name__ == '__main__':
    main()
