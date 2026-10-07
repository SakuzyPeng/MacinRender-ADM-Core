"""Strict, portable evidence primitives for the Rust phase-2 baseline."""
import array
import hashlib
import importlib.util
import json
import math
from pathlib import Path
import re
import shlex
import struct
import sys

ROOT = Path(__file__).resolve().parents[2]
FORMATS = {'.f32': (4, 'f'), '.f64': (8, 'd'), '.c32': (4, 'f'), '.i32': (4, 'i')}
SOURCE_DIRS = ('src', 'include', 'cmake', 'rust', 'tests/tools', 'tests/support', 'scripts/consistency')
TEXT_SUFFIXES = {'.h', '.hpp', '.c', '.cpp', '.rs', '.toml', '.lock', '.json', '.py', '.sh', '.cmake', '.txt', '.xml', '.dat'}


def save(path, value):
    path.parent.mkdir(parents=True, exist_ok=True)
    path.write_text(json.dumps(value, ensure_ascii=False, sort_keys=True, indent=2, allow_nan=False) + '\n', encoding='utf-8')


def digest(data):
    return hashlib.sha256(data).hexdigest()


def json_digest(value):
    return digest(json.dumps(value, sort_keys=True, separators=(',', ':'), allow_nan=False).encode())


def source_files(root=ROOT):
    paths = [root / 'CMakeLists.txt', root / 'CMakePresets.json', root / 'third_party/manifest.json']
    for directory in SOURCE_DIRS:
        paths.extend(p for p in (root / directory).rglob('*') if p.is_file()
                     and not any(part in {'target', '__pycache__', '.git'} for part in p.relative_to(root).parts)
                     and (p.suffix in TEXT_SUFFIXES or p.suffix in {'.bin', '.sofa', '.f32le'}))
    return sorted(set(paths))


def source_fingerprint(root=ROOT):
    # Canonical text corresponds to Git's LF sources; binary assets remain exact.
    rows = {}
    for p in source_files(root):
        data = p.read_bytes()
        if p.suffix in TEXT_SUFFIXES:
            data = data.replace(b'\r\n', b'\n')
        rows[p.relative_to(root).as_posix()] = digest(data)
    return {'sha256': json_digest(rows), 'files': rows}


def load_module(name, path):
    spec = importlib.util.spec_from_file_location(name, path)
    module = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(module)
    return module


def offline_cases():
    source = (ROOT / 'scripts/consistency/render-matrix.sh').read_text(encoding='utf-8')
    match = re.search(r"cases=\$\(\s*cat <<'EOF'\n(.*?)\nEOF", source, re.S)
    if not match:
        raise ValueError('legacy matrix no longer has the recorded case block')
    rows = [(name, fixture, shlex.split(args)) for name, fixture, args in
            (line.split('|') for line in match.group(1).splitlines())]
    if len(rows) != 16:
        raise ValueError('review legacy matrix changes explicitly')
    extra = load_module('phase2_pcm_cases', ROOT / 'scripts/consistency/compare-pcm-mix.py').CASES
    extra = list(extra) + [
        ('scene-binaural-point', 'objects-point', ['--renderer', 'saf-binaural', '--output-layout', 'binaural']),
        ('scene-binaural-cloud', 'objects-extent-multi', ['--renderer', 'saf-binaural', '--output-layout', 'binaural', '--binaural-spread-mode', 'cloud']),
        ('scene-binaural-rotated', 'objects-extent', ['--renderer', 'saf-binaural', '--output-layout', 'binaural', '--binaural-spread-mode', 'cloud', '--listener-yaw', '37', '--listener-pitch', '23', '--listener-roll', '-19']),
        ('scene-hoa-extent', 'objects-extent-multi', ['--renderer', 'hoa', '--output-layout', 'hoa3']),
    ]
    rows += [(name, fixture, ['--no-peak-limit', *args]) for name, fixture, args in extra]
    rows += [
        ('ear-loudness-minus23', 'objects-point', ['--renderer', 'ear', '--output-layout', '5.1', '--no-peak-limit', '--loudness-target', '-23']),
        ('ear-peak-minus6', 'objects-point', ['--renderer', 'ear', '--output-layout', '5.1', '--peak-normalize-to-limit', '--peak-limit-dbtp', '-6']),
    ]
    result, seen = [], set()
    for name, fixture, args in rows:
        opts, i = {}, 0
        while i < len(args):
            key = args[i]
            if key in {'--no-peak-limit', '--peak-normalize-to-limit'}:
                opts[key] = True; i += 1
            else:
                opts[key] = args[i + 1]; i += 2
        key = (fixture, tuple(sorted(opts.items())))
        if key not in seen:
            seen.add(key)
            result.append({'id': name, 'fixture': fixture, 'args': args})
    return result


