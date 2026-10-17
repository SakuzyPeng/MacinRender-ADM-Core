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
SOURCE_DIRS = ('src', 'include', 'cmake', 'rust', 'tests/tools', 'tests/support', 'tests/fixtures/sofa',
               'tests/fixtures/room/phase2', 'scripts/consistency')
BINARY_SUFFIXES = {'.bin', '.sofa', '.f32le', '.f32', '.f64', '.u32'}
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
                     and (p.suffix in TEXT_SUFFIXES or p.suffix in BINARY_SUFFIXES))
    return sorted(set(paths))


def source_fingerprint(root=ROOT):
    # Canonical text corresponds to Git's LF sources; binary assets remain exact.
    rows = {}
    for p in source_files(root):
        data = p.read_bytes()
        # The room pack's manifests are pinned bytes (.gitattributes -text), like its assets.
        if p.suffix in TEXT_SUFFIXES and not p.is_relative_to(root / ROOM_FIXTURE_DIR):
            data = data.replace(b'\r\n', b'\n')
        rows[p.relative_to(root).as_posix()] = digest(data)
    return {'sha256': json_digest(rows), 'files': rows}


def load_module(name, path):
    spec = importlib.util.spec_from_file_location(name, path)
    module = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(module)
    return module


# Committed binary inputs, never generated on CI or replaced by a local dataset.
SOFA_FIXTURES = {
    'simple.sofa': '03e3a0ed655fb39d3e0af0e829b7f48e39b0f27477cb326949a5771b7815bf2d',
    'general-compressed.sofa': 'ccfe5aac2be59406a462d41056024ac14068645b62e2eed1d4e7e39a833c6a82',
    'off-axis-compressed.sofa': '482486e39774d1f64a0ca8121f839c0afb78e2784174a59616f4b52e14335b58',
}


def validate_sofa_fixtures(directory):
    for name, expected in SOFA_FIXTURES.items():
        path = directory / name
        if not path.is_file() or digest(path.read_bytes()) != expected:
            raise ValueError('missing or modified pinned SOFA fixture: ' + name)


# JSON inputs of PCM consistency 4.2 (semantic policies, a DirectSpeakers matrix and a screen
# configuration), written by the collector into fixtures/ with save() and hashed with the other fixtures. IDs are those make_fixture assigns:
# policy-objects holds AO_1001 "Dialogue Lead", AO_1002 "Dialogue Alt" (content "Dialogue"), AO_1003
# "Ambience Wide" and AO_1004 "FX Cartesian" (content "Ambience"), tracks ATU_00000001-4, programme
# "Main Mix". Every object rule matches, so no case renders with an unmatched-rule warning.
POLICY_SCHEMA = 'mradm.semantic-policy.v1'
JSON_FIXTURES = {
    # Identity matching and merge order: global first, then each matching rule in array order with
    # last-write-wins leaves (FX is muted by name, unmuted again by track UID).
    'policy-identity.json': {
        'schema': POLICY_SCHEMA,
        'global': {'gain': {'scale': 0.875}, 'extent': {'scale': 0.75}},
        'objects': [
            {'id': 'ao_1001', 'gain': {'gain_db': -2.5}},
            {'name_glob': 'dialogue*', 'diffuse': {'scale': 0.5}, 'position': {'offset': {'azimuth': 7.5}}},
            {'content': 'Ambience', 'extent': {'width_scale': 0.5, 'max': 30}},
            {'importance_min': 4, 'importance_max': 6, 'gain': {'scale': 0.5}},
            {'dialogue_id': 1, 'divergence': {'scale': 0.5}},
            {'programme': 'main mix', 'head_locked': False},
            {'name_glob': 'FX ?artesian', 'gain': {'mute': True}},
            {'track_uid': 'ATU_00000004', 'gain': {'mute': False, 'gain_db': 1.25}},
        ],
    },
    # channelLock (tighter distance, enabling on unlocked objects), divergence scale and range clamp,
    # and the interpolation limit with jumpPosition honoured.
    'policy-lock.json': {
        'schema': POLICY_SCHEMA,
        'global': {'interpolation': {'max_ms': 20}},
        'objects': [
            {'id': 'AO_1001', 'channel_lock': {'max_distance': 0.25},
             'interpolation': {'honor_jump_position': True, 'max_ms': 30}},
            {'name': 'Dialogue Alt', 'divergence': {'scale': 1.5, 'range_scale': 2, 'max_range': 45},
             'channel_lock': {'enabled': True, 'max_distance': 1.0}},
            {'importance_max': 3, 'channel_lock': {'enabled': True}},
            {'id': 'AO_1004', 'channel_lock': {'enabled': False}},
        ],
    },
    # Absolute positions, offsets and azimuth/elevation locks, including the Cartesian object that the
    # policy turns polar, with jumpPosition ignored and head locking by content.
    'policy-position.json': {
        'schema': POLICY_SCHEMA,
        'global': {'interpolation': {'honor_jump_position': False}},
        'objects': [
            {'id': 'AO_1001', 'position': {'azimuth': -60, 'elevation': 20}},
            {'name_glob': '*alt', 'position': {'lock_azimuth': 45, 'offset': {'azimuth': 10, 'elevation': -5, 'distance': 0.5}}},
            {'name': 'FX Cartesian', 'position': {'lock_elevation': 30, 'offset': {'azimuth': -15}}},
            {'content': 'Ambience', 'head_locked': True},
        ],
    },
    # DirectSpeakers filters on the 5.1 bed whose LFE1 carries an ADM lowPass: a global LFE rule and
    # per-label rules, applied in order and each with its own filter.
    'policy-direct-speakers.json': {
        'schema': POLICY_SCHEMA,
        'global': {'direct_speakers': {'lfe': True, 'gain': {'gain_db': -6}}},
        'objects': [
            {'all': True, 'direct_speakers': {'speaker_label': 'm+110', 'gain': {'scale': 0.5},
                                              'position': {'azimuth': 120}}},
            {'name_glob': '*', 'direct_speakers': {'speaker_label': 'M+000', 'head_locked': True}},
            {'all': True, 'direct_speakers': {'lfe': False, 'speaker_label': 'M-030', 'gain': {'mute': True}}},
        ],
    },
    # Triple Balance keeps equal-size extent inside the room cube: uniform extent scale and clamp, gain.
    'policy-triple-balance.json': {
        'schema': POLICY_SCHEMA,
        'global': {'extent': {'scale': 1.5, 'max': 0.75}, 'gain': {'gain_db': -1.5}},
        'objects': [{'all': True, 'diffuse': {'enabled': False}}],
    },
    # Weighted, split and muted routes covering every non-LFE channel of the directspeakers bed.
    'ds-matrix-51.json': {
        'schema': 'mradm.direct-speakers-matrix.v1',
        'output_layout': '5.1',
        'routes': [
            {'source_label': 'M+000', 'targets': [{'label': 'M+030', 'weight': 1}, {'label': 'M-030', 'weight': 3}]},
            {'source_label': 'M+030', 'targets': [{'label': 'M+030', 'weight': 1}]},
            {'source_label': 'M-030', 'targets': [{'label': 'M-030', 'weight': 2}, {'label': 'M+000', 'weight': 1}]},
            {'source_label': 'M+110', 'targets': [{'label': 'M+110', 'weight': 2}, {'label': 'M+030', 'weight': 1}]},
            {'source_label': 'M-110', 'mute': True},
        ],
    },
    # Reference and reproduction screens for the room renderer's screen-referenced object.
    'screen-config.json': {
        'schema': 'mradm.screen-config', 'version': 1,
        'reference': {'coordinates': 'polar', 'centre': [0, 0, 1], 'width': 58, 'aspect_ratio': 1.78},
        'reproduction': {'coordinates': 'polar', 'centre': [5, -2, 1], 'width': 90, 'aspect_ratio': 1.6},
    },
}


