#!/usr/bin/env python3
"""Regressions for private FFI array contracts and reference retirement."""

from contextlib import redirect_stderr, redirect_stdout
import hashlib
import importlib.util
import io
import json
from pathlib import Path
import sys
import tempfile
import unittest
from unittest.mock import patch


REPO = Path(__file__).resolve().parents[2]


def load_check(name):
    spec = importlib.util.spec_from_file_location(name, REPO / "scripts/quality" / f"{name}.py")
    module = importlib.util.module_from_spec(spec)
    sys.modules[name] = module
    spec.loader.exec_module(module)
    return module


ffi = load_check("check-ffi-headers")
retention = load_check("check-reference-retention")


class FfiArrayTests(unittest.TestCase):
    def compare(self, array_declaration):
        hand = ffi.Header({}, {}, {}, {})
        generated = ffi.Header({}, {}, {}, {})
        ffi.parse_header(
            "int mradm_dsp_tb_filter_process(void* p, const float* src, size_t n, "
            "float* out, size_t len, char* message, size_t capacity);",
            "hand.h", hand,
        )
        ffi.parse_header(
            "typedef struct Decorrelator Decorrelator;\n" + array_declaration + "\n"
            "int32_t mradm_dsp_tb_filter_process(Decorrelator* p, const float* src, size_t n, "
            "FilteredFrame* out, size_t len, uint8_t* message, size_t capacity);",
            "generated.h", generated,
        )
        checker = ffi.Checker(hand, generated)
        checker.run()
        return checker.errors

    def test_four_float_frames_match(self):
        self.assertEqual(self.compare("typedef float FilteredFrame[4];"), [])

    def test_changed_frame_lengths_are_rejected(self):
        for length in (3, 8):
            with self.subTest(length=length):
                errors = self.compare(f"typedef float FilteredFrame[{length}];")
                self.assertTrue(errors)
                self.assertIn("FilteredFrame", "\n".join(errors))

    def test_changed_frame_elements_are_rejected(self):
        for scalar in ("double", "int32_t"):
            with self.subTest(scalar=scalar):
                errors = self.compare(f"typedef {scalar} FilteredFrame[4];")
                self.assertTrue(errors)
                self.assertIn("FilteredFrame", "\n".join(errors))

    def test_missing_array_definition_is_rejected(self):
        self.assertTrue(self.compare(""))


class ReferenceRetirementTests(unittest.TestCase):
    def setUp(self):
        temp = tempfile.TemporaryDirectory(prefix="mradm-quality-test-")
        self.addCleanup(temp.cleanup)
        self.root = Path(temp.name)
        self.reference = self.root / "tests/reference"
        self.provenance = "tests/reference/monitor/provenance.json"
        self.doc = "docs/migration.md"
        files = {
            "tests/reference/monitor/legacy.h": b"// Frozen implementation.\n",
            self.provenance: b'{"baseline": "1234567"}\n',
        }
        for relative, data in {
            **files,
            self.doc: b"Migration acceptance and retirement evidence.\n",
            "docs/policy.md": b"Reference retention policy.\n",
            "CMakeLists.txt": b"add_test(NAME monitor_reference_test COMMAND monitor_reference_test)\n",
        }.items():
            path = self.root / relative
            path.parent.mkdir(parents=True, exist_ok=True)
            path.write_bytes(data)
        self.unit = {
            "id": "monitor", "kind": "frozen", "status": "retained", "accepted": "2026-10-06",
            "migration_doc": self.doc, "provenance": self.provenance,
            "tests": ["monitor_reference_test"], "depends_on": [], "open_conditions": ["released"],
            "files": {name: hashlib.sha256(data).hexdigest() for name, data in files.items()},
        }
        self.registry = {
            "schema": "mradm.reference-retention.v1", "policy": "docs/policy.md",
            "conditions": {"released": "The implementation has shipped."}, "units": [self.unit],
        }

    def check(self):
        registry_path = self.reference / "retention.json"
        registry_path.write_text(json.dumps(self.registry), encoding="utf-8")
        output = io.StringIO()
        with patch.multiple(retention, REPO_ROOT=self.root, REFERENCE_ROOT=self.reference, REGISTRY=registry_path):
            with redirect_stdout(output), redirect_stderr(output):
                result = retention.main()
        return result, output.getvalue()

    def retire(self):
        for relative in self.unit["files"]:
            (self.root / relative).unlink()
        self.unit.update(status="retired", retired_in="1234567", files={}, open_conditions=[])
        (self.root / "CMakeLists.txt").write_text("", encoding="utf-8")

    def test_retained_unit_passes(self):
        result, output = self.check()
        self.assertEqual(result, 0, output)

    def test_retained_unit_still_requires_provenance(self):
        (self.root / self.provenance).unlink()
        result, output = self.check()
        self.assertEqual(result, 1)
        self.assertIn("provenance", output)

    def test_documented_retirement_keeps_historical_provenance_path(self):
        self.retire()
        result, output = self.check()
        self.assertEqual(result, 0, output)
        self.assertEqual(self.unit["provenance"], self.provenance)

    def test_retired_unit_still_requires_migration_document(self):
        self.retire()
        (self.root / self.doc).unlink()
        result, output = self.check()
        self.assertEqual(result, 1)
        self.assertIn("migration_doc", output)

    def test_retired_unit_cannot_leave_unregistered_files(self):
        self.retire()
        (self.root / self.provenance).write_text("{}", encoding="utf-8")
        result, output = self.check()
        self.assertEqual(result, 1)
        self.assertIn(self.provenance, output)


if __name__ == "__main__":
    unittest.main()
