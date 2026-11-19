"""Shared parts of the boundary records (phase 6): the CLI transcript and the C ABI capture.

Both record one implementation of a public boundary on one build and compare two records byte for byte.
NORMALIZATIONS is the complete list of rewrites compare applies; every other byte is compared.
"""
import datetime
import hashlib
import os
from pathlib import PurePosixPath, PureWindowsPath
import re
import shutil
import subprocess

import collection as common

# Bytes of the non-WAVE input used by the error cases.
TEXT_FIXTURE = b'not a RIFF/RF64/BW64 file\n'
ISO_UTC = re.compile(rb'(\d{4})-(\d{2})-(\d{2})T(\d{2}):(\d{2}):(\d{2})Z')
CLOCK_PLACEHOLDER = b'0000-00-00T00:00:00Z'
BEXT_CLOCK_PLACEHOLDER = b'0000-00-0000-00-00'
SIDECAR = re.compile(rb'\.((?:render|gain|trim|output)_tmp)\.[0-9a-f]{16}(?=\.|_rendered_id_|\s|$)')
EARLY_ABSL_LOG = re.compile(rb'^([IWEF]0000 00:00:)([0-9]{1,12})\.[0-9]{6} +[0-9]+'
                            rb'(?= [^\r\n]+:[0-9]+\] )', re.MULTILINE)
IAMF_INFO_LOG = rb'I0000 00:00:<clock> <thread> '
IAMF_FIRST_DECODER = re.compile(rb'^(' + IAMF_INFO_LOG + rb'opus_decoder\.cc:[0-9]+\] Opus decoded '
                                rb'[0-9]+ samples per channel\. With )[0-9]+(?= channels\.\r?$)', re.MULTILINE)