# The synthetic room pack (tests/tools/make_room_phase2_fixtures.py). Its phase table came from the
# platform libm, so these committed bytes are the input on every platform, never regenerated.
ROOM_FIXTURE_DIR = 'tests/fixtures/room/phase2'
ROOM_FIXTURES = {
    'bands.f64': '65c5345153d2f48b057ce282be27f774447677c4afb9cde391a88c6bd1acdb29',
    'delays.f64': 'f5a9165f9a4852698b1b7bd7512f2f602da6b2688a9a4106be78a67455a7b044',
    'directions.f64': '919bad9bc21c53f36119acf78720fa4ab632ae16de52faf914bf12fb3b9ad1e8',
    'eq.f64': '36d1f313e90cb0d038c0fad2f3775286629b047f0d32ca64a6d6e27a1472e5d1',
    'faces.u32': '2b01153bd81c50c9bde2f118c9053419f74c889e72618efa5e27555c4168e1e5',
    'hrir.f32': 'ae6e48971ac2408354aa765e02909cb146432c9a1c71d5cce0bb01578463b1ca',
    'hrir.json': 'b920747e2e6b704207e74e7ede3ebf1c7f16955f8852817ebb16fbe2b6f9b4cf',
    'late-directions.f64': '919bad9bc21c53f36119acf78720fa4ab632ae16de52faf914bf12fb3b9ad1e8',
    'late.f32': '1ddeebc4b02202fd13ff29514d72fac33da349c241eb5aa4076d02d3412bfc97',
    'phases.f32': '741062476d4f6cb9394484bf0b281a39ed8e2df9712d5ea6ff24f803a4d8322d',
    'room.json': '8a21e8afcb6b6d00c7b196889ed2d00c9ee67afc85048dc5081887ee03974696',
}


# Frames the pinned room appends after the end of the material (late reverberation and alignment).
ROOM_TAIL_FRAMES = 4441


def validate_room_fixtures(directory):
    names = {p.name for p in directory.iterdir()} if directory.is_dir() else set()
    if names != set(ROOM_FIXTURES):
        raise ValueError('room fixture inventory differs from the pinned pack')
    for name, expected in ROOM_FIXTURES.items():
        if digest((directory / name).read_bytes()) != expected:
            raise ValueError('modified pinned room fixture: ' + name)


def write_json_fixtures(directory):
    for name, value in JSON_FIXTURES.items():
        save(directory / name, value)


