#!/usr/bin/env python3
"""Evidence corruption must fail independently of numerical gate policy."""
import importlib.util
import json
from pathlib import Path
import struct
import sys
import tempfile
import unittest
import argparse
import subprocess

OPTIONS = None

ROOT = Path(__file__).resolve().parents[2]
sys.path.insert(0, str(ROOT / 'scripts/consistency'))
import phase2_common as common
comparator = common.load_module('phase2_comparator', ROOT / 'scripts/consistency/compare-rust-phase2.py')


class EvidenceTests(unittest.TestCase):
    def test_signed_zero_subnormal_and_ulp(self):
        a = struct.pack('<III', 0x80000000, 1, 0x3f800000)
        b = struct.pack('<III', 0, 2, 0x3f800001)
        row = common.compare_words(a, b, '.f32')
        self.assertFalse(row['identical'])
        self.assertEqual(row['different_words'], 3)
        self.assertEqual(row['first_bits'], ['0x80000000', '0x00000000'])
        self.assertEqual(row['max_ulp'], 1)

    def test_double_is_never_narrowed(self):
        row = common.compare_words(struct.pack('<Q', 0x3ff0000000000000), struct.pack('<Q', 0x3ff0000000000001), '.f64')
        self.assertFalse(row['identical'])
        self.assertEqual(row['max_ulp'], 1)

    def test_corrupt_and_nonfinite_checkpoints(self):
        for data in (b'', b'123', struct.pack('<f', float('nan')), struct.pack('<f', float('inf'))):
            with self.subTest(data=data), self.assertRaises(ValueError):
                common.compare_words(data, data, '.f32')
        with self.assertRaises(ValueError):
            common.validate_words(struct.pack('<f', 1), '.c32')
        with self.assertRaises(ValueError):
            common.compare_words(struct.pack('<f', 1), struct.pack('<ff', 1, 1), '.f32')

    def test_pcm_shape_and_frame_location(self):
        with tempfile.TemporaryDirectory() as tmp:
            a, b = (Path(tmp) / x for x in ['a', 'b'])
            header = struct.pack('<4sIIIQ', b'MRPB', 1, 2, 48000, 2)
            a.write_bytes(header + struct.pack('<IIII', 0, 0, 0, 0))
            b.write_bytes(header + struct.pack('<IIII', 0, 0, 0, 1))
            row = common.compare_pcm(a, b)
            self.assertEqual((row['first_frame'], row['first_channel']), (1, 1))
            with self.assertRaises(ValueError):
                common.require_same(a, b)
            b.write_bytes(b'')
            with self.assertRaises(ValueError):
                common.pcm_bytes(b)

    def test_catalog_covers_current_backends_and_exact_requests(self):
        offline, scene = common.offline_cases(), common.scene_cases()
        self.assertEqual(len(offline), 34)
        self.assertEqual(len({r['id'] for r in offline}), 34)
        self.assertEqual(len(scene), 22)
        self.assertTrue(any('--loudness-target' in c['args'] for c in offline))
        self.assertTrue(any('triple-balance' in c['args'] for c in offline))
        for row in scene:
            positions = [event['sample'] for event in row['metadata']]
            self.assertEqual(positions, sorted(set(positions)))
            self.assertTrue(all(0 < n <= 1024 for n in row['partition']))
            self.assertEqual(row['epochs'][1], {'epoch': 2, 'target': 257, 'end': 290})

    def test_incomplete_and_path_escape_fail(self):
        with tempfile.TemporaryDirectory() as tmp:
            root = Path(tmp) / 'run'; root.mkdir()
            common.save(root / 'manifest.json', {'schema': 'mradm.phase2.v1', 'complete': False})
            with self.assertRaises(ValueError):
                comparator.validate(root)
            (Path(tmp) / 'external').write_bytes(b'1234')
            for name in ['missing', '../external']:
                with self.assertRaises(ValueError):
                    comparator.safe_file(root, name)

    def test_device_segments_require_complete_nonoverlapping_media(self):
        with tempfile.TemporaryDirectory() as tmp:
            root = Path(tmp); (root / 'device').mkdir()
            spec = {'device_dsp': True, 'input_rate': 48000, 'output_rate': 48000,
                    'epochs': [{'epoch': 1, 'target': 0, 'end': 3}]}
            for stage in ('70-before-hptf', '80-hptf', '90-output'):
                (root / 'device' / f'e1-s0.{stage}.f32').write_bytes(struct.pack('<ff', 0.125, -0.125))
                (root / 'device' / f'e1-s1.{stage}.f32').write_bytes(struct.pack('<ffff', 0.25, -0.25, 0.5, -0.5))
            common.canonicalize_device_trace(root, spec)
            self.assertEqual((root / 'device/e1.90-output.f32').read_bytes(), struct.pack('<ffffff', 0.125, -0.125, 0.25, -0.25, 0.5, -0.5))
            for p in (root / 'device').iterdir():
                p.unlink()
            for stage in ('70-before-hptf', '80-hptf', '90-output'):
                (root / 'device' / f'e1-s0.{stage}.f32').write_bytes(struct.pack('<ff', 1, 1))
                (root / 'device' / f'e1-s2.{stage}.f32').write_bytes(struct.pack('<ff', 1, 1))
            with self.assertRaisesRegex(ValueError, 'gap, overlap'):
                common.canonicalize_device_trace(root, spec)

    def test_actual_compiler_is_pinned(self):
        for version in ('unavailable', 'release: 1.97.0\ncommit-hash: x\nLLVM version: 22'):
            with self.assertRaises(ValueError):
                comparator.compiler_identity({'rust.compiler_verbose': version})
        result = comparator.compiler_identity({'rust.compiler_verbose': 'release: 1.98.0\ncommit-hash: abc\nLLVM version: 22.1.8'})
        self.assertEqual(result['commit-hash'], 'abc')

    def test_missing_and_tampered_worker_artifacts_fail(self):
        with tempfile.TemporaryDirectory() as tmp:
            root = Path(tmp)
            payload = struct.pack('<4sIIIQff', b'MRPB', 1, 2, 48000, 1, 0.0, 0.0)
            experiments = {}
            for name, workers, groups in [('w1-g4', 1, 4), ('w2-g4', 2, 4), ('w1-g1', 1, 1)]:
                (root / name).write_bytes(payload)
                row = {'workers': workers, 'group_budget': groups, 'actual_grouping': [['3', str(min(groups, 3))]] * 2, 'path': name}
                if name != 'w1-g4':
                    row['versus_w1_g4'] = common.compare_pcm(root / 'w1-g4', root / name)
                experiments[name] = row
            manifest = {'worker_experiments': experiments}
            comparator.validate_experiments(root, manifest)
            (root / 'w1-g1').unlink()
            with self.assertRaises(ValueError):
                comparator.validate_experiments(root, manifest)
            (root / 'w1-g1').write_bytes(payload[:-4] + struct.pack('<I', 1))
            with self.assertRaises(ValueError):
                comparator.validate_experiments(root, manifest)

    def test_dependency_order_precedes_filename_order(self):
        self.assertLess(comparator.checkpoint_order('scene/e1-s2048.20-effective.f32'), comparator.checkpoint_order('scene/e1-s0.40-render.f32'))
        self.assertLess(comparator.checkpoint_order('ear.02-direct.f64'), comparator.checkpoint_order('post/loudness.10-measurement.f64'))

    def test_topology_difference_is_not_equality(self):
        with tempfile.TemporaryDirectory() as tmp:
            a, b = (Path(tmp) / x for x in ['a', 'b']); a.mkdir(); b.mkdir()
            (a / 'v.20-output.f32').write_bytes(struct.pack('<f', 1))
            rows = comparator.checkpoints(a, b)
            self.assertEqual(rows[0]['status'], 'different-observation-topology')
            self.assertFalse(rows[0]['identical'])


