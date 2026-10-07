#!/usr/bin/env python3
"""Evidence corruption must fail independently of numerical gate policy."""
import importlib.util
import copy
import fnmatch
import json
import os
from pathlib import Path
import struct
import sys
import tempfile
import unittest
import argparse
import subprocess
from unittest.mock import patch

OPTIONS = None

ROOT = Path(__file__).resolve().parents[2]
sys.path.insert(0, str(ROOT / 'scripts/consistency'))
import phase2_common as common
comparator = common.load_module('phase2_comparator', ROOT / 'scripts/consistency/compare-rust-phase2.py')
collector = common.load_module('phase2_collector', ROOT / 'scripts/consistency/run-rust-phase2.py')


class CollectionTests(unittest.TestCase):
    def setUp(self):
        temporary = tempfile.TemporaryDirectory()
        self.addCleanup(temporary.cleanup)
        self.root = Path(temporary.name)
        self.build = self.root / 'build'; self.build.mkdir()
        self.source = common.source_fingerprint()
        self.cache = {'CMAKE_BUILD_TYPE': 'Release', 'MR_ADM_ENABLE_IAMF': 'OFF', 'MR_ADM_ENABLE_SOFA': 'OFF',
                      'MR_ADM_FLAC_PROVIDER': 'VENDORED', 'MR_ADM_OPUS_PROVIDER': 'VENDORED',
                      'MR_ADM_CORE_USE_INSTALLED_DEPS': 'OFF', 'MR_ADM_STRICT_FP': 'OFF',
                      'MR_ADM_CONSISTENCY_DIAGNOSTICS': 'OFF'}
        self.configure()
        common.save(self.build / 'consistency-dependencies.json', {'version': 1, 'dependencies': []})
        common.save(self.build / 'rust-dependencies.json', {'packages': [], 'resolve': {'nodes': []}})
        self.binaries = {}
        for name in ('mradm', 'mr_adm_make_fixture', 'mr_adm_pcm_bits', 'mr_adm_phase2_render',
                     'mr_adm_phase2_scene', 'mr_adm_phase2_kernels', 'mr_adm_repeat_render'):
            path = self.build / (name + ('.exe' if os.name == 'nt' else ''))
            path.write_bytes(b'unchanged executable')
            self.binaries[name] = path
        common.stamp_build(self.build)

    def configure(self):
        (self.build / 'CMakeCache.txt').write_text(''.join(f'{k}:STRING={v}\n' for k, v in self.cache.items()))
        strict = ' -fno-fast-math -ffp-contract=off' if self.cache['MR_ADM_STRICT_FP'] == 'ON' else ''
        common.save(self.build / 'compile_commands.json', [{'file': 'probe.cpp', 'command': 'c++ -O3' + strict}])

    def assert_collection_rejected(self, message, *arguments):
        output = self.root / 'output'
        with patch.object(sys, 'argv', ['collector', str(self.build), str(output), *arguments]), \
                patch.object(collector, 'run') as run, patch.object(collector, 'collect') as collect:
            with self.assertRaisesRegex(ValueError, message):
                collector.main()
            run.assert_not_called()
            collect.assert_not_called()
        self.assertFalse(output.exists(), 'preflight failure must not leave an incomplete collection')

    def test_configure_only_change_cannot_relabel_old_binaries(self):
        common.validate_build_stamp(self.build, self.source, self.binaries)
        self.cache['MR_ADM_STRICT_FP'] = 'ON'
        self.configure()  # CMake configuration changed; no executable was rebuilt.
        self.assert_collection_rejected('stale.*configuration', '--config', 'b')

    def test_changed_commands_and_dependency_records_invalidate_stamp(self):
        changes = {
            'compile_commands.json': [{'file': 'probe.cpp', 'command': 'c++ -O3 -march=native'}],
            'consistency-dependencies.json': {'version': 1, 'dependencies': [{'name': 'fmt', 'provider': 'system'}]},
            'rust-dependencies.json': {'packages': [], 'resolve': {'nodes': [{'features': ['diagnostics']}]}},
        }
        for name, value in changes.items():
            with self.subTest(file=name):
                path = self.build / name
                original = path.read_bytes()
                common.save(path, value)
                with self.assertRaisesRegex(ValueError, 'stale.*configuration'):
                    common.validate_build_stamp(self.build, self.source, self.binaries)
                path.write_bytes(original)

    def test_legacy_stamp_requires_rebuild(self):
        path = self.build / 'phase2-build-stamp.json'
        stamp = json.loads(path.read_text())
        del stamp['configuration']
        common.save(path, stamp)
        self.assert_collection_rejected('stale.*configuration', '--config', 'a')

    def test_noninterference_requires_plain_baseline_before_collection(self):
        self.cache['MR_ADM_CONSISTENCY_DIAGNOSTICS'] = 'ON'
        self.configure()
        common.stamp_build(self.build)
        root = self.root / 'baseline'
        baseline = {'schema': 'mradm.phase2.v1', 'complete': True, 'source': self.source, 'config': 'a',
                    'diagnostics': False, 'build': {'cmake.MR_ADM_CONSISTENCY_DIAGNOSTICS': 'OFF'}}
        common.save(root / 'manifest.json', baseline)
        self.assertEqual(collector.load_uninstrumented_baseline(root, self.source, 'a'), baseline)
        for diagnostics, flag in ((True, 'ON'), (True, 'OFF'), (False, 'ON'), (None, 'OFF'), (False, None)):
            with self.subTest(diagnostics=diagnostics, build_flag=flag):
                baseline['diagnostics'] = diagnostics
                baseline['build']['cmake.MR_ADM_CONSISTENCY_DIAGNOSTICS'] = flag
                common.save(root / 'manifest.json', baseline)
                self.assert_collection_rejected('uninstrumented baseline', '--config', 'a', '--baseline', str(root))


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
        self.assertEqual(len(scene), 58)
        self.assertTrue(any('--loudness-target' in c['args'] for c in offline))
        self.assertTrue(any('triple-balance' in c['args'] for c in offline))
        for row in scene:
            positions = [event['sample'] for event in row['metadata']]
            self.assertEqual(positions, sorted(set(positions)))
            self.assertTrue(all(0 < n <= 1024 for n in row['partition']))
            self.assertEqual(row['epochs'][1], {'epoch': 2, 'target': 257, 'end': 290})
        self.assertEqual({(r['input_rate'], r['output_rate']) for r in scene},
                         {(48000, 48000), (48000, 44100), (44100, 48000),
                          (96000, 96000), (48000, 96000), (96000, 48000)})
        for rate in ((96000, 96000), (48000, 96000), (96000, 48000)):
            self.assertEqual({tuple(r['partition']) for r in scene
                              if (r['input_rate'], r['output_rate']) == rate},
                             {(512,), (1, 7, 127, 511, 1024)})

    def test_original_matrix_parameters_remain_frozen(self):
        closeout = json.loads((ROOT / 'docs/architecture/evidence/rust-phase2/closeout.json').read_text())
        original_scene = [row for row in common.scene_cases()
                          if row['version'] == 1 and max(row['input_rate'], row['output_rate']) <= 48000]
        self.assertEqual(len(original_scene), closeout['inventory']['scene_configurations'])
        self.assertEqual(common.json_digest(common.offline_cases()),
                         closeout['inventory']['offline_parameters_sha256'])
        self.assertEqual(common.json_digest(original_scene),
                         closeout['inventory']['scene_parameters_sha256'])

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

    def test_fft_simd_features_are_rejected(self):
        scalar = {'rust.fft_features': json.dumps({'rustfft': [], 'realfft': []})}
        self.assertEqual(comparator.fft_features(scalar), {'rustfft': [], 'realfft': []})
        for features in ({'rustfft': ['avx', 'neon', 'sse'], 'realfft': []},
                         {'rustfft': [], 'realfft': ['default']},
                         {'rustfft': []}):
            with self.subTest(features=features), self.assertRaises(ValueError):
                comparator.fft_features({'rust.fft_features': json.dumps(features)})
        with self.assertRaises(ValueError):
            comparator.fft_features({})

    def test_kernel_inventory_matches_probe_sizes(self):
        names = common.kernel_outputs()
        self.assertEqual(len(names), len(set(names)))
        self.assertIn('fft-twiddles.20-table.f32', names)
        self.assertIn('spreader.40-output.f32', names)
        self.assertEqual(len(names), 133)
        for rates in common.RESAMPLER_RATES:
            self.assertIn(f'resampler-{rates[0]}-{rates[1]}.20-output.f32', names)
        for name in common.OM_EDGE_CASES:
            self.assertIn(f'om-edge-{name}.40-residual.f32', names)
        for size in common.HRTF_PROBE_SIZES:
            self.assertIn(f'hrtf-{size}.20-grid-weights.f32', names)
            self.assertIn(f'hrtf-{size}.50-continuous.c32', names)
        for size in common.FFT_SIZES:
            self.assertIn(f'fft-{size}.20-spectrum.c32', names)
        self.assertTrue(all(Path(name).suffix in common.FORMATS for name in names))

    def test_gates_fail_on_difference_or_empty_match(self):
        def pair(kernel_identical, case_identical):
            return {'platforms': ['a', 'b'],
                    'kernels': [{'checkpoint': 'fft-256.20-spectrum.c32', 'identical': kernel_identical},
                                {'checkpoint': 'fft-twiddles.10-libm.f64', 'identical': False}],
                    'cases': {'binaural-point': {'identical': case_identical}}}
        gates = [{'kind': 'kernel', 'pattern': 'fft-[0-9]*', 'reason': 'r'},
                 {'kind': 'case', 'pattern': 'binaural-*', 'reason': 'r'}]
        self.assertEqual(comparator.gate_failures({'pairs': [pair(True, True)]}, gates), [])
        failures = comparator.gate_failures({'pairs': [pair(False, True)]}, gates)
        self.assertEqual([f['name'] for f in failures], ['fft-256.20-spectrum.c32'])
        failures = comparator.gate_failures({'pairs': [pair(True, False)]}, gates)
        self.assertEqual([f['name'] for f in failures], ['binaural-point'])
        failures = comparator.gate_failures({'pairs': [pair(True, True)]},
                                            [{'kind': 'kernel', 'pattern': 'missing-*', 'reason': 'r'}])
        self.assertEqual(failures, [{'pattern': 'missing-*', 'name': None, 'platforms': None}])

    def test_committed_gates_are_valid(self):
        gates = comparator.load_gates(comparator.DEFAULT_GATES)
        names = common.kernel_outputs()
        for gate in gates:
            if gate['kind'] == 'kernel':
                self.assertTrue(any(fnmatch.fnmatchcase(n, gate['pattern']) for n in names), gate['pattern'])
            else:
                cases = [c['id'] for c in common.offline_cases()]
                cases += [c['id'] + f'-epoch{e}' for c in common.scene_cases() for e in (1, 2)]
                self.assertTrue(any(fnmatch.fnmatchcase(n, gate['pattern']) for n in cases), gate['pattern'])
        # Every PCM case is bit-identical on all three platforms (ADR 0017): each case is gated by
        # exact id, so a new case must join the gates (or be argued out here) in the same change.
        case_gates = [g['pattern'] for g in gates if g['kind'] == 'case']
        self.assertEqual(len(case_gates), len(set(case_gates)))
        cases = [c['id'] for c in common.offline_cases()]
        cases += [c['id'] + f'-epoch{e}' for c in common.scene_cases() for e in (1, 2)]
        self.assertEqual(sorted(case_gates), sorted(cases))
        for bad in ({'schema': 'x', 'gates': []}, {'schema': 'mradm.phase2.gates.v1', 'gates': [{'kind': 'kernel'}]}):
            with self.subTest(bad=bad), tempfile.TemporaryDirectory() as tmp:
                path = Path(tmp) / 'gates.json'
                path.write_text(json.dumps(bad), encoding='utf-8')
                with self.assertRaises(ValueError):
                    comparator.load_gates(path)

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
    def run_probe(self, case, success, no_artifacts=False):
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
                if no_artifacts:
                    self.assertFalse((tmp / 'out').exists(), 'invalid script must not be recorded as evidence')

    def test_fence_timeout_clock_injection_and_recovery(self):
        if OPTIONS.scene is None:
            self.skipTest('native replay executable not provided')
        result = subprocess.run([str(OPTIONS.scene), '--self-test'], capture_output=True, text=True, timeout=15)
        self.assertEqual(result.returncode, 0, result.stderr)

    def test_seek_short_tail_and_no_underrun(self):
        self.run_probe(common.scene_cases()[1], True)

    def test_every_catalog_script_is_supported(self):
        for case in common.scene_cases():
            with self.subTest(case=case['id']):
                self.run_probe(case, True)

    def test_unexecuted_script_changes_are_rejected(self):
        original = common.scene_cases()[0]
        changes = [
            ('version', 2), ('clock', 'wall clock'), ('state_complete_on_every_frame', False),
            ('controls', {}), ('epochs', [{'epoch': 1, 'target': 0, 'end': 2048}]),
            ('signal', dict(original['signal'], seed=original['signal']['seed'] + 1)),
            ('signal', dict(original['signal'], scale=0.25)),
            ('signal', dict(original['signal'], generator='different-generator')),
            ('output_controls', {'0': 'volume=0.5'}), ('backend', 'unknown'),
        ]
        for field, value in changes:
            with self.subTest(field=field, value=value):
                case = copy.deepcopy(original)
                case[field] = value
                self.run_probe(case, False, no_artifacts=True)
        case = copy.deepcopy(original)
        del case['controls']
        self.run_probe(case, False, no_artifacts=True)

    def test_unexecuted_device_controls_are_rejected(self):
        case = next(row for row in common.scene_cases() if row['device_dsp'])
        case['output_controls']['2048'] = 'HpTF bypass'
        self.run_probe(case, False, no_artifacts=True)

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