def validate_json_fixtures(directory):
    for name, value in JSON_FIXTURES.items():
        path = directory / name
        if not path.is_file() or json.loads(path.read_text(encoding='utf-8')) != value:
            raise ValueError('missing or modified JSON fixture: ' + name)


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
    for sofa in ('simple.sofa', 'off-axis-compressed.sofa'):
        for mode, fixture in [('point', 'objects-point'), ('cloud', 'objects-extent-multi'),
                              ('saf-spreader', 'objects-extent-multi')]:
            args = ['--no-peak-limit', '--renderer', 'saf-binaural', '--output-layout', 'binaural']
            if mode != 'point':
                args += ['--binaural-spread-mode', mode]
            result.append({'id': f'sofa-{Path(sofa).stem}-{mode}', 'fixture': fixture, 'args': args, 'sofa': sofa})
    # Later slices extend the matrix outside the frozen phase-two parameter set.
    result += [
        {'id': 'ear-hoa3', 'fixture': 'hoa3', 'slice': '3.3',
         'args': ['--no-peak-limit', '--renderer', 'ear', '--output-layout', '7.1.4']},
        {'id': 'hoa-loudness-minus23', 'fixture': 'objects-extent-multi', 'slice': '3.3',
         'args': ['--no-peak-limit', '--renderer', 'hoa', '--output-layout', 'hoa3', '--loudness-target', '-23']},
        # PCM consistency 3.4: Triple Balance size path (standard mode) and D mode on a moving,
        # equal-size Cartesian object.
        {'id': 'triple-size-714', 'fixture': 'objects-room', 'slice': '3.4',
         'args': ['--no-peak-limit', '--renderer', 'triple-balance', '--output-layout', '7.1.4']},
        {'id': 'triple-size-916', 'fixture': 'objects-room', 'slice': '3.4',
         'args': ['--no-peak-limit', '--renderer', 'triple-balance', '--output-layout', '9.1.6']},
        {'id': 'triple-size-222', 'fixture': 'objects-room', 'slice': '3.4',
         'args': ['--no-peak-limit', '--renderer', 'triple-balance', '--output-layout', '22.2']},
        {'id': 'triple-size-window', 'fixture': 'objects-room', 'slice': '3.4',
         'args': ['--no-peak-limit', '--renderer', 'triple-balance', '--output-layout', '9.1.6', '--start', '0.2', '--end', '0.7']},
        {'id': 'triple-d-916', 'fixture': 'objects-room', 'slice': '3.4',
         'args': ['--no-peak-limit', '--renderer', 'triple-balance', '--output-layout', '9.1.6', '--triple-balance-mode', 'd']},
        {'id': 'triple-d-222', 'fixture': 'objects-room', 'slice': '3.4',
         'args': ['--no-peak-limit', '--renderer', 'triple-balance', '--output-layout', '22.2', '--triple-balance-mode', 'd']},
        # PCM consistency 3.5: ADM dB gains, a Cartesian DirectSpeakers channel and 40 blocks sharing a
        # start (the last in document order is in effect), plus decimal command-line values that sit
        # next to binary32 rounding boundaries.
        {'id': 'params-ear', 'fixture': 'params', 'slice': '3.5',
         'args': ['--no-peak-limit', '--renderer', 'ear', '--output-layout', '7.1.4']},
        {'id': 'params-saf', 'fixture': 'params', 'slice': '3.5',
         'args': ['--no-peak-limit', '--renderer', 'saf', '--output-layout', '5.1']},
        {'id': 'params-hoa', 'fixture': 'params', 'slice': '3.5',
         'args': ['--no-peak-limit', '--renderer', 'hoa', '--output-layout', 'hoa3']},
        {'id': 'params-binaural', 'fixture': 'params', 'slice': '3.5',
         'args': ['--no-peak-limit', '--renderer', 'saf-binaural', '--output-layout', 'binaural']},
        {'id': 'params-binaural-rotated', 'fixture': 'params', 'slice': '3.5',
         'args': ['--no-peak-limit', '--renderer', 'saf-binaural', '--output-layout', 'binaural',
                  '--listener-yaw', '37.000001', '--listener-pitch', '1.25e1', '--listener-roll', '-9.9999999']},
        # PCM consistency 3.6: the final file. Integer WAV (16 / 24-bit quantization and clipping),
        # the 7.1.4 WAVE channel permutation and ADM BW64 metadata on integer PCM, the final gain
        # rewrite past 0 dBFS, float32 CAF, and trims whose seconds land on (or next to) half a frame.
        {'id': 'final-ear-714-i24', 'fixture': 'objects-extent', 'slice': '3.6',
         'args': ['--no-peak-limit', '--renderer', 'ear', '--output-layout', '7.1.4', '--output-bit-depth', 'i24']},
        {'id': 'final-ear-51-i16-clip', 'fixture': 'objects-point', 'slice': '3.6',
         'args': ['--no-peak-limit', '--renderer', 'ear', '--output-layout', '5.1', '--final-gain-db', '6.5',
                  '--output-bit-depth', 'i16']},
        {'id': 'final-ear-51-f32-gain', 'fixture': 'objects-point', 'slice': '3.6',
         'args': ['--no-peak-limit', '--renderer', 'ear', '--output-layout', '5.1', '--final-gain-db', '6.5']},
        {'id': 'final-hoa3-i24-trim', 'fixture': 'objects-point', 'slice': '3.6',
         'args': ['--no-peak-limit', '--renderer', 'hoa', '--output-layout', 'hoa3',
                  '--start', '0.25001041666666666', '--end', '0.7000104166666667', '--output-bit-depth', 'i24']},
        {'id': 'final-binaural-i16-trim-gain', 'fixture': 'objects-extent', 'slice': '3.6',
         'args': ['--no-peak-limit', '--renderer', 'saf-binaural', '--output-layout', 'binaural',
                  '--binaural-spread-mode', 'cloud', '--start', '0.1', '--end', '0.65', '--final-gain-db', '-3.25',
                  '--output-bit-depth', 'i16']},
        {'id': 'final-saf-222-i24', 'fixture': 'directspeakers', 'slice': '3.6',
         'args': ['--no-peak-limit', '--renderer', 'saf', '--output-layout', '22.2', '--output-bit-depth', 'i24']},
        {'id': 'final-triple-916-i24', 'fixture': 'objects-room', 'slice': '3.6',
         'args': ['--no-peak-limit', '--renderer', 'triple-balance', '--output-layout', '9.1.6',
                  '--output-bit-depth', 'i24']},
        {'id': 'final-caf-ear-714-trim-gain', 'fixture': 'objects-extent', 'slice': '3.6', 'container': 'caf',
         'args': ['--no-peak-limit', '--renderer', 'ear', '--output-layout', '7.1.4',
                  '--start', '0.25001041666666666', '--final-gain-db', '2.5']},
        {'id': 'final-caf-binaural-postproc', 'fixture': 'objects-point', 'slice': '3.6', 'container': 'caf',
         'args': ['--renderer', 'saf-binaural', '--output-layout', 'binaural']},
        # PCM consistency 4.2: offline object smoothing across the four moving blocks of objects-room
        # for VBAP, HOA and binaural (EAR has ear-window; Triple Balance rejects smoothing), and the
        # automatic loudness and peak gains on VBAP and Triple Balance.
        {'id': 'smoothing-saf-714', 'fixture': 'objects-room', 'slice': '4.2',
         'args': ['--no-peak-limit', '--renderer', 'saf', '--output-layout', '7.1.4',
                  '--object-smoothing-frames', '2048']},
        {'id': 'smoothing-hoa3', 'fixture': 'objects-room', 'slice': '4.2',
         'args': ['--no-peak-limit', '--renderer', 'hoa', '--output-layout', 'hoa3',
                  '--object-smoothing-frames', '2048']},
        {'id': 'smoothing-binaural', 'fixture': 'objects-room', 'slice': '4.2',
         'args': ['--no-peak-limit', '--renderer', 'saf-binaural', '--output-layout', 'binaural',
                  '--object-smoothing-frames', '2048']},
        {'id': 'saf-loudness-minus23', 'fixture': 'objects-extent-multi', 'slice': '4.2',
         'args': ['--no-peak-limit', '--renderer', 'saf', '--output-layout', '7.1.4', '--loudness-target', '-23']},
        {'id': 'saf-peak-minus6', 'fixture': 'objects-point', 'slice': '4.2',
         'args': ['--renderer', 'saf', '--output-layout', '5.1', '--peak-normalize-to-limit', '--peak-limit-dbtp', '-6']},
        {'id': 'triple-loudness-minus23', 'fixture': 'objects-room', 'slice': '4.2',
         'args': ['--no-peak-limit', '--renderer', 'triple-balance', '--output-layout', '7.1.4',
                  '--loudness-target', '-23']},
        {'id': 'triple-peak-minus6', 'fixture': 'objects-room', 'slice': '4.2',
         'args': ['--renderer', 'triple-balance', '--output-layout', '9.1.6', '--peak-normalize-to-limit',
                  '--peak-limit-dbtp', '-6']},
        # PCM consistency 4.2: the semantic policy matrix (JSON_FIXTURES) and DirectSpeakers matrix
        # routing, the paths phase 4.4 moves to Rust. The policy cases are the first whose 06-policy
        # render-scene checkpoint differs from 05-import.
        {'id': 'policy-identity-ear-714', 'fixture': 'policy-objects', 'slice': '4.2',
         'files': {'--semantic-policy': 'policy-identity.json'},
         'args': ['--no-peak-limit', '--renderer', 'ear', '--output-layout', '7.1.4']},
        {'id': 'policy-lock-saf-714', 'fixture': 'policy-objects', 'slice': '4.2',
         'files': {'--semantic-policy': 'policy-lock.json'},
         'args': ['--no-peak-limit', '--renderer', 'saf', '--output-layout', '7.1.4']},
        {'id': 'policy-position-binaural', 'fixture': 'policy-objects', 'slice': '4.2',
         'files': {'--semantic-policy': 'policy-position.json'},
         'args': ['--no-peak-limit', '--renderer', 'saf-binaural', '--output-layout', 'binaural']},
        {'id': 'policy-direct-speakers-ear-51', 'fixture': 'directspeakers-lfe', 'slice': '4.2',
         'files': {'--semantic-policy': 'policy-direct-speakers.json'},
         'args': ['--no-peak-limit', '--renderer', 'ear', '--output-layout', '5.1']},
        {'id': 'policy-triple-714', 'fixture': 'objects-room', 'slice': '4.2',
         'files': {'--semantic-policy': 'policy-triple-balance.json'},
         'args': ['--no-peak-limit', '--renderer', 'triple-balance', '--output-layout', '7.1.4']},
        {'id': 'ds-matrix-ear-51', 'fixture': 'directspeakers', 'slice': '4.2',
         'files': {'--direct-speakers-matrix': 'ds-matrix-51.json'},
         'args': ['--no-peak-limit', '--renderer', 'ear', '--output-layout', '5.1', '--direct-speakers-routing', 'matrix']},
        {'id': 'ds-matrix-saf-51', 'fixture': 'directspeakers', 'slice': '4.2',
         'files': {'--direct-speakers-matrix': 'ds-matrix-51.json'},
         'args': ['--no-peak-limit', '--renderer', 'saf', '--output-layout', '5.1', '--direct-speakers-routing', 'matrix']},
        # PCM consistency 4.2: room-binaural at the exact tier (ADR 0020) on the pinned synthetic room
        # pack: its own HRIR, a pinned SOFA set, and screen-referenced objects with a screen configuration.
        {'id': 'room-exact', 'fixture': 'objects-extent-multi', 'slice': '4.2',
         'files': {'--room': 'room/room.json'},
         'args': ['--no-peak-limit', '--renderer', 'room-binaural', '--output-layout', 'binaural', '--room-quality', 'exact']},
        {'id': 'room-exact-sofa', 'fixture': 'objects-room', 'slice': '4.2', 'sofa': 'simple.sofa',
         'files': {'--room': 'room/room.json'},
         'args': ['--no-peak-limit', '--renderer', 'room-binaural', '--output-layout', 'binaural', '--room-quality', 'exact']},
        {'id': 'room-exact-screen', 'fixture': 'policy-objects', 'slice': '4.2',
         'files': {'--room': 'room/room.json', '--screen-config': 'screen-config.json'},
         'args': ['--no-peak-limit', '--renderer', 'room-binaural', '--output-layout', 'binaural', '--room-quality', 'exact']},
    ]
    return result