class ReplayTests(unittest.TestCase):
    def run_probe(self, case, success):
        if OPTIONS.scene is None:
            self.skipTest('native replay executable not provided')
        with tempfile.TemporaryDirectory() as tmp:
            tmp = Path(tmp)
            common.save(tmp / 'case.json', case)
            result = subprocess.run([str(OPTIONS.scene), str(tmp / 'case.json'), str(tmp / 'out')],
                                    capture_output=True, text=True, timeout=30)
            self.assertEqual(result.returncode == 0, success, result.stderr)
            if success:
                for epoch in (1, 2):
                    common.require_same(tmp / 'out' / f'pass-1-epoch-{epoch}.pcmbits', tmp / 'out' / f'pass-2-epoch-{epoch}.pcmbits')
                self.assertEqual(json.loads((tmp / 'out/pass-1.json').read_text())['underruns'], 0)
            else:
                self.assertIn('replay', result.stderr)

    def test_fence_timeout_clock_injection_and_recovery(self):
        if OPTIONS.scene is None:
            self.skipTest('native replay executable not provided')
        result = subprocess.run([str(OPTIONS.scene), '--self-test'], capture_output=True, text=True, timeout=15)
        self.assertEqual(result.returncode, 0, result.stderr)

    def test_seek_short_tail_and_no_underrun(self):
        self.run_probe(common.scene_cases()[1], True)

    def test_active_intervals_cannot_pass_as_silence(self):
        case = common.scene_cases()[0]
        case['metadata'] = sorted([*case['metadata'], {'sample': 1536, 'field': 'gain', 'value': 0, 'ramp': 0}], key=lambda r: r['sample'])
        self.run_probe(case, False)

    def test_zero_partition_rejected(self):
        self.run_probe(dict(common.scene_cases()[0], partition=[0]), False)

    def test_out_of_order_events_rejected(self):
        case = common.scene_cases()[0]
        case['metadata'] = list(reversed(case['metadata']))
        self.run_probe(case, False)


if __name__ == '__main__':
    parser = argparse.ArgumentParser(add_help=False)
    parser.add_argument('--scene', type=Path)
    OPTIONS, rest = parser.parse_known_args()
    unittest.main(argv=[sys.argv[0], *rest])
