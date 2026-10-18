#!/usr/bin/env python3
"""SOFA preparation tests; optionally validate every node with the native checker."""
import importlib.util
import json
from pathlib import Path
import sys
import tempfile
import unittest

import h5py
import numpy as np

ROOT = Path(__file__).resolve().parents[2]
spec = importlib.util.spec_from_file_location("prepare_hrir", ROOT / "scripts/room/prepare-hrir.py")
prepare = importlib.util.module_from_spec(spec)
spec.loader.exec_module(prepare)
CHECKER = sys.argv.pop(1) if len(sys.argv) > 1 and not sys.argv[1].startswith("-") else None
DIRECTIONS = np.array([[1, 0, 0], [0, 1, 0], [-1, 0, 0], [0, -1, 0], [0, 0, 1], [0, 0, -1]], dtype=float)


def fixture(path, rate=48000, delays=None, reverse=False, rotation=False):
    ir = np.zeros((6, 2, 32))
    ir[:, 0, 2], ir[:, 1, 4] = .8, .6
    if reverse:
        ir = ir[:, ::-1].copy()
    with h5py.File(path, "w") as file:
        file.attrs["SOFAConventions"] = "SimpleFreeFieldHRIR"
        file.attrs["DataType"] = "FIR"
        file.create_dataset("Data.IR", data=ir)
        file.create_dataset("Data.SamplingRate", data=[rate]).attrs["Units"] = "hertz"
        source = DIRECTIONS.copy()
        if rotation:
            source = np.column_stack((-source[:, 1], source[:, 0], source[:, 2])) + [3, 2, 1]
            file.create_dataset("ListenerPosition", data=[[3, 2, 1]])
            file.create_dataset("ListenerView", data=[[0, 1, 0]])
            file.create_dataset("ListenerUp", data=[[0, 0, 1]])
        ds = file.create_dataset("SourcePosition", data=source)
        ds.attrs["Type"], ds.attrs["Units"] = "cartesian", "metre"
        ears = np.array([[0, .09, 0], [0, -.09, 0]])
        file.create_dataset("ReceiverPosition", data=(ears[::-1] if reverse else ears)[:, :, None])
        if delays is not None:
            file.create_dataset("Data.Delay", data=delays)
    return ir


def array(root, entry):
    return np.fromfile(root / entry["file"], dtype=entry["dtype"]).reshape(entry["shape"])