def stream_cases():
    """File-streaming replays (PCM consistency 3.1 binaural, 3.2 EAR, 3.3 HOA, 3.4 Triple Balance).

    An IRenderStream is pulled in a fixed or fragmented request partition while live gain, mute
    and (binaural) topology overrides, a head orientation and a seek land at fixed output frames.
    The IRenderStream contract makes the delivered PCM independent of the request partition.
    Version 3 names the renderer, the requested output layout and the channel count; version 4
    (PCM consistency 4.2) adds the room pack of a room-binaural replay and the tail it appends.
    """
    binaural_events = [
        {'frame': 0, 'objects': [{'index': 0, 'gain_db': -3.0}]},
        {'frame': 7001, 'objects': [{'index': 0, 'gain_db': -14.5}]},
        {'frame': 12345, 'objects': [{'index': 0, 'gain_db': 2.25, 'extent_scale': 0.5, 'diffuse_scale': 1.5}]},
        {'frame': 20000, 'orientation': [37.0, 12.0, -9.0]},
        {'frame': 26000, 'objects': [{'index': 0, 'gain_db': -6.0, 'width_scale': 1.75, 'height_scale': 0.25,
                                      'depth_scale': 2.0, 'divergence_scale': 0.5}]},
        {'frame': 31000, 'seek': 4800},
        {'frame': 40000, 'objects': [{'index': 0, 'mute': True}]},
        {'frame': 44000, 'objects': []},
    ]
    # EAR streams apply live gain and mute per input channel; DirectSpeakers beds also take
    # per-channel overrides by speaker label, which win over the whole-object entry.
    object_events = [
        {'frame': 0, 'objects': [{'index': 0, 'gain_db': -3.0}]},
        {'frame': 7001, 'objects': [{'index': 0, 'gain_db': -14.5}]},
        {'frame': 12345, 'objects': [{'index': 0, 'gain_db': 2.25}]},
        {'frame': 26000, 'objects': [{'index': 0, 'gain_db': -6.0}]},
        {'frame': 31000, 'seek': 4800},
        {'frame': 40000, 'objects': [{'index': 0, 'mute': True}]},
        {'frame': 44000, 'objects': []},
    ]
    bed_events = [
        {'frame': 0, 'objects': [{'index': 0, 'gain_db': -3.0}]},
        {'frame': 7001, 'objects': [{'index': 0, 'gain_db': -1.5},
                                    {'index': 0, 'speaker_label': 'M+030', 'gain_db': -12.0},
                                    {'index': 0, 'speaker_label': 'M-110', 'mute': True}]},
        {'frame': 12345, 'objects': [{'index': 0, 'speaker_label': 'M+000', 'gain_db': -20.0}]},
        {'frame': 26000, 'objects': [{'index': 0, 'gain_db': 2.25}]},
        {'frame': 31000, 'seek': 4800},
        {'frame': 40000, 'objects': [{'index': 0, 'mute': True},
                                     {'index': 0, 'speaker_label': 'M-030', 'gain_db': -6.0}]},
        {'frame': 44000, 'objects': []},
    ]
    # Triple Balance edits gain, mute and one linked isotropic size scale.
    room_events = [
        {'frame': 0, 'objects': [{'index': 0, 'gain_db': -3.0}]},
        {'frame': 7001, 'objects': [{'index': 0, 'gain_db': -14.5}]},
        {'frame': 12345, 'objects': [{'index': 0, 'gain_db': 2.25, 'extent_scale': 0.5}]},
        {'frame': 26000, 'objects': [{'index': 0, 'gain_db': -6.0, 'extent_scale': 1.5}]},
        {'frame': 31000, 'seek': 4800},
        {'frame': 40000, 'objects': [{'index': 0, 'mute': True}]},
        {'frame': 44000, 'objects': []},
    ]
    # HOA input packs carry no AudioObject that a live override could address.
    seek_events = [{'frame': 31000, 'seek': 4800}]
    variants = [('binaural-cloud', 'objects-extent-multi', 'saf-binaural', 'binaural', 2, 'cloud', binaural_events),
                ('binaural-cartesian', 'objects-cartesian', 'saf-binaural', 'binaural', 2, 'cloud', binaural_events),
                ('binaural-point', 'objects-point', 'saf-binaural', 'binaural', 2, 'none', binaural_events),
                ('ear-point', 'objects-point', 'ear', '7.1.4', 12, None, object_events),
                ('ear-extent', 'objects-extent-multi', 'ear', '9.1.6', 16, None, object_events),
                ('ear-bed', 'directspeakers', 'ear', '7.1.4', 12, None, bed_events),
                ('hoa-extent', 'objects-extent-multi', 'hoa', 'hoa3', 16, None, object_events),
                ('hoa-bed', 'directspeakers', 'hoa', 'hoa3', 16, None, bed_events),
                ('ear-hoa3', 'hoa3', 'ear', '7.1.4', 12, None, seek_events),
                ('triple-714', 'objects-room', 'triple-balance', '7.1.4', 12, None, room_events),
                ('triple-916', 'objects-room', 'triple-balance', '9.1.6', 16, None, room_events),
                ('triple-222', 'objects-room', 'triple-balance', '22.2', 24, None, room_events)]
    result = []
    for name, fixture, renderer, layout, channels, spread, events in variants:
        for partition_name, partition in [('fixed', [512]), ('fragmented', [1, 7, 127, 511, 1024])]:
            case = {'id': f'stream-{name}-{partition_name}', 'version': 3, 'fixture': fixture,
                    'renderer': renderer, 'output_layout': layout, 'channels': channels}
            if spread is not None:
                case['spread'] = spread
            case.update({'partition': partition, 'input_frames': 48000, 'sample_rate': 48000,
                         'frames': 96000, 'events': events})
            result.append(case)
    # room-binaural at the exact tier with live overrides, a head orientation and a seek. Room
    # switches are asynchronous (a preparation thread decides when the new room starts), so they
    # have no fixed output frame and stay outside the replay.
    for name, fixture, spread in [('room-extent', 'objects-extent-multi', 'cloud'), ('room-cartesian', 'objects-cartesian', 'none')]:
        for partition_name, partition in [('fixed', [512]), ('fragmented', [1, 7, 127, 511, 1024])]:
            result.append({'id': f'stream-{name}-{partition_name}', 'version': 4, 'fixture': fixture,
                           'renderer': 'room-binaural', 'output_layout': 'binaural', 'channels': 2, 'spread': spread,
                           'room': 'room/room.json', 'tail_frames': ROOM_TAIL_FRAMES, 'partition': partition,
                           'input_frames': 48000, 'sample_rate': 48000, 'frames': 96000, 'events': binaural_events})
    return result


