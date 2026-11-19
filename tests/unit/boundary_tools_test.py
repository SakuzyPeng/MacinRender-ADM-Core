#!/usr/bin/env python3
"""Unit tests of the CLI record (scripts/consistency/cli-transcript.py and boundary.py), restored for the phase 7 island slices."""
import contextlib
import copy
import io
import json
import os
from pathlib import Path
import re
import struct
import sys
import tempfile
import time
import unittest

ROOT = Path(__file__).resolve().parents[2]
sys.path.insert(0, str(ROOT / 'scripts/consistency'))
import boundary  # noqa: E402
import collection as common  # noqa: E402

cli = common.load_module('cli_transcript', ROOT / 'scripts/consistency/cli-transcript.py')

NOW = 1_800_000_000


def manifest(root='/records/a', start=NOW, end=NOW + 10):
    return {'root': root, 'clock': {'start': start, 'end': end}}


def iso(seconds):
    return time.strftime('%Y-%m-%dT%H:%M:%SZ', time.gmtime(seconds)).encode()


def chunk(tag, payload):
    return tag + struct.pack('<I', len(payload)) + payload + (b'\0' if len(payload) & 1 else b'')


def bext(seconds):
    payload = bytearray(602)
    stamp = time.strftime('%Y-%m-%d%H-%M-%S', time.gmtime(seconds)).encode()
    payload[320:338] = stamp
    return bytes(payload)


def riff(*chunks):
    body = b'WAVE' + b''.join(chunks)
    return b'RIFF' + struct.pack('<I', len(body)) + body


def rf64_with_trailing_bext(samples, seconds):
    ds64 = chunk(b'ds64', struct.pack('<QQQI', 0, len(samples), 0, 0))
    data = b'data' + struct.pack('<I', 0xFFFFFFFF) + samples + (b'\0' if len(samples) & 1 else b'')
    body = b'WAVE' + ds64 + chunk(b'fmt ', bytes(16)) + data + chunk(b'bext', bext(seconds))
    return b'RF64' + struct.pack('<I', 0xFFFFFFFF) + body