class HrirPreparation(unittest.TestCase):
    def setUp(self):
        self.directory = tempfile.TemporaryDirectory(prefix="mradm-hrir-prepare-")
        self.root = Path(self.directory.name)
        self.source, self.output = self.root / "input.sofa", self.root / "bank"

    def tearDown(self):
        self.directory.cleanup()

    def build(self, **kwargs):
        return prepare.prepare(self.source, self.output, checker=CHECKER, **kwargs)

    def test_nodes_keep_amplitude_tail_and_ear_order(self):
        original = fixture(self.source, reverse=True)
        original[:, 0, -1] = .125
        with h5py.File(self.source, "r+") as file:
            file["Data.IR"][...] = original
        manifest = self.build()
        actual = array(self.output, manifest["assets"]["impulses"])
        np.testing.assert_array_equal(actual, original[:, ::-1].astype(np.float32))
        self.assertEqual(manifest["preprocessing_guard_samples"], 0)
        self.assertEqual(manifest["preparation"]["source_ear_indices"], [1, 0])
        if CHECKER:
            report = json.loads((self.output / "node-validation.json").read_text())
            self.assertTrue(report["passed"])
            self.assertEqual(report["nodes"], 6)

    def test_listener_frame_and_original_mesh(self):
        fixture(self.source, rotation=True)
        faces = prepare.mesh(DIRECTIONS)
        path = self.root / "faces.json"
        path.write_text(json.dumps((faces + 1).tolist()))
        result = self.build(faces=path, index_base=1)
        np.testing.assert_allclose(array(self.output, result["assets"]["directions"]), DIRECTIONS, atol=1e-12)
        np.testing.assert_array_equal(array(self.output, result["assets"]["faces"]), faces)

    def test_fractional_negative_delay_and_full_resampling_support(self):
        delays = np.array([[-.25, 2.5]])
        fixture(self.source, rate=44100, delays=delays)
        manifest = self.build()
        actual = array(self.output, manifest["assets"]["impulses"])[0].astype(float)
        guard = manifest["preprocessing_guard_samples"]
        self.assertGreater(guard, 40)
        self.assertGreater(actual.shape[-1], int(np.ceil(32 * 48000 / 44100)))
        # First moments independently measure both causal guards and the declared
        # fractional delays, without comparing with the preparation convolution.
        measured = (actual * np.arange(actual.shape[-1])).sum(axis=-1) / actual.sum(axis=-1)
        expected = (np.array([2., 4.]) + delays[0]) * 48000 / 44100 + guard
        np.testing.assert_allclose(measured, expected, atol=.002, rtol=0)
        np.testing.assert_allclose(actual.sum(axis=-1), [.8, .6], atol=.0001, rtol=0)

    def test_integer_delay_keeps_exact_samples(self):
        fixture(self.source, delays=[[-2, 3]])
        result = self.build()
        actual = array(self.output, result["assets"]["impulses"])
        self.assertEqual(result["preprocessing_guard_samples"], 2)
        self.assertEqual(np.argmax(actual[0, 0]), 2)
        self.assertEqual(np.argmax(actual[0, 1]), 9)
        self.assertEqual(float(actual[0, 0].max()), float(np.float32(.8)))

    def test_seconds_delay_uses_source_clock_before_resampling(self):
        fixture(self.source, rate=96000, delays=[[-2/96000, 3/96000]])
        with h5py.File(self.source, "r+") as file:
            file["Data.Delay"].attrs["Units"] = "seconds"
        result = self.build()
        actual = array(self.output, result["assets"]["impulses"])[0].astype(float)
        measured = (actual * np.arange(actual.shape[-1])).sum(axis=-1) / actual.sum(axis=-1)
        expected = np.array([0., 3.5]) + result["preprocessing_guard_samples"]
        np.testing.assert_allclose(measured, expected, atol=.002, rtol=0)

    def test_spherical_general_fir(self):
        fixture(self.source)
        polar = np.column_stack((np.degrees(np.arctan2(DIRECTIONS[:, 1], DIRECTIONS[:, 0])),
                                 np.degrees(np.arcsin(DIRECTIONS[:, 2])), np.ones(6)))
        with h5py.File(self.source, "r+") as file:
            file.attrs["SOFAConventions"] = "GeneralFIR"
            file["SourcePosition"][...] = polar
            file["SourcePosition"].attrs["Type"] = "spherical"
            file["SourcePosition"].attrs["Units"] = "degree, degree, metre"
        result = self.build()
        np.testing.assert_allclose(array(self.output, result["assets"]["directions"]), DIRECTIONS, atol=1e-12)

    def test_invalid_sources_leave_no_partial_package(self):
        def complex_ir(file):
            data = file["Data.IR"][...].astype(complex) + .25j
            del file["Data.IR"]
            file.create_dataset("Data.IR", data=data)

        mutations = {
            "multiple radii": lambda f: f["SourcePosition"].__setitem__((0, 0), 2),
            "nonfinite PCM": lambda f: f["Data.IR"].__setitem__((0, 0, 0), np.nan),
            "complex PCM must not discard its imaginary component": complex_ir,
            "unsupported length unit must not be treated as metres":
                lambda f: f["SourcePosition"].attrs.__setitem__("Units", "centimetre"),
            "duplicate direction": lambda f: f["SourcePosition"].__setitem__(0, DIRECTIONS[1]),
            "ambiguous ears": lambda f: f["ReceiverPosition"].__setitem__((1, 1, 0), .08),
            "excessive support": lambda f: f.create_dataset("Data.Delay", data=[[-5000., 0]]),
        }
        for label, mutation in mutations.items():
            with self.subTest(label=label):
                fixture(self.source)
                with h5py.File(self.source, "r+") as file:
                    mutation(file)
                with self.assertRaises(ValueError):
                    self.build()
                self.assertFalse(self.output.exists())

    def test_insufficient_coverage_and_broken_mesh(self):
        upper = DIRECTIONS.copy()
        upper[:, 2] = np.abs(upper[:, 2]) + .2
        upper /= np.linalg.norm(upper, axis=1)[:, None]
        with self.assertRaises(ValueError):
            prepare.mesh(upper)
        path = self.root / "broken.json"
        path.write_text(json.dumps([[0, 1, 4]] * 8))
        with self.assertRaises(ValueError):
            prepare.mesh(DIRECTIONS, path)

    def test_output_is_never_overwritten(self):
        fixture(self.source)
        self.output.mkdir()
        (self.output / "keep").write_text("existing")
        with self.assertRaises(ValueError):
            self.build()
        self.assertEqual((self.output / "keep").read_text(), "existing")


if __name__ == "__main__":
    unittest.main()