def stream_output_shape(case):
    """Expected PCM shape from the pinned input and seek timeline, independent of measured output."""
    # A room-binaural stream continues past EOS with its room tail (version 4).
    input_frames, limit = case['input_frames'] + case.get('tail_frames', 0), case['frames']
    position, media_position = 0, 0
    for event in case['events']:
        frame = event['frame']
        if not position <= frame < limit:
            raise ValueError('file-streaming event outside the ordered replay timeline')
        advance = frame - position
        if advance > input_frames - media_position:
            raise ValueError('file-streaming event after expected EOS')
        media_position += advance
        position = frame
        if 'seek' in event:
            if event['seek'] < 0:
                raise ValueError('negative file-streaming seek target')
            media_position = min(event['seek'], input_frames)
    frames = position + min(limit - position, input_frames - media_position)
    return (case['channels'], case['sample_rate'], frames)


def validate_stream_shape(case, shape):
    if tuple(shape) != stream_output_shape(case):
        raise ValueError('file-streaming PCM shape/frame count differs from replay timeline: ' + case['id'])


def case_ids():
    """Every PCM output id: offline cases, both epochs of each Scene replay and file-streaming replays."""
    ids = [c['id'] for c in offline_cases()]
    ids += [c['id'] + f'-epoch{e}' for c in scene_cases() for e in (1, 2)]
    ids += [c['id'] for c in stream_cases()]
    return ids


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
        for input_rate, output_rate in [(48000, 48000), (48000, 44100), (44100, 48000),
                                        (96000, 96000), (48000, 96000), (96000, 48000)]:
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
    # Version 2 adds speaker-backend switching; the original replay catalog stays frozen.
    for layout, sized, input_rate, output_rate in [
            (layout, sized, 48000, 48000) for layout in ('7.1.4', '9.1.6', '22.2') for sized in (False, True)
    ] + [('7.1.4', False, 96000, 96000), ('9.1.6', True, 48000, 44100)]:
        for partition_name, partition in [('fixed', [512]), ('fragmented', [1, 7, 127, 511, 1024])]:
            row = dict(result[0], id=f'scene-triple-balance-{layout}-size{int(sized)}-{input_rate}-{output_rate}-{partition_name}',
                       version=2, backend='triple-balance', layout=layout, cloud=sized,
                       input_rate=input_rate, output_rate=output_rate, partition=partition)
            row['controls'] = dict(row['controls'], **{'8192': 'switch speaker backend'})
            row['metadata'] = [dict(event) for event in metadata[:3]]
            row['metadata'] += ([{'sample': 3073, 'field': 'size', 'value': 0.7, 'ramp': 97},
                                 {'sample': 5003, 'field': 'size', 'value': 0.0, 'ramp': 71}] if sized else
                                [{'sample': 3073, 'field': 'position', 'value': [0.75, -0.25, 0.5], 'ramp': 97},
                                 {'sample': 5003, 'field': 'position', 'value': [0.0, 1.0, 0.0], 'ramp': 0}])
            result.append(row)
    # Version 3 keeps the binaural backend and switches the external dataset to KEMAR.
    # Together these exercise native HRIRs, HRIR resampling and output resampling.
    for sofa, input_rate, output_rate in [('simple.sofa', 48000, 44100),
                                          ('general-compressed.sofa', 48000, 48000),
                                          ('off-axis-compressed.sofa', 44100, 48000)]:
        for cloud in (False, True):
            for partition_name, partition in [('fixed', [512]), ('fragmented', [1, 7, 127, 511, 1024])]:
                row = dict(result[0], id=f'scene-sofa-{Path(sofa).stem}-cloud{int(cloud)}-{input_rate}-{output_rate}-{partition_name}',
                           version=3, backend='binaural', layout='binaural', cloud=cloud, sofa=sofa,
                           input_rate=input_rate, output_rate=output_rate, partition=partition)
                row['controls'] = dict(row['controls'], **{'8192': 'switch SOFA to built-in KEMAR'})
                result.append(row)
    # Version 4 runs the device DSP (HpTF transitions, peak guard, volume) behind
    # resampled output: an external SOFA set that switches to KEMAR, and a stereo
    # VBAP feed that switches to binaural (PCM consistency 1.6).
    device = next(row for row in result if row['id'] == 'scene-device-dsp-fixed')
    for name, backend, layout, sofa, input_rate, output_rate in [
            ('sofa-simple', 'binaural', 'binaural', 'simple.sofa', 48000, 44100),
            ('vbap', 'vbap', '0+2+0', None, 44100, 48000)]:
        for partition_name, partition in [('fixed', [512]), ('fragmented', [1, 7, 127, 511, 1024])]:
            row = dict(device, id=f'scene-device-dsp-{name}-{input_rate}-{output_rate}-{partition_name}', version=4,
                       backend=backend, layout=layout, input_rate=input_rate, output_rate=output_rate, partition=partition)
            if sofa:
                row['sofa'] = sofa
                row['controls'] = dict(row['controls'], **{'8192': 'switch SOFA to built-in KEMAR'})
            result.append(row)
    # Extend the same multichannel VBAP replay contract to the 24-channel 22.2 layout.
    for rate in (48000, 96000):
        for partition_name in ('fixed', 'fragmented'):
            template = next(row for row in result
                            if row['id'] == f'scene-vbap-4+7+0-cloud0-{rate}-{rate}-{partition_name}')
            result.append(dict(template, id=f'scene-vbap-9+10+3-cloud0-{rate}-{rate}-{partition_name}',
                               layout='9+10+3'))
    # Version 5 (PCM consistency 4.2): the element carries an ADM identity and the case its own policy,
    # so SceneStreamEngine's per-update policy application is covered by identity-matched object rules.
    # Triple Balance keeps Cartesian room positions, so its policy has no polar offset.
    identity = {'object_id': 'AO_1001', 'object_name': 'Dialogue Lead', 'track_uid': 'ATU_00000001',
                'importance': 9, 'dialogue_id': 1, 'contents': [{'id': 'ACO_1001', 'name': 'Dialogue'}],
                'programmes': [{'id': 'APR_1001', 'name': 'Main Mix'}]}
    rules = [{'name_glob': 'dialogue*', 'gain': {'gain_db': -3.5}, 'extent': {'scale': 1.25, 'max': 0.5}},
             {'content': 'dialogue', 'diffuse': {'scale': 0.5}},
             {'importance_min': 8, 'divergence': {'range_scale': 0.5}},
             {'track_uid': 'atu_00000001', 'gain': {'scale': 0.75}}]
    for template_id, offset in [('scene-vbap-4+7+0-cloud0-48000-48000', True),
                                ('scene-binaural-binaural-cloud1-48000-48000', True),
                                ('scene-triple-balance-7.1.4-size1-48000-48000', False)]:
        # The binaural replay switches to stereo VBAP at 8192, whose two-speaker pair renders silence
        # outside +-30 degrees; the offset keeps the replayed source inside the pair.
        objects = rules + ([{'programme': 'Main Mix', 'position': {'offset': {'azimuth': 5.5, 'elevation': 6}}}]
                           if offset else [])
        policy = {'schema': POLICY_SCHEMA, 'global': {'gain': {'scale': 0.875}}, 'objects': objects}
        for partition_name in ('fixed', 'fragmented'):
            template = next(row for row in result if row['id'] == f'{template_id}-{partition_name}')
            row = dict(template, id=template_id.replace('scene-', 'scene-policy-', 1) + '-' + partition_name,
                       version=5, semantic={'identity': identity, 'policy': policy})
            row['controls'] = dict(row['controls'], **{'6144': 'semantic policy and identity from the case'})
            result.append(row)
    # Version 6 (PCM consistency 4.2): room-binaural at the exact tier on the pinned room pack, with
    # and without extent spread. Room switches are asynchronous, so the replay keeps one room.
    for cloud in (False, True):
        for partition_name in ('fixed', 'fragmented'):
            template = next(row for row in result
                            if row['id'] == f'scene-binaural-binaural-cloud{int(cloud)}-48000-48000-{partition_name}')
            row = dict(template, id=f'scene-room-binaural-cloud{int(cloud)}-48000-48000-{partition_name}', version=6,
                       backend='room-binaural', room='room/room.json', tail_frames=ROOM_TAIL_FRAMES)
            row['controls'] = dict(row['controls'], **{'8192': 'no backend switch'})
            result.append(row)
    return result



