#!/usr/bin/env python3
"""FFT timing evidence validation and transactional reference preparation."""
import copy
import importlib.util
from pathlib import Path
import sys
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[2]
sys.path.insert(0, str(ROOT / 'scripts/consistency'))
spec = importlib.util.spec_from_file_location('fft_benchmark', ROOT / 'scripts/consistency/benchmark-fft.py')
benchmark = importlib.util.module_from_spec(spec)
spec.loader.exec_module(benchmark)


class BenchmarkTests(unittest.TestCase):
    def report(self):
        return {'schema': 'mradm.fft.benchmark.v1', 'measurements': [
            {'length': n, 'iterations': 20000000 // n, 'forward_ns': 10, 'inverse_ns': 11,
             'spectrum_fnv1a64': '17', 'inverse_fnv1a64': '19'} for n in benchmark.LENGTHS]}

    def test_complete_report(self):
        self.assertEqual(len(benchmark.measurements(self.report())), len(benchmark.LENGTHS))

    def test_missing_duplicate_or_reordered_lengths(self):
        for inventory in [[], [128] * 9, list(reversed(benchmark.LENGTHS))]:
            value = self.report()
            value['measurements'] = [dict(value['measurements'][0], length=n) for n in inventory]
            with self.assertRaises(ValueError):
                benchmark.measurements(value)

    def test_invalid_values_and_fingerprints(self):
        for field, bad in [('forward_ns', float('nan')), ('inverse_ns', float('inf')),
                           ('forward_ns', 0), ('inverse_ns', -1), ('iterations', 0),
                           ('spectrum_fnv1a64', '-1'), ('inverse_fnv1a64', str(2**64)),
                           ('inverse_fnv1a64', ''), ('spectrum_fnv1a64', None)]:
            value = copy.deepcopy(self.report())
            value['measurements'][0][field] = bad
            with self.assertRaises(ValueError):
                benchmark.measurements(value)

    def test_failure_restores_both_manifests(self):
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            (root / 'rust').mkdir()
            cargo = root / 'rust/Cargo.toml'
            lock = root / 'rust/Cargo.lock'
            original = b'[patch.crates-io]\r\n' + benchmark.PATCH + b'\r\n'
            cargo.write_bytes(original)
            lock.write_bytes(b'original lock\n')
            with self.assertRaisesRegex(RuntimeError, 'build failed'):
                with benchmark.unpatched_manifest(root):
                    self.assertNotIn(benchmark.PATCH, cargo.read_bytes())
                    lock.write_bytes(b'partially updated lock\n')
                    raise RuntimeError('build failed')
            self.assertEqual(cargo.read_bytes(), original)
            self.assertEqual(lock.read_bytes(), b'original lock\n')

    def test_unknown_patch_is_rejected_without_writes(self):
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            (root / 'rust').mkdir()
            for body in [b'', benchmark.PATCH + b'\n' + benchmark.PATCH]:
                (root / 'rust/Cargo.toml').write_bytes(body)
                (root / 'rust/Cargo.lock').write_bytes(b'lock')
                with self.assertRaises(ValueError):
                    with benchmark.unpatched_manifest(root):
                        self.fail('invalid manifest was accepted')
                self.assertEqual((root / 'rust/Cargo.toml').read_bytes(), body)
                self.assertEqual((root / 'rust/Cargo.lock').read_bytes(), b'lock')

    def test_reference_keeps_other_dependencies_locked(self):
        lock = 'version = 4\n[[package]]\nname = "rustfft"\nversion = "6.4.1"\n'
        lock += '[[package]]\nname = "realfft"\nversion = "3.5.0"\n'
        updated = lock.replace('version = "6.4.1"', 'version = "6.4.1"\nsource = "registry"')
        self.assertEqual(benchmark.other_packages(lock), benchmark.other_packages(updated))
        self.assertNotEqual(benchmark.other_packages(lock),
                            benchmark.other_packages(updated.replace('3.5.0', '3.5.1')))


if __name__ == '__main__':
    unittest.main()