def scene_cases():
    result = []
    variants = [('vbap', '0+2+0', False), ('vbap', '4+7+0', False),
                ('binaural', 'binaural', False), ('binaural', 'binaural', True)]
    metadata = [
        {'sample': 37, 'field': 'gain', 'value': 0.625, 'ramp': 127},
        {'sample': 509, 'field': 'position', 'value': [-0.25, 0.75, 0.125], 'ramp': 1024},
        {'sample': 1001, 'field': 'gain', 'value': 0.875, 'ramp': 511},
        {'sample': 3073, 'field': 'head_locked', 'value': True, 'ramp': 0},
        {'sample': 5003, 'field': 'head_locked', 'value': False, 'ramp': 0},
    ]
    for backend, layout, cloud in variants:
        for input_rate, output_rate in [(48000, 48000), (48000, 44100), (44100, 48000)]:
            # Multichannel conversion is represented by the same VBAP kernel's stereo cases.
            if layout == '4+7+0' and input_rate != output_rate:
                continue
            for partition_name, partition in [('fixed', [512]), ('fragmented', [1, 7, 127, 511, 1024])]:
                result.append({
                    'id': f'scene-{backend}-{layout}-cloud{int(cloud)}-{input_rate}-{output_rate}-{partition_name}',
                    'version': 1, 'clock': 'epoch*2s + input_sample/input_rate; +1s at sample 10240', 'backend': backend, 'layout': layout, 'cloud': cloud,
                    'input_rate': input_rate, 'output_rate': output_rate, 'partition': partition,
                    'device_dsp': False, 'metadata': metadata, 'state_complete_on_every_frame': True,
                    'controls': {'2048': 'pose(37,23,-19)', '4096': 'generation=2',
                                 '6144': 'semantic gain scale=0.5', '8192': 'switch stereo backend',
                                 '10240': 'advance virtual clock by 1s; expire tracking'},
                    'epochs': [{'epoch': 1, 'target': 0, 'end': 12289}, {'epoch': 2, 'target': 257, 'end': 290}],
                    'signal': {'generator': 'lcg32-v1', 'seed': 0x12345678, 'scale': 0.125},
                })
    for partition_name, partition in [('fixed', [512]), ('fragmented', [1, 7, 127, 511, 1024])]:
        row = dict(result[0], id=f'scene-device-dsp-{partition_name}', backend='binaural', layout='binaural',
                   device_dsp=True, partition=partition, signal={'generator': 'lcg32-v1', 'seed': 0x12345678, 'scale': 4.0},
                   output_controls={'0': 'volume=0.875; HpTF bypass', '2048': 'HpTF peak 1700Hz +6dB Q0.75 preamp -3dB auto-trim',
                                    '4096': 'HpTF high-shelf 4200Hz -4dB Q0.75 preamp -6dB', '6144': 'HpTF bypass', '8192': 'volume=0.5'})
        result.append(row)
    return result


FFT_SIZES = (256, 512, 1024, 2048, 4096, 8192, 16384, 32768)


def kernel_outputs():
    """Measurement files written by the mr_adm_phase2_kernels probe, per pass."""
    names = ['fft-twiddles.10-libm.f64', 'fft-twiddles.20-table.f32']
    names += [f'fft-{n}.{part}' for n in FFT_SIZES for part in ('10-input.f32', '20-spectrum.c32', '30-inverse.f32')]
    names += ['scene.10-input.f32', 'scene.20-output.f32']
    names += [f'ear-{layout}.{part}' for layout in ('0+5+0', '4+7+0', '9+10+3') for part in ('10-layout.f64', '20-fir.f32')]
    names += [f'om-{i}.{part}' for i in range(4)
              for part in ('10-input.f32', '20-real.f32', '30-complex.c32', '40-residual.f32')]
    names += [f'resampler-{a}-{b}.{part}' for a, b in ((48000, 48000), (48000, 44100), (44100, 48000))
              for part in ('10-input.f32', '20-output.f32')]
    names += [f'hptf-{rate}.{part}' for rate in (44100, 48000) for part in ('10-input.f64', '20-coefficients.f32')]
    names += ['trig.10-input.f64', 'trig.20-sin.f64', 'trig.30-cos.f64']
    return sorted(names)


def pcm_bytes(path):
    data = Path(path).read_bytes()
    if len(data) < 24:
        raise ValueError(f'truncated PCM: {path}')
    magic, version, channels, rate, frames = struct.unpack('<4sIIIQ', data[:24])
    if magic != b'MRPB' or version != 1 or not 1 <= channels <= 256 or not rate or not frames or len(data) != 24 + channels * frames * 4:
        raise ValueError(f'invalid PCM header/shape: {path}')
    validate_words(data[24:], '.f32')
    return (channels, rate, frames), data[24:]


def validate_words(data, suffix):
    width, code = FORMATS[suffix]
    if not data or len(data) % width:
        raise ValueError('empty or truncated checkpoint')
    if suffix == '.c32' and len(data) % 8:
        raise ValueError('truncated complex checkpoint')
    values = array.array(code)
    values.frombytes(data)
    if sys.byteorder != 'little':
        values.byteswap()
    if code != 'i' and not all(map(math.isfinite, values)):
        raise ValueError('non-finite checkpoint')
    return values