def partition_invariant(row):
    """Scene replays whose fixed and fragmented partitions must give identical PCM.

    VBAP and Triple Balance render from absolute sample positions, and an
    unchanged rate bypasses the resampler. Binaural convolution, a stereo switch
    to it and fractional resampling ratios depend on block boundaries; their
    partitions stay separate cases (PCM consistency 1.5)."""
    return (row['id'].endswith('-fixed') and row['backend'] in ('vbap', 'triple-balance')
            and row['layout'] != '0+2+0' and row['input_rate'] == row['output_rate'])

FFT_SIZES = (256, 512, 1024, 2048, 4096, 8192, 16384, 32768)
RESAMPLER_RATES = ((48000, 48000), (48000, 44100), (44100, 48000), (96000, 96000),
                   (48000, 96000), (96000, 48000), (48000, 192000), (192000, 48000))
OM_EDGE_CASES = ('zero', 'repeated', 'rank-one', 'antiphase', 'near-rank-one', 'tiny',
                 'complex-rank-one', 'floor-adjacent')
HRTF_PROBE_SIZES = (256, 1024)
# Production meter probe: 4x, 2x and no oversampling; specialized stereo and generic 12-channel scanners.
METER_RATES = (44100, 48000, 96000, 192000)
METER_CHANNELS = (2, 12)


