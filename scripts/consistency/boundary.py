"""Shared parts of the boundary records (phase 6): the CLI transcript and the C ABI capture.

Both record one implementation of a public boundary on one build and compare two records byte for byte.
NORMALIZATIONS is the complete list of rewrites compare applies; every other byte is compared.
"""
import datetime
import hashlib
import os
from pathlib import Path
import re
import shutil
import subprocess

import collection as common

# Bytes of the non-WAVE input used by the error cases.
TEXT_FIXTURE = b'not a RIFF/RF64/BW64 file\n'
ISO_UTC = re.compile(rb'(\d{4})-(\d{2})-(\d{2})T(\d{2}):(\d{2}):(\d{2})Z')
CLOCK_PLACEHOLDER = b'0000-00-00T00:00:00Z'
BEXT_CLOCK_PLACEHOLDER = b'0000-00-0000-00-00'
SIDECAR = re.compile(rb'\.((?:render|gain|trim)_tmp)\.[0-9a-f]{16}\.')
NORMALIZATIONS = (
    'root: the absolute record directory (as recorded, with / and \\ separators) becomes <root>',
    'clock: an ISO 8601 UTC time YYYY-MM-DDTHH:MM:SSZ inside the record\'s own wall-clock window '
    '(start - 1 s to end + 1 s) becomes 0000-00-00T00:00:00Z, same length',
    'bext clock: in a RIFF/RF64/BW64 file, the bext OriginationDate and OriginationTime (payload bytes 320-338) '
    'inside the same window become 0000-00-0000-00-00',
    'sidecar: in text, the random 16-hex token of a .render_tmp/.gain_tmp/.trim_tmp sidecar name becomes <token>',
    'matroska uid: in a Matroska file, the payload of Segment/Tracks/TrackEntry/TrackUID (drawn at random per file) '
    'becomes zero bytes, located by walking the EBML tree up to the first Cluster',
)


class PreconditionError(ValueError):
    pass


def digest(data):
    return hashlib.sha256(data).hexdigest()


def exe(build, name):
    return build / (name + ('.exe' if os.name == 'nt' else ''))


def files_under(directory):
    if not directory.is_dir():
        return {}
    return {p.relative_to(directory).as_posix(): p for p in sorted(directory.rglob('*')) if p.is_file()}


def prepare_fixtures(root, build, adm=(), sofa=(), text=()):
    """Write the inputs under root/fixtures: generated ADM fixtures, the JSON fixtures, the synthetic room pack,
    the named SOFA fixtures and non-WAVE text files."""
    fixtures = root / 'fixtures'
    for folder in ('adm', 'json', 'room', 'sofa', 'text'):
        (fixtures / folder).mkdir(parents=True)
    for name in sorted(set(adm)):
        result = subprocess.run([str(exe(build, 'mr_adm_make_fixture')), name, str(fixtures / 'adm' / (name + '.wav'))],
                                capture_output=True)
        if result.returncode:
            raise RuntimeError(f'fixture {name} failed: {result.stdout.decode(errors="replace")}'
                               f'{result.stderr.decode(errors="replace")}')
    common.write_json_fixtures(fixtures / 'json')
    for name in common.ROOM_FIXTURES:
        shutil.copyfile(common.ROOT / common.ROOM_FIXTURE_DIR / name, fixtures / 'room' / name)
    common.validate_room_fixtures(fixtures / 'room', common.ROOM_FIXTURES)
    for name in sorted(set(sofa)):
        if name not in common.SOFA_FIXTURES:
            raise ValueError('unknown SOFA fixture: ' + name)
        shutil.copyfile(common.ROOT / 'tests/fixtures/sofa' / name, fixtures / 'sofa' / name)
    for name in sorted(set(text)):
        (fixtures / 'text' / name).write_bytes(TEXT_FIXTURE)
    return {name: digest(p.read_bytes()) for name, p in files_under(fixtures).items()}


def check_record_fixtures(root, manifest):
    for name, sha in manifest['fixtures'].items():
        if digest((root / 'fixtures' / name).read_bytes()) != sha:
            raise ValueError('fixture integrity failure: ' + name)


def check_clock(manifest):
    clock = manifest['clock']
    if not (isinstance(clock['start'], int) and isinstance(clock['end'], int) and clock['start'] <= clock['end']):
        raise ValueError('invalid wall-clock window')
    if manifest['normalizations'] != list(NORMALIZATIONS):
        raise ValueError('record was made with other normalization rules')
    if not isinstance(manifest.get('root'), str) or not Path(manifest['root']).is_absolute():
        raise ValueError('missing original record root')


