#!/usr/bin/env python3
"""Exercise the real PCM CLI and baseline scripts with small protocol fixtures."""
import argparse
import shlex
import shutil
import struct
import subprocess
import sys
import tempfile
import unittest
from pathlib import Path


OPTIONS = None
REPO = Path(__file__).resolve().parents[2]


def image_bytes(bits, channels=1, rate=48000, frames=None):
    if frames is None:
        frames = len(bits) // channels
    return struct.pack('<4sIIIQ', b'MRPB', 1, channels, rate, frames) + b''.join(
        struct.pack('<I', value) for value in bits
    )


def wav_bytes(bits):
    payload = b''.join(struct.pack('<I', value) for value in bits)
    return struct.pack(
        '<4sI4s4sIHHIIHH4sI', b'RIFF', 36 + len(payload), b'WAVE', b'fmt ',
        16, 3, 1, 48000, 192000, 4, 32, b'data', len(payload)
    ) + payload


class Harness(unittest.TestCase):
    def setUp(self):
        self.temp = tempfile.TemporaryDirectory(prefix='mradm-consistency-test-')
        self.addCleanup(self.temp.cleanup)
        self.root = Path(self.temp.name).resolve()
        self.tool = Path(OPTIONS.pcm_bits).resolve()

    def write(self, name, data):
        path = self.root / name
        path.write_bytes(data)
        return path

    def invoke(self, *args):
        return subprocess.run([str(self.tool), *map(str, args)], capture_output=True, text=True)

    def assert_status(self, result, status):
        self.assertEqual(result.returncode, status, result.stdout + result.stderr)


class ToolTests(Harness):
    def test_identical_and_one_ulp(self):
        a = self.write('a.pcmbits', image_bytes([0x3F800000]))
        b = self.write('b.pcmbits', image_bytes([0x3F800001]))
        self.assert_status(self.invoke('compare', a, a), 0)
        result = self.invoke('compare', a, b)
        self.assert_status(result, 1)
        self.assertRegex(result.stdout, r'max_ulp=1\b')

    def test_ulp_order_across_zero_and_negative_neighbours(self):
        for left, right, distance in [
            (0x80000000, 0, 1),
            (0x80000001, 1, 3),
            (0xBF800001, 0xBF800000, 1),
        ]:
            with self.subTest(left=left, right=right):
                a = self.write('a.pcmbits', image_bytes([left]))
                b = self.write('b.pcmbits', image_bytes([right]))
                result = self.invoke('compare', a, b)
                self.assert_status(result, 1)
                self.assertRegex(result.stdout, rf'max_ulp={distance}\b')

    def test_extraction_preserves_signed_zero_and_subnormal_bits(self):
        bits = [0, 0x80000000, 1, 0x80000001, 0x3F800000]
        wav = self.write('input.wav', wav_bytes(bits))
        out = self.root / 'out.pcmbits'
        self.assert_status(self.invoke('extract', wav, out), 0)
        self.assertEqual(out.read_bytes(), image_bytes(bits))
        self.assert_status(self.invoke('validate', out), 0)
        self.assert_status(self.invoke('validate', wav), 2)

    def test_nonfinite_rejected_in_both_input_forms(self):
        for bits in [0x7FC00000, 0xFFC00000, 0x7F800000, 0xFF800000]:
            with self.subTest(bits=bits):
                img = self.write('bad.pcmbits', image_bytes([bits]))
                wav = self.write('bad.wav', wav_bytes([bits]))
                self.assert_status(self.invoke('validate', img), 2)
                self.assert_status(self.invoke('compare', img, img), 2)
                self.assert_status(self.invoke('extract', wav, self.root / 'out.pcmbits'), 2)

    def test_bad_headers_and_io_are_errors(self):
        cases = [
            b'MRPB',
            image_bytes([], channels=0, frames=0),
            image_bytes([0], rate=0),
            image_bytes([], channels=2, frames=1 << 63),
            image_bytes([0], frames=2),
            image_bytes([0]) + b'x',
            b'MRPB' + struct.pack('<I', 99) + image_bytes([0])[8:],
        ]
        for data in cases:
            with self.subTest(data=data):
                img = self.write('bad.pcmbits', data)
                self.assert_status(self.invoke('validate', img), 2)
        self.assert_status(self.invoke('compare', self.root / 'missing', self.root / 'missing'), 2)
        wav = self.write('input.wav', wav_bytes([0]))
        self.assert_status(self.invoke('extract', wav, self.root / 'missing' / 'out.pcmbits'), 2)

    def test_shape_difference_is_reported(self):
        a = self.write('a.pcmbits', image_bytes([0], rate=48000))
        b = self.write('b.pcmbits', image_bytes([0], rate=44100))
        result = self.invoke('compare', a, b)
        self.assert_status(result, 1)
        self.assertIn('shape mismatch', result.stdout)