def compare_words(a, b, suffix):
    av, bv = validate_words(a, suffix), validate_words(b, suffix)
    width, code = FORMATS[suffix]
    row = {'identical': a == b, 'sha256': [digest(a), digest(b)], 'words': [len(av), len(bv)]}
    if len(av) != len(bv):
        raise ValueError('checkpoint shape mismatch')
    if a == b:
        return row
    words_a, words_b = array.array('I' if width == 4 else 'Q'), array.array('I' if width == 4 else 'Q')
    words_a.frombytes(a); words_b.frombytes(b)
    if sys.byteorder != 'little':
        words_a.byteswap(); words_b.byteswap()
    sign = 1 << (width * 8 - 1)
    mask = sign * 2 - 1
    def ordered(v):
        return (~v & mask) if v & sign else v | sign
    differences = [i for i, (x, y) in enumerate(zip(words_a, words_b)) if x != y]
    first = differences[0]
    row.update(first_word=first, different_words=len(differences),
               first_bits=[f'0x{v[first]:0{width*2}x}' for v in (words_a, words_b)],
               first_values=[av[first], bv[first]], max_absolute_error=max(abs(av[i] - bv[i]) for i in differences))
    if code != 'i':
        row['max_ulp'] = max(abs(ordered(words_a[i]) - ordered(words_b[i])) for i in differences)
    return row


def compare_pcm(a, b):
    sa, a = pcm_bytes(a); sb, b = pcm_bytes(b)
    if sa != sb:
        raise ValueError('PCM shape mismatch')
    row = compare_words(a, b, '.f32')
    row['shape'] = sa
    if 'first_word' in row:
        row['first_frame'], row['first_channel'] = divmod(row['first_word'], sa[0])
    return row


def require_same(a, b):
    result = compare_pcm(a, b)
    if not result['identical']:
        raise ValueError(f'repeatability/noninterference failure: {a} vs {b}: {result}')
    return result


def build_configuration(build):
    # Local build identity, not a cross-platform equality key. A configure-only
    # change updates these files even when every executable still has its old hash.
    names = ('CMakeCache.txt', 'compile_commands.json', 'consistency-dependencies.json', 'rust-dependencies.json')
    return {name: digest((build / name).read_bytes()) for name in names}


def validate_build_stamp(build, source, binaries):
    stamp = json.loads((build / 'phase2-build-stamp.json').read_text(encoding='utf-8'))
    if (stamp.get('source') != source
            or stamp.get('binaries') != {name: digest(path.read_bytes()) for name, path in binaries.items()}
            or stamp.get('configuration') != build_configuration(build)):
        raise ValueError('stale source/binaries/configuration; build target mr_adm_phase2_tools first')


def stamp_build(build):
    """Called by CMake only after all measured executables were built."""
    names = ('mradm', 'mr_adm_make_fixture', 'mr_adm_pcm_bits', 'mr_adm_phase2_render',
             'mr_adm_phase2_scene', 'mr_adm_phase2_kernels', 'mr_adm_repeat_render')
    import os
    binaries = {name: digest((build / (name + ('.exe' if os.name == 'nt' else ''))).read_bytes()) for name in names}
    save(build / 'phase2-build-stamp.json', {'source': source_fingerprint(), 'binaries': binaries,
                                           'configuration': build_configuration(build)})


if __name__ == '__main__':
    import argparse
    parser = argparse.ArgumentParser()
    parser.add_argument('--stamp-build', required=True, type=Path)
    stamp_build(parser.parse_args().stamp_build)


def canonicalize_device_trace(root, spec):
    """Device callbacks are transport segments, not numeric shape boundaries.

    Reassemble each measured epoch/stage by its media offset. Gaps, overlaps,
    truncated frames and wrong total duration are errors, never zero-filled.
    """
    from collections import defaultdict
    if not spec['device_dsp']:
        return
    groups = defaultdict(list)
    for path in (root / 'device').glob('*.f32'):
        match = re.fullmatch(r'e(\d+)-s(\d+)\.(\d+-.+)\.f32', path.name)
        if match is None:
            raise ValueError('unexpected device checkpoint name')
        groups[(int(match[1]), match[3])].append((int(match[2]), path))
    stages = ('70-before-hptf', '80-hptf', '90-output')
    expected = {(epoch['epoch'], stage): ((epoch['end'] - epoch['target']) * spec['output_rate'] + spec['input_rate'] - 1) // spec['input_rate']
                for epoch in spec['epochs'] for stage in stages}
    if groups.keys() != expected.keys():
        raise ValueError('missing device checkpoint stage or epoch')
    assembled = {}
    for key, segments in groups.items():
        cursor, pieces = 0, []
        for start, path in sorted(segments):
            data = path.read_bytes()
            validate_words(data, '.f32')
            if len(data) % 8 or start != cursor:
                raise ValueError('device checkpoint gap, overlap or incomplete stereo frame')
            cursor += len(data) // 8
            pieces.append(data)
        if cursor != expected[key]:
            raise ValueError('device checkpoint duration mismatch')
        assembled[key] = b''.join(pieces)
    for (epoch, stage), data in assembled.items():
        (root / 'device' / f'e{epoch}.{stage}.f32').write_bytes(data)
    for segments in groups.values():
        for _, path in segments:
            path.unlink()