IAMF_FRAME_LINE = IAMF_INFO_LOG + rb'audio_frame\.cc:[0-9]+\] '
IAMF_FRAME_LINES = re.compile(rb'(?:^' + IAMF_FRAME_LINE + rb'[^\r\n]*(?:\r?\n|$))+', re.MULTILINE)
IAMF_FRAME_START = re.compile(rb'^' + IAMF_FRAME_LINE + rb'  audio_substream_id= ', re.MULTILINE)
NORMALIZATIONS = (
    'root: the absolute capture working directory (as recorded, with / and \\ separators) becomes <root>',
    'clock: an ISO 8601 UTC time YYYY-MM-DDTHH:MM:SSZ inside the record\'s own wall-clock window '
    '(start - 1 s to end + 1 s) becomes 0000-00-00T00:00:00Z, same length',
    'bext clock: in a RIFF/RF64/BW64 file, the bext OriginationDate and OriginationTime (payload bytes 320-338) '
    'inside the same window become 0000-00-0000-00-00',
    'sidecar: in text, the random 16-hex token of a .render_tmp/.gain_tmp/.trim_tmp/.output_tmp name becomes <token>, '
    'including the IAMF bridge staging prefixes',
    'matroska uid: replace Segment/Tracks/TrackEntry/TrackUID with a one-byte zero and adjust the EBML sizes '
    'affected by the random UID width, preserving size padding and everything from the first Cluster onward',
    'mp4 clock: the creation and modification times in moov/mvhd, moov/trak/tkhd and moov/trak/mdia/mdhd '
    '(version 0 or 1, seconds since 1904) inside the same window become zero bytes; other fields and mdat stay intact',
    'gpac iamf name: in the sound handler name of moov/trak/mdia/hdlr, zero the random token of the IAMF sidecar '
    'copied by GPAC; keep the handler type and the rest of its name',
    'iamf diagnostic log: in early Abseil log headers, normalize in-window clocks and thread IDs; '
    'sort contiguous audio_frame.cc info records by complete substream record and normalize the arbitrary '
    'channel count in the first opus_decoder.cc info message',
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


def stage_room(source, destination):
    destination.mkdir(parents=True, exist_ok=True)
    for name in common.ROOM_FIXTURES:
        shutil.copyfile(source / name, destination / name)
    common.validate_room_fixtures(destination)


def prepare_fixtures(root, build, adm=(), sofa=(), text=()):
    """Write the inputs under root/fixtures: generated ADM fixtures, the JSON fixtures, the synthetic room pack,
    the named SOFA fixtures and non-WAVE text files."""
    fixtures = root / 'fixtures'
    for folder in ('adm', 'json', 'sofa', 'text'):
        (fixtures / folder).mkdir(parents=True)
    for name in sorted(set(adm)):
        result = subprocess.run([str(exe(build, 'mr_adm_make_fixture')), name, str(fixtures / 'adm' / (name + '.wav'))],
                                capture_output=True)
        if result.returncode:
            raise RuntimeError(f'fixture {name} failed: {result.stdout.decode(errors="replace")}'
                               f'{result.stderr.decode(errors="replace")}')
    common.write_json_fixtures(fixtures / 'json')
    stage_room(common.ROOT / common.ROOM_FIXTURE_DIR, fixtures / 'room')
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
    if not (type(clock['start']) is int and type(clock['end']) is int and clock['start'] <= clock['end']):
        raise ValueError('invalid wall-clock window')
    if manifest['normalizations'] != list(NORMALIZATIONS):
        raise ValueError('record was made with other normalization rules')
    path_type = PureWindowsPath if manifest['platform']['system'] == 'Windows' else PurePosixPath
    if not isinstance(manifest.get('root'), str) or not path_type(manifest['root']).is_absolute():
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


def ebml_size_width(size):
    # The all-ones value of a size VINT is reserved for unknown sizes.
    return ((size + 1).bit_length() + 6) // 7


def normalize_matroska(data):
    """Canonicalize TrackUID and the element sizes affected by its random width."""
    if not data.startswith(b'\x1a\x45\xdf\xa3'):
        return data
    edits = []

    def walk(start, end, depth):
        delta = 0
        offset = start
        while offset < end:
            element, size_at = ebml_vint(data, offset, True)
            if element is None or element == MATROSKA_CLUSTER:
                break
            size, payload = ebml_vint(data, size_at, False)
            if size is None or payload > end:
                break
            stop = end if size == -1 else payload + size
            if stop > end:
                break
            change = 0
            if depth == len(MATROSKA_PATH) and element == MATROSKA_TRACK_UID and 1 <= size <= 8:
                # Keep a TrackUID element so an absent element still compares differently.
                edits.append((payload, stop, b'\0'))
                change = 1 - size
            elif depth < len(MATROSKA_PATH) and element == MATROSKA_PATH[depth]:
                change = walk(payload, stop, depth + 1)
            if change and size != -1:
                # Remove any width change caused by the new size while preserving deliberate padding.
                old_width = payload - size_at
                padding = old_width - ebml_size_width(size)
                new_size = size + change
                width = ebml_size_width(new_size) + padding
                encoded = ((1 << (7 * width)) | new_size).to_bytes(width, 'big')
                edits.append((size_at, payload, encoded))
                change += width - old_width
            delta += change
            offset = stop
        return delta

    walk(0, len(data), 0)
    if not edits:
        return data
    out = bytearray(data)
    for start, stop, replacement in sorted(edits, reverse=True):
        out[start:stop] = replacement
    return bytes(out)


MP4_EPOCH_OFFSET = 2_082_844_800  # Seconds between 1904-01-01 and the Unix epoch.
MP4_CONTAINERS = {(): b'moov', (b'moov',): b'trak', (b'moov', b'trak'): b'mdia'}
MP4_CLOCKS = {(b'moov',): b'mvhd', (b'moov', b'trak'): b'tkhd', (b'moov', b'trak', b'mdia'): b'mdhd'}
GPAC_IAMF_NAME = re.compile(rb'(?:[^/\\\x00]+\.)?render_tmp\.([0-9a-f]{16})\.iamf@GPAC[^\x00]*\x00')


def normalize_mp4(data, inside):
    """Normalize in-window full-box clocks and the GPAC audio handler's IAMF sidecar token."""
    if len(data) < 8 or data[4:8] != b'ftyp':
        return data
    out = bytearray(data)

    def walk(start, end, path):
        offset = start
        while offset + 8 <= end:
            size = int.from_bytes(data[offset:offset + 4], 'big')
            tag, payload = data[offset + 4:offset + 8], offset + 8
            if size == 1:
                if payload + 8 > end:
                    return
                size = int.from_bytes(data[payload:payload + 8], 'big')
                payload += 8
            elif size == 0:
                size = end - offset
            stop = offset + size
            if stop > end or stop < payload:
                return
            if tag == MP4_CONTAINERS.get(path):
                walk(payload, stop, (*path, tag))
            elif tag == MP4_CLOCKS.get(path) and payload + 4 <= stop:
                width = {0: 4, 1: 8}.get(data[payload])
                if width is not None and payload + 4 + 2 * width <= stop:
                    for field in (payload + 4, payload + 4 + width):
                        stamp = int.from_bytes(data[field:field + width], 'big') - MP4_EPOCH_OFFSET
                        if inside(stamp):
                            out[field:field + width] = bytes(width)
            elif tag == b'hdlr' and path == (b'moov', b'trak', b'mdia'):
                name_at = payload + 24
                if name_at < stop and data[payload] == 0 and data[payload + 8:payload + 12] == b'soun':
                    match = GPAC_IAMF_NAME.fullmatch(data[name_at:stop])
                    if match:
                        out[name_at + match.start(1):name_at + match.end(1)] = b'0' * 16
            offset = stop

    walk(0, len(data), ())
    return bytes(out)


def normalize_iamf_diagnostics(data, inside):
    def log_prefix(match):
        return match[1] + b'<clock> <thread>' if inside(int(match[2])) else match[0]

    def sort_substreams(match):
        block = match[0]
        starts = [m.start() for m in IAMF_FRAME_START.finditer(block)]
        if not starts or starts[0] != 0:
            return block
        return b''.join(sorted(block[start:stop] for start, stop in zip(starts, starts[1:] + [len(block)])))

    data = EARLY_ABSL_LOG.sub(log_prefix, data)
    data = IAMF_FIRST_DECODER.sub(rb'\1<channels>', data)
    return IAMF_FRAME_LINES.sub(sort_substreams, data)


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
        return normalize_iamf_diagnostics(SIDECAR.sub(rb'.\1.<token>', data), inside)
    return normalize_mp4(normalize_matroska(normalize_bext(data, inside)), inside)


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