class ScriptTests(Harness):
    def setUp(self):
        super().setUp()
        if not OPTIONS.bash:
            self.skipTest('Bash unavailable; PCM tool checks still run')
        self.bash = Path(OPTIONS.bash).resolve().as_posix()
        self.script = self.root / 'compare-platforms.sh'
        shutil.copyfile(REPO / 'scripts/consistency/compare-platforms.sh', self.script)
        self.gates = self.root / 'expected-identical.txt'
        self.gates.write_text('# Initially ungated.\n', encoding='utf-8')
        self.dirs = [self.root / name for name in ['one', 'two', 'three']]
        for d in self.dirs:
            (d / 'fixtures').mkdir(parents=True)
            (d / 'pcm').mkdir()
            (d / 'cases.txt').write_text('probe\n', encoding='utf-8')
            (d / 'fixtures/input.wav').write_bytes(wav_bytes([0x3E800000]))
            (d / 'pcm/probe.pcmbits').write_bytes(image_bytes([0x3E800000]))

    def command(self, tool=None, expected=None):
        flag = ['--expected', Path(expected).as_posix()] if expected else []
        return [self.bash, self.script.as_posix(), *flag, (tool or self.tool).as_posix(),
                *[d.as_posix() for d in self.dirs]]

    def compare(self, tool=None, expected=None):
        return subprocess.run(self.command(tool, expected), capture_output=True, text=True)

    def test_complete_and_ungated_different_baselines(self):
        self.assert_status(self.compare(), 0)
        (self.dirs[0] / 'pcm/probe.pcmbits').write_bytes(image_bytes([0x3F000000]))
        result = self.compare()
        self.assert_status(result, 0)
        self.assertIn('differing: 1', result.stdout)
        self.assertIn('two vs three: identical', result.stdout)

    def test_missing_output_is_always_an_error(self):
        (self.dirs[1] / 'pcm/probe.pcmbits').unlink()
        self.assert_status(self.compare(), 2)

    def test_mismatched_cases_are_an_error(self):
        (self.dirs[1] / 'cases.txt').write_text('other\n', encoding='utf-8')
        self.assert_status(self.compare(), 2)

    def test_empty_and_duplicate_cases_are_errors(self):
        for text in ['', 'probe\nprobe\n', '../probe\n']:
            with self.subTest(text=text):
                (self.dirs[0] / 'cases.txt').write_text(text, encoding='utf-8')
                self.assert_status(self.compare(), 2)

    def test_fixture_inventories_and_contents_must_match(self):
        extra = self.dirs[1] / 'fixtures/extra.wav'
        extra.write_bytes(wav_bytes([0]))
        self.assert_status(self.compare(), 2)
        extra.unlink()
        (self.dirs[1] / 'fixtures/input.wav').write_bytes(wav_bytes([0x3F000000]))
        self.assert_status(self.compare(), 1)

    def test_invalid_images_cannot_pass_as_identical(self):
        for d in self.dirs:
            (d / 'pcm/probe.pcmbits').write_bytes(image_bytes([0x7FC00000]))
        self.assert_status(self.compare(), 2)

    def test_missing_and_failing_comparators_are_errors(self):
        self.assert_status(self.compare(self.root / 'missing-tool'), 2)
        tool = self.write('failing-tool.sh', b'#!/usr/bin/env bash\nif [ "$1" = validate ]; then exit 0; fi\nexit 2\n')
        tool.chmod(0o755)
        result = self.compare(tool)
        self.assert_status(result, 2)
        self.assertIn('comparison tool failed with status 2', result.stderr)

    def test_missing_gate_configuration_is_an_error(self):
        self.gates.unlink()
        self.assert_status(self.compare(), 2)

    def test_gate_regression_propagates_through_tee(self):
        self.gates.write_text('probe\n', encoding='utf-8')
        self.assert_status(self.compare(), 0)
        (self.dirs[1] / 'pcm/probe.pcmbits').write_bytes(image_bytes([0x3F000000]))
        self.assert_status(self.compare(), 1)
        report = self.root / 'report.txt'
        command = shlex.join(self.command()) + ' 2>&1 | tee ' + shlex.quote(report.as_posix())
        result = subprocess.run([self.bash, '--noprofile', '--norc', '-eo', 'pipefail', '-c', command],
                                capture_output=True, text=True)
        self.assert_status(result, 1)
        self.assertIn('REGRESSED', report.read_text(encoding='utf-8'))

    def test_unknown_gated_case_is_an_error(self):
        self.gates.write_text('unknown\n', encoding='utf-8')
        self.assert_status(self.compare(), 2)

    def test_alternate_gate_list_replaces_the_default(self):
        # The controlled build (roadmap config B) gates on its own list, so --expected must both
        # take effect and leave the default list unused.
        self.gates.write_text('probe\n', encoding='utf-8')
        other = self.root / 'expected-identical-controlled.txt'
        other.write_text('# Ungated.\n', encoding='utf-8')
        (self.dirs[1] / 'pcm/probe.pcmbits').write_bytes(image_bytes([0x3F000000]))
        self.assert_status(self.compare(), 1)
        result = self.compare(expected=other)
        self.assert_status(result, 0)
        self.assertIn('expected-identical-controlled.txt', result.stdout)

    def test_alternate_gate_list_is_enforced_and_must_exist(self):
        other = self.root / 'expected-identical-controlled.txt'
        other.write_text('probe\n', encoding='utf-8')
        self.assert_status(self.compare(expected=other), 0)
        (self.dirs[1] / 'pcm/probe.pcmbits').write_bytes(image_bytes([0x3F000000]))
        self.assert_status(self.compare(expected=other), 1)
        other.unlink()
        self.assert_status(self.compare(expected=other), 2)


if __name__ == '__main__':
    parser = argparse.ArgumentParser()
    parser.add_argument('--pcm-bits', required=True)
    parser.add_argument('--bash', default=shutil.which('bash'))
    OPTIONS, remaining = parser.parse_known_args()
    unittest.main(argv=[sys.argv[0], *remaining])