def utc(fields):
    try:
        return datetime.datetime(*(int(f) for f in fields), tzinfo=datetime.timezone.utc).timestamp()
    except ValueError:
        return None


def normalize_bext(data, inside):
    """Blank the bext origination date and time of a RIFF / RF64 / BW64 file when they fall inside the window."""
    if len(data) < 12 or data[:4] not in (b'RIFF', b'RF64', b'BW64') or data[8:12] != b'WAVE':
        return data
    offset, ds64_data = 12, None
    while offset + 8 <= len(data):
        chunk, size = data[offset:offset + 4], int.from_bytes(data[offset + 4:offset + 8], 'little')
        payload = offset + 8
        if chunk == b'ds64' and size >= 16 and payload + 16 <= len(data):
            ds64_data = int.from_bytes(data[payload + 8:payload + 16], 'little')
        if chunk == b'data' and size == 0xFFFFFFFF:
            if ds64_data is None:
                return data
            size = ds64_data  # 64-bit data size of RF64 / BW64
        if chunk == b'bext' and payload + 338 <= len(data):
            stamp = data[payload + 320:payload + 338]
            match = re.fullmatch(rb'(\d{4})-(\d{2})-(\d{2})(\d{2})-(\d{2})-(\d{2})', stamp)
            if match and inside(utc(match.groups())):
                return data[:payload + 320] + BEXT_CLOCK_PLACEHOLDER + data[payload + 338:]
            return data
        offset = payload + size + (size & 1)
    return data


def ebml_vint(data, offset, keep_marker):
    if offset >= len(data) or data[offset] == 0:
        return None, offset
    length = 9 - data[offset].bit_length()
    if offset + length > len(data):
        return None, offset
    value = int.from_bytes(data[offset:offset + length], 'big')
    if not keep_marker:
        value &= (1 << (7 * length)) - 1
        if value == (1 << (7 * length)) - 1:
            value = -1  # unknown size
    return value, offset + length


MATROSKA_PATH = (0x18538067, 0x1654AE6B, 0xAE)  # Segment, Tracks, TrackEntry
MATROSKA_TRACK_UID, MATROSKA_CLUSTER = 0x73C5, 0x1F43B675


def normalize_matroska(data):
    """Zero the per-file random TrackUID of a Matroska file (Segment/Tracks/TrackEntry/TrackUID)."""
    if not data.startswith(b'\x1a\x45\xdf\xa3'):
        return data
    out = bytearray(data)

    def walk(start, end, depth):
        offset = start
        while offset < end:
            element, cursor = ebml_vint(data, offset, True)
            size, payload = ebml_vint(data, cursor, False)
            if element is None or size is None or element == MATROSKA_CLUSTER:
                return
            stop = end if size == -1 else min(payload + size, end)
            if depth == len(MATROSKA_PATH) and element == MATROSKA_TRACK_UID:
                out[payload:stop] = bytes(stop - payload)
            elif depth < len(MATROSKA_PATH) and element == MATROSKA_PATH[depth]:
                walk(payload, stop, depth + 1)
            offset = stop
    walk(0, len(data), 0)
    return bytes(out)


def normalize(data, manifest, text=False):
    """Apply NORMALIZATIONS to one text stream or one file of a record."""
    root = manifest['root']
    for spelling in {root, root.replace('\\', '/'), root.replace('/', '\\')}:
        data = data.replace(spelling.encode('utf-8'), b'<root>')
    low, high = manifest['clock']['start'] - 1, manifest['clock']['end'] + 1

    def inside(value):
        return value is not None and low <= value <= high
    data = ISO_UTC.sub(lambda m: CLOCK_PLACEHOLDER if inside(utc(m.groups())) else m.group(0), data)
    if text:
        return SIDECAR.sub(rb'.\1.<token>.', data)
    return normalize_matroska(normalize_bext(data, inside))


def first_difference(x, y):
    for index, (p, q) in enumerate(zip(x, y)):
        if p != q:
            return index
    return min(len(x), len(y)) if len(x) != len(y) else None


def compare_files(manifest_a, manifest_b, files_a, files_b, label):
    """Compare two {name: path} inventories after normalization; return the difference rows."""
    rows = []
    for name in sorted(set(files_a) ^ set(files_b)):
        rows.append({'item': f'{label}/{name}', 'reference': name in files_a, 'candidate': name in files_b})
    for name in sorted(set(files_a) & set(files_b)):
        xa, xb = normalize(files_a[name].read_bytes(), manifest_a), normalize(files_b[name].read_bytes(), manifest_b)
        if xa != xb:
            rows.append({'item': f'{label}/{name}', 'first_difference': first_difference(xa, xb),
                         'reference_bytes': len(xa), 'candidate_bytes': len(xb)})
    return rows