def ebml(element_id, payload, unknown=False, width=None):
    raw_id = element_id.to_bytes((element_id.bit_length() + 7) // 8, 'big')
    if unknown:
        return raw_id + b'\x01\xff\xff\xff\xff\xff\xff\xff' + payload
    if width is None:
        width = 1 if len(payload) < 127 else 2
    return raw_id + ((1 << (7 * width)) | len(payload)).to_bytes(width, 'big') + payload


def mp4_box(tag, payload, extended=False, to_end=False):
    if extended:
        return struct.pack('>I4sQ', 1, tag, len(payload) + 16) + payload
    return struct.pack('>I4s', 0 if to_end else len(payload) + 8, tag) + payload


def mp4_header(version, created, modified):
    width = 8 if version == 1 else 4
    return (bytes([version, 0, 0, 0]) + (created + boundary.MP4_EPOCH_OFFSET).to_bytes(width, 'big')
            + (modified + boundary.MP4_EPOCH_OFFSET).to_bytes(width, 'big') + b'other-header-fields')


class NormalizationTests(unittest.TestCase):
    def test_root_and_clock_only_inside_the_window(self):
        m = manifest()
        text = b'/records/a/out.wav ' + iso(NOW + 3) + b' ' + iso(NOW + 100) + b' ' + iso(NOW - 100)
        out = boundary.normalize(text, m)
        self.assertEqual(out, b'<root>/out.wav 0000-00-00T00:00:00Z ' + iso(NOW + 100) + b' ' + iso(NOW - 100))
        self.assertEqual(boundary.normalize(b'C:\\records\\a\\x', manifest('C:\\records\\a')), b'<root>\\x')
        self.assertEqual(boundary.normalize(b'2026-13-45T99:99:99Z', m), b'2026-13-45T99:99:99Z')

    def test_bext_clock_in_riff_and_after_rf64_data(self):
        m = manifest()
        inside = riff(chunk(b'fmt ', bytes(16)), chunk(b'bext', bext(NOW + 1)), chunk(b'data', b'\1\2\3'))
        out = boundary.normalize(inside, m)
        self.assertEqual(len(out), len(inside))
        self.assertIn(b'0000-00-0000-00-00', out)
        self.assertEqual(out.replace(b'0000-00-0000-00-00', b''), inside.replace(inside[inside.index(b'bext') + 328:
                                                                                         inside.index(b'bext') + 346], b''))
        outside = riff(chunk(b'bext', bext(NOW + 1000)))
        self.assertEqual(boundary.normalize(outside, m), outside)
        trailing = rf64_with_trailing_bext(b'\x55' * 7, NOW + 2)
        self.assertIn(b'0000-00-0000-00-00', boundary.normalize(trailing, m))
        # Only the bext fields move: a sample that happens to look like a bext stamp is kept.
        lookalike = time.strftime('%Y-%m-%d%H-%M-%S', time.gmtime(NOW + 2)).encode()
        samples = rf64_with_trailing_bext(lookalike, NOW + 2)
        self.assertEqual(boundary.normalize(samples, m).count(lookalike), 1)

    def test_sidecar_token_only_in_text(self):
        line = b'wrote out.render_tmp.0123456789abcdef.wav and out.trim_tmp.fedcba9876543210.wav'
        self.assertEqual(boundary.normalize(line, manifest(), text=True),
                         b'wrote out.render_tmp.<token>.wav and out.trim_tmp.<token>.wav')
        self.assertEqual(boundary.normalize(line, manifest()), line)
        self.assertEqual(boundary.normalize(b'out.bitdepth_tmp.0001.wav', manifest(), text=True), b'out.bitdepth_tmp.0001.wav')
        self.assertEqual(boundary.normalize(b'out.output_tmp.0123456789abcdef_rendered_id_42.wav', manifest(), text=True),
                         b'out.output_tmp.<token>_rendered_id_42.wav')

    def test_matroska_track_uid_only_in_the_track_entry(self):
        uid = bytes(range(1, 9))
        header = ebml(0x1A45DFA3, ebml(0x4282, b'matroska'))
        cluster = ebml(0x1F43B675, ebml(0xA3, b'\x73\xc5\x88' + uid))

        def matroska(track_uid):
            tracks = ebml(0x1654AE6B, ebml(0xAE, ebml(0xD7, b'\x01') + ebml(0x73C5, track_uid)))
            return header + ebml(0x18538067, ebml(0x1549A966, b'\x00') + tracks + cluster, unknown=True)

        self.assertEqual(boundary.normalize(matroska(uid), manifest()), matroska(b'\0'))
        self.assertEqual(boundary.normalize(b'not ebml' + uid, manifest()), b'not ebml' + uid)

    def test_matroska_uid_width_changes_the_parent_sizes(self):
        header = ebml(0x1A45DFA3, ebml(0x4282, b'matroska'))
        cluster = ebml(0x1F43B675, ebml(0xA3, b'audio'))

        def matroska(uid, entry_width=None):
            # The 7- and 8-byte UID payloads put the TrackEntry on either side of 127.
            track_number = ebml(0xD7, b'\x01')
            track_uid = ebml(0x73C5, uid) if uid is not None else b''
            entry = ebml(0xAE, track_number + ebml(0xEC, bytes(111)) + track_uid, width=entry_width)
            return header + ebml(0x18538067, ebml(0x1654AE6B, entry) + cluster)

        expected = matroska(b'\0')
        for uid in (b'\x11' * 7, b'\x22' * 8):
            self.assertEqual(boundary.normalize(matroska(uid), manifest()), expected)
        self.assertEqual(boundary.normalize(matroska(b'\x11' * 7, entry_width=2), manifest()),
                         matroska(b'\0', entry_width=2))
        self.assertNotEqual(boundary.normalize(matroska(None), manifest()), expected)

    def test_mp4_clocks_preserve_sample_data_and_other_header_fields(self):
        for version in (0, 1):
            with self.subTest(version=version):
                header = mp4_header(version, NOW + 1, NOW + 2)
                zeroed = header[:4] + bytes(16 if version == 1 else 8) + b'other-header-fields'

                def movie(fields):
                    return mp4_box(b'moov', mp4_box(b'mvhd', fields) + mp4_box(b'trak',
                                   mp4_box(b'tkhd', fields) + mp4_box(b'mdia', mp4_box(b'mdhd', fields))),
                                   extended=True)

                prefix = mp4_box(b'ftyp', b'M4A \0\0\0\0M4A ')
                # A whole movie tree embedded in mdat must stay byte-for-byte intact.
                samples = mp4_box(b'mdat', movie(header))
                data = prefix + samples + movie(header)
                self.assertEqual(boundary.normalize(data, manifest()), prefix + samples + movie(zeroed))
                changed_samples = prefix + samples[:-1] + b'!' + movie(header)
                self.assertNotEqual(boundary.normalize(data, manifest()),
                                    boundary.normalize(changed_samples, manifest()))

    def test_mp4_clock_window_versions_and_malformed_boxes(self):
        prefix = mp4_box(b'ftyp', b'M4A \0\0\0\0M4A ')
        mixed = mp4_header(0, NOW - 100, NOW + 3)
        expected = mixed[:8] + bytes(4) + mixed[12:]
        wrap = lambda fields: prefix + mp4_box(b'moov', mp4_box(b'mvhd', fields), to_end=True)
        self.assertEqual(boundary.normalize(wrap(mixed), manifest()), wrap(expected))
        for data in (wrap(mp4_header(1, NOW - 100, NOW + 100)),
                     wrap(mp4_header(2, NOW + 1, NOW + 2)),
                     wrap(mp4_header(1, NOW + 1, NOW + 2)[:12]),
                     prefix + struct.pack('>I4s', 4, b'moov'),
                     prefix + struct.pack('>I4s', 9999, b'moov'),
                     prefix + struct.pack('>I4s', 1, b'moov'),
                     prefix + mp4_box(b'mvhd', mp4_header(0, NOW + 1, NOW + 2))):
            with self.subTest(data=data):
                self.assertEqual(boundary.normalize(data, manifest()), data)

    def test_mp4_normalizes_the_gpac_iamf_sound_handler_token(self):
        prefix = mp4_box(b'ftyp', b'mp42\0\0\0\1iamf')
        name = b'render_tmp.0123456789abcdef.iamf@GPAC26.02\0'

        def handler(name, kind=b'soun'):
            return mp4_box(b'hdlr', bytes(8) + kind + bytes(12) + name)

        def movie(name):
            media = mp4_box(b'mdia', handler(name) + handler(name, b'vide'))
            return mp4_box(b'moov', mp4_box(b'trak', media) + handler(name))

        samples = mp4_box(b'mdat', handler(name))
        data = prefix + movie(name) + samples
        # The same text in a video handler, an unrelated box or the sample data is preserved.
        expected = data.replace(name, name.replace(b'0123456789abcdef', b'0' * 16), 1)
        self.assertEqual(boundary.normalize(data, manifest()), expected)

    def test_iamf_log_keeps_diagnostics_and_groups_each_substream(self):
        def log(stamp, thread, channels, substreams):
            prefix = f'I0000 00:00:{stamp}.123456 {thread} '.encode()
            summary = f'opus_decoder.cc:103] Opus decoded 960 samples per channel. With {channels} channels.'.encode()
            lines = [prefix + summary]
            for stream_id, size in substreams:
                lines.extend([prefix + f'audio_frame.cc:91]   audio_substream_id= {stream_id}'.encode(),
                              prefix + f'audio_frame.cc:98]   // size_of(audio_frame)= {size}'.encode()])
            return b'\n'.join(lines) + b'\n[warning] preserved CLI diagnostic\n'

        a = log(NOW, 1001, 2, [(0, 500), (1, 3)])
        b = log(NOW + 1, 2001, 1, [(1, 3), (0, 500)])
        normalized = boundary.normalize(a, manifest(), text=True)
        self.assertEqual(boundary.normalize(b, manifest(), text=True), normalized)
        self.assertIn(b'I0000 00:00:<clock> <thread> audio_frame.cc:98]   // size_of(audio_frame)= 500', normalized)
        self.assertIn(b'[warning] preserved CLI diagnostic', normalized)
        wrong_sizes = log(NOW + 1, 2001, 1, [(1, 500), (0, 3)])
        self.assertNotEqual(boundary.normalize(wrong_sizes, manifest(), text=True), normalized)
        stale = log(NOW - 100, 4001, 2, [(0, 500), (1, 3)])
        self.assertEqual(boundary.normalize(stale, manifest(), text=True), stale)


class RecordValidationTests(unittest.TestCase):
    def setUp(self):
        temporary = tempfile.TemporaryDirectory()
        self.addCleanup(temporary.cleanup)
        self.root = Path(temporary.name)

    def write_record(self, name, platform, original_root):
        folder = self.root / name
        streams = folder / 'commands'
        streams.mkdir(parents=True)
        stdout = original_root.encode() + b'\n'
        (streams / 'where.stdout').write_bytes(stdout)
        (streams / 'where.stderr').write_bytes(b'')
        empty = boundary.digest(b'')
        data = {
            **manifest(original_root),
            'schema': cli.SCHEMA, 'complete': True, 'normalizations': list(boundary.NORMALIZATIONS),
            'source': {'files': {}, 'sha256': common.json_digest({})}, 'table': empty,
            'platform': {'system': platform, 'machine': 'test'},
            'build_identity': {'configuration': empty, 'binaries': dict.fromkeys(cli.TOOLS, empty)},
            'tool_override': None, 'cli_sha256': empty, 'fixtures': {},
            'commands': {'where': {'class': 'contract', 'args': ['where'], 'exit_code': 0,
                                   'stdout': boundary.digest(stdout), 'stderr': empty, 'files': {}, 'removed_files': []}},
        }
        common.save(folder / 'manifest.json', data)
        return folder, data

    def test_record_paths_use_the_capture_platform(self):
        cases = [('Windows', r'C:\records\old', r'\\server\share\new'), ('Linux', '/records/old', '/records/new')]
        for platform, old, new in cases:
            with self.subTest(platform=platform):
                a, _ = self.write_record(platform + '-a', platform, old)
                b, _ = self.write_record(platform + '-b', platform, new)
                self.assertTrue(cli.compare(a, b)['identical'])

    def test_missing_record_fields_return_two(self):
        a, _ = self.write_record('a', 'Linux', '/records/a')
        b, valid = self.write_record('b', 'Linux', '/records/b')
        for fields in (('build_identity',), ('tool_override',), ('commands', 'where', 'exit_code')):
            with self.subTest(fields=fields):
                damaged = copy.deepcopy(valid)
                parent = damaged
                for field in fields[:-1]:
                    parent = parent[field]
                del parent[fields[-1]]
                common.save(b / 'manifest.json', damaged)
                stderr = io.StringIO()
                with contextlib.redirect_stderr(stderr):
                    self.assertEqual(cli.main(['compare', str(a), str(b)]), 2)
                self.assertIn('invalid record', stderr.getvalue())
                self.assertIn(fields[-1], stderr.getvalue())


@unittest.skipIf(os.name == 'nt', 'fake tools are POSIX shell scripts')
class CliTranscriptTests(unittest.TestCase):
    def setUp(self):
        temporary = tempfile.TemporaryDirectory()
        self.addCleanup(temporary.cleanup)
        self.root = Path(temporary.name)
        self.build = self.root / 'build'
        self.build.mkdir()
        (self.build / 'CMakeCache.txt').write_text('CMAKE_BUILD_TYPE:STRING=Release\n')
        self.tool(self.build / 'mr_adm_make_fixture', 'exit 0')
        self.tool(self.build / 'mradm', self.fake_cli())
        self.table = self.root / 'table.json'
        common.save(self.table, {'schema': cli.TABLE_SCHEMA, 'commands': [
            {'id': 'formats', 'args': ['formats']},
            {'id': 'write', 'args': ['render', '-o', '{out:out.txt}', '-i', '{json:policy-identity.json}']},
            {'id': 'help', 'args': ['--help'], 'class': 'help'},
            {'id': 'bad', 'args': ['render', '--nope'], 'class': 'parse-error'},
        ]})

    def tool(self, path, body):
        path.write_text('#!/bin/sh\n' + body + '\n')
        path.chmod(0o755)
        return path

    @staticmethod
    def fake_cli(formats='flac 8ch', help_text='usage', write='pcm', bad_code=106):
        return (f'case "$1" in\n'
                f'  formats) echo "{formats}";;\n'
                f'  --help) echo "{help_text}";;\n'
                f'  render) if [ "$2" = --nope ]; then echo "not expected" >&2; exit {bad_code}; fi;'
                f' printf "{write} $(date -u +%Y-%m-%dT%H:%M:%SZ)" > "$3"; echo "wrote $3.render_tmp.0123456789abcdef.wav";;\n'
                f'esac\n')

    def record(self, name, tool=None):
        return cli.record(self.build, self.root / name, tool, self.table)

    def test_same_cli_records_are_identical_after_normalization(self):
        self.record('a')
        time.sleep(1.1)
        self.record('b')
        report = cli.compare(self.root / 'a', self.root / 'b')
        self.assertTrue(report['identical'], report)
        self.assertNotEqual((self.root / 'a/files/write/outputs/write/out.txt').read_bytes(),
                            (self.root / 'b/files/write/outputs/write/out.txt').read_bytes())

    def test_contract_differences_and_text_only_classes(self):
        self.record('a')
        candidate = self.tool(self.root / 'candidate', self.fake_cli('flac 9ch', 'Usage:', 'pcm!', 2))
        self.record('b', candidate)
        report = cli.compare(self.root / 'a', self.root / 'b')
        items = {(row['command'], row['item']) for row in report['differences']}
        self.assertEqual(items, {('formats', 'stdout'), ('write', 'files/outputs/write/out.txt'), ('bad', 'exit_code')})
        self.assertEqual({(row['command'], row['item']) for row in report['informational']}, {('help', 'stdout')})
        self.assertEqual(cli.main(['compare', str(self.root / 'a'), str(self.root / 'b')]), 1)

    def test_files_outside_output_directory_and_intermediate_changes(self):
        common.save(self.table, {'schema': cli.TABLE_SCHEMA, 'commands': [
            {'id': name, 'args': [name]} for name in ('create', 'overwrite', 'remove')]})
        body = ('case "$1" in\n'
                'create) mkdir -p nested; echo initial > unexpected.wav; echo nested > nested/out.txt;;\n'
                'overwrite) echo replaced > unexpected.wav;;\n'
                'remove) rm unexpected.wav;;\n'
                'esac\n')
        self.tool(self.build / 'mradm', body)
        a = self.record('a')
        self.record('b')
        self.assertTrue(cli.compare(self.root / 'a', self.root / 'b')['identical'])
        self.assertEqual(set(a['commands']['create']['files']), {'unexpected.wav', 'nested/out.txt'})
        self.assertEqual((self.root / 'a/files/create/unexpected.wav').read_bytes(), b'initial\n')
        self.assertEqual((self.root / 'a/files/overwrite/unexpected.wav').read_bytes(), b'replaced\n')
        self.assertEqual(a['commands']['remove']['removed_files'], ['unexpected.wav'])
        candidate = self.tool(self.root / 'candidate', body.replace('initial', 'different').replace('rm unexpected.wav', ':'))
        self.record('c', candidate)
        report = cli.compare(self.root / 'a', self.root / 'c')
        self.assertEqual({(row['command'], row['item']) for row in report['differences']},
                         {('create', 'files/unexpected.wav'), ('remove', 'removed_files')})

    def test_unexpected_working_directory_file_is_a_difference(self):
        self.record('a')
        candidate = self.tool(self.root / 'candidate', 'echo leftover > unexpected.wav\n' + self.fake_cli())
        self.record('b', candidate)
        report = cli.compare(self.root / 'a', self.root / 'b')
        self.assertFalse(report['identical'])
        self.assertIn(('formats', 'files/unexpected.wav'),
                      {(row['command'], row['item']) for row in report['differences']})
        (self.root / 'b/files/formats/unexpected.wav').write_bytes(b'tampered')
        with self.assertRaisesRegex(boundary.PreconditionError, 'output integrity'):
            cli.compare(self.root / 'a', self.root / 'b')

    def test_help_and_parse_errors_still_compare_file_effects(self):
        common.save(self.table, {'schema': cli.TABLE_SCHEMA, 'commands': [
            {'id': 'help', 'args': ['--help'], 'class': 'help'},
            {'id': 'bad', 'args': ['render', '--nope'], 'class': 'parse-error'},
        ]})
        self.record('a')
        candidate = self.tool(self.root / 'candidate',
                              'if [ "$1" = --help ]; then echo unexpected > outputs/help/out.wav; '
                              'else rm outputs/help/out.wav; echo unexpected > outputs/bad/out.wav; fi\n'
                              + self.fake_cli(help_text='Usage:'))
        self.record('b', candidate)
        report = cli.compare(self.root / 'a', self.root / 'b')
        self.assertFalse(report['identical'])
        self.assertEqual({(row['command'], row['item']) for row in report['differences']},
                         {('help', 'files/outputs/help/out.wav'), ('bad', 'files/outputs/bad/out.wav'),
                          ('bad', 'removed_files')})
        self.assertEqual({(row['command'], row['item']) for row in report['informational']}, {('help', 'stdout')})

    def test_preconditions(self):
        self.record('a')
        self.record('b')
        manifest_b = json.loads((self.root / 'b/manifest.json').read_text())
        manifest_b['cli_sha256'] = '0' * 64
        common.save(self.root / 'b/manifest.json', manifest_b)
        with self.assertRaisesRegex(boundary.PreconditionError, 'CLI digest'):
            cli.compare(self.root / 'a', self.root / 'b')
        (self.root / 'a/commands/formats.stdout').write_bytes(b'tampered')
        with self.assertRaisesRegex(boundary.PreconditionError, 'integrity'):
            cli.compare(self.root / 'a', self.root / 'b')
        self.assertEqual(cli.main(['compare', str(self.root / 'a'), str(self.root / 'b')]), 2)
        with self.assertRaisesRegex(ValueError, 'must be empty'):
            self.record('a')

    def test_expected_output_when_build_feature_is_available(self):
        common.save(self.table, {'schema': cli.TABLE_SCHEMA, 'commands': [
            {'id': 'formats', 'args': ['formats']},
            {'id': 'encode', 'args': ['render', '-o', '{out:out.mp4}'],
             'expected_output': 'out.mp4', 'output_requires_feature': 'iamf_mp4_packager'},
        ]})
        self.tool(self.build / 'mradm',
                  'case "$1" in formats) echo "Build features: iamf_mp4_packager=no";; render) exit 1;; esac')
        self.assertEqual(self.record('unsupported')['commands']['encode']['exit_code'], 1)

        for name, render in (('failed', 'exit 1'), ('empty', ': > "$3"'), ('success', 'echo media > "$3"')):
            with self.subTest(name=name):
                self.tool(self.build / 'mradm',
                          'case "$1" in formats) echo "Build features: iamf_mp4_packager=yes";; '
                          f'render) {render};; esac')
                if name == 'success':
                    self.assertIn('outputs/encode/out.mp4', self.record(name)['commands']['encode']['files'])
                else:
                    with self.assertRaisesRegex(ValueError, 'expected exit 0 and non-empty out.mp4'):
                        self.record(name)
                    self.assertFalse((self.root / name / 'manifest.json').exists())

    def test_table_validation(self):
        for commands, message in (([{'id': 'x', 'args': []}, {'id': 'x', 'args': []}], 'duplicate'),
                                  ([{'id': 'X', 'args': []}], 'invalid command id'),
                                  ([{'id': 'x', 'args': [], 'class': 'loose'}], 'unknown class'),
                                  ([{'id': 'x', 'args': [], 'expected_output': 'out.mp4'}], 'expected_output'),
                                  ([{'id': 'x', 'args': ['{out:out.mp4}'], 'expected_output': 'out.mp4',
                                     'output_requires_feature': 'iamf_mp4_packager'}], 'earlier formats command'),
                                  ([{'id': 'x', 'args': ['{json:nope.json}']}], 'unknown JSON fixture'),
                                  ([{'id': 'x', 'args': ['{room:nope}']}], 'unknown room file')):
            common.save(self.table, {'schema': cli.TABLE_SCHEMA, 'commands': commands})
            with self.subTest(message=message), self.assertRaisesRegex(ValueError, message):
                cli.load_table(self.table)


class CommandTableTests(unittest.TestCase):
    def test_table_covers_every_subcommand(self):
        table = cli.load_table()
        first = {row['args'][0] for row in table['commands'] if row['args']}
        public = {'render', 'inspect', 'backends', 'layouts', 'input-layouts', 'formats', 'export', 'room-check'}
        self.assertLessEqual(public | {'__apac-encode'}, first)
        # The subcommands of the Rust command line's option table.
        declared = set(re.findall(r'Sub \{\s*name: "([^"]+)"', (ROOT / 'rust/crates/mradm-cli/src/args.rs').read_text(
            encoding='utf-8')))
        self.assertEqual(declared, public | {'__apac-encode'})
        for name in public:
            rows = [row for row in table['commands'] if row['args'][:1] == [name]]
            with self.subTest(subcommand=name):
                self.assertTrue(any(row.get('class', 'contract') == 'contract' for row in rows))
                self.assertTrue(any(row.get('class') == 'parse-error' or 'missing' in row['id'] or 'unknown' in row['id']
                                    or 'bad' in row['id'] or 'extra' in row['id'] or 'invalid' in row['id']
                                    for row in rows) or name == 'backends')


if __name__ == '__main__':
    unittest.main()