def kernel_outputs():
    """Measurement files written by the mr_adm_phase2_kernels probe, per pass."""
    names = ['fft-twiddles.10-libm.f64', 'fft-twiddles.20-table.f32']
    names += [f'fft-{n}.{part}' for n in FFT_SIZES for part in ('10-input.f32', '20-spectrum.c32', '30-inverse.f32')]
    names += ['scene.10-input.f32', 'scene.20-output.f32']
    names += [f'ear-{layout}.{part}' for layout in ('0+5+0', '4+7+0', '9+10+3') for part in ('10-layout.f64', '20-fir.f32')]
    names += [f'om-{i}.{part}' for i in range(4)
              for part in ('10-input.f32', '20-real.f32', '30-complex.c32', '40-residual.f32')]
    names += [f'resampler-{a}-{b}.{part}' for a, b in RESAMPLER_RATES
              for part in ('10-input.f32', '20-output.f32')]
    names += [f'om-edge-{name}.{part}' for name in OM_EDGE_CASES
              for part in ('10-input.f32', '20-real.f32', '30-complex.c32', '40-residual.f32')]
    names += [f'hrtf-{size}.{part}' for size in HRTF_PROBE_SIZES for part in
              ('10-directions.f32', '11-hrir.f32', '12-queries.f32', '13-parameters.i32',
               '20-grid-weights.f32', '21-grid-indices.i32', '30-spectra.c32',
               '40-quantized.c32', '41-magnitudes.f32', '42-complex-sum.c32',
               '43-scales.f32', '50-continuous.c32')]
    names += [f'hptf-{rate}.{part}' for rate in (44100, 48000) for part in ('10-input.f64', '20-coefficients.f32')]
    names += ['trig.10-input.f64', 'trig.20-sin.f64', 'trig.30-cos.f64']
    names += [f'meter-{rate}.05-k-weighting.f64' for rate in METER_RATES]
    names += [f'stream-dsp.{part}' for part in ('10-spread.f32', '20-fold.f32', '30-live-gain.f32', '40-mix.f32')]
    names += [f'ear-gains.{part}' for part in ('10-point.f64', '20-extent.f64', '30-direct.f64')]
    names += [f'hoa-encode.{part}' for part in ('10-point.f32', '20-extent.f32', '30-direct.f32')]
    names += ['ear-hoa.10-decode.f64']
    names += [f'triple-balance.{part}' for part in ('10-point.f32', '20-room.f32', '30-size.f32', '40-d-mode.f32')]
    names += ['adm-import.10-gain.f32', 'adm-import.20-cartesian.f32', 'decimal.10-parse.f64']
    names += ['timeline.10-seconds-to-frame.f64'] + [f'wav-quantize.{part}' for part in ('10-pcm16.i32', '20-pcm24.i32', '30-pcm32.i32')]
    names += [f'post-gain.{part}' for part in ('10-conversions.f64', '20-render-plan.f64', '21-render-gain.f32',
                                                '30-standalone.f64', '31-standalone-gain.f32')]
    names += [f'meter-{rate}-{channels}.{part}' for rate in METER_RATES for channels in METER_CHANNELS
              for part in ('10-parameters.i32', '20-true-peak.f64', '30-loudness.f64')]
    names += [f'meter-acceptance-{rate}.{part}' for rate in METER_RATES
              for part in ('10-gates.f64', '20-true-peak.f64', '30-long-silence.f64', '40-chunking.f64', '50-reset.f64')]
    names += ['spreader.10-input.f32', 'spreader.20-voronoi.f32', 'spreader.30-fir.c32', 'spreader.40-output.f32']
    names += [f'sofa-{Path(sofa).stem}.{part}' for sofa in SOFA_FIXTURES for part in
              ('10-parameters.i32', '11-directions.f32', '12-hrir.f32', '20-grid-weights.f32',
               '21-grid-indices.i32', '30-spectra.c32', '40-queries.f32', '50-quantized.c32', '60-continuous.c32')]
    names += ['room.10-hrir.f32', 'room.20-impulse.f32']
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


def validate_sofa_pcm(path):
    shape, bits = pcm_bytes(path)
    values = validate_words(bits, '.f32')
    if shape[0] != 2 or any(not any(abs(v) > 1e-9 for v in values[ear::2]) for ear in (0, 1)):
        raise ValueError('SOFA output must contain non-silent stereo PCM')


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
             'mr_adm_phase2_scene', 'mr_adm_phase2_stream', 'mr_adm_phase2_kernels', 'mr_adm_repeat_render')
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
