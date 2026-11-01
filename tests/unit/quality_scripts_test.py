#!/usr/bin/env python3
"""Regressions for FFI contracts, optional reference analysis, retirement and the C/C++ surface ratchet."""

from contextlib import redirect_stderr, redirect_stdout
import hashlib
import importlib.util
import io
import json
import os
from pathlib import Path
import shutil
import subprocess
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
surface = load_check("language-surface")


@unittest.skipIf(sys.platform == "win32", "Quality shell scripts run on the macOS quality host")
class OptionalReferenceAnalysisTests(unittest.TestCase):
    def setUp(self):
        temporary = tempfile.TemporaryDirectory(prefix="mradm-quality-scope-")
        self.addCleanup(temporary.cleanup)
        self.root = Path(temporary.name)
        self.build = self.root / "build/debug"
        self.build.mkdir(parents=True)
        (self.build / "compile_commands.json").write_text("[]", encoding="utf-8")
        self.log = self.root / "tools.jsonl"
        self.bin = self.root / "bin"
        self.bin.mkdir()
        for name in ("clang-format", "clang-tidy", "cppcheck"):
            tool = self.bin / name
            tool.write_text(
                f"#!{sys.executable}\n"
                "import json, os, sys\n"
                "with open(os.environ['QUALITY_TEST_LOG'], 'a', encoding='utf-8') as log:\n"
                "    log.write(json.dumps(sys.argv) + '\\n')\n", encoding="utf-8",
            )
            tool.chmod(0o755)
        self.env = {
            **os.environ, "PATH": str(self.bin) + os.pathsep + os.environ["PATH"],
            "CLANG_FORMAT": str(self.bin / "clang-format"), "CLANG_TIDY": str(self.bin / "clang-tidy"),
            "QUALITY_TEST_LOG": str(self.log),
        }
        self.git("init", "-q")
        (self.root / "README.md").write_text("Quality scope fixture.\n", encoding="utf-8")
        self.git("add", "README.md")
        self.git("commit", "-qm", "baseline")
        self.sources = (
            "src/adm_core/module.cpp", "tests/reference/libadm/reference_test.cpp",
            "tests/support/scene_json/scene_json.cpp", "tests/support/unconditional.cpp",
        )
        for name in self.sources:
            path = self.root / name
            path.parent.mkdir(parents=True, exist_ok=True)
            path.write_text("// Analysis fixture.\n", encoding="utf-8")
        for name in ("clang-tidy.sh", "check-changed.sh", "cppcheck.sh"):
            path = self.root / "scripts/quality" / name
            path.parent.mkdir(parents=True, exist_ok=True)
            shutil.copyfile(REPO / "scripts/quality" / name, path)
        (self.root / "include").mkdir()
        self.git("add", ".")
        self.git("commit", "-qm", "candidate")

    def git(self, *args):
        subprocess.run(["git", "-c", "user.name=Quality test", "-c", "user.email=test@example.invalid",
                        "-c", "commit.gpgsign=false", "-c", f"core.hooksPath={self.root / 'no-hooks'}",
                        *args], cwd=self.root, check=True, capture_output=True, text=True)

    def run_quality(self, script, enabled):
        (self.build / "CMakeCache.txt").write_text(
            f"MR_ADM_BUILD_LIBADM_REFERENCE_TESTS:BOOL={'ON' if enabled else 'OFF'}\n", encoding="utf-8",
        )
        self.log.unlink(missing_ok=True)
        args = ["--base", "HEAD^", "--build-dir", str(self.build)] if script == "check-changed.sh" else [str(self.build)]
        result = subprocess.run(["bash", str(self.root / "scripts/quality" / script), *args],
                                cwd=self.root, env=self.env, capture_output=True, text=True)
        self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
        return {Path(call[0]).name: call[1:] for call in
                (json.loads(line) for line in self.log.read_text(encoding="utf-8").splitlines())}

    def test_full_tidy_analyzes_reference_sources_only_when_enabled(self):
        for enabled in (False, True):
            with self.subTest(enabled=enabled):
                args = self.run_quality("clang-tidy.sh", enabled)["clang-tidy"]
                for index, name in enumerate(self.sources):
                    self.assertEqual(str(self.root / name) in args, enabled or index in (0, 3), name)

    def test_changed_checks_keep_formatting_optional_reference_sources(self):
        for enabled in (False, True):
            with self.subTest(enabled=enabled):
                calls = self.run_quality("check-changed.sh", enabled)
                for index, name in enumerate(self.sources):
                    self.assertIn(str(self.root / name), calls["clang-format"])
                    for tool in ("clang-tidy", "cppcheck"):
                        self.assertEqual(str(self.root / name) in calls[tool], enabled or index in (0, 3), name)

    def test_full_cppcheck_excludes_disabled_reference_support(self):
        for enabled in (False, True):
            with self.subTest(enabled=enabled):
                args = self.run_quality("cppcheck.sh", enabled)["cppcheck"]
                excludes = [args[index + 1] for index, arg in enumerate(args[:-1]) if arg == "-i"]
                for name in ("tests/reference/libadm", "tests/support/scene_json"):
                    self.assertEqual(str(self.root / name) in excludes, not enabled, name)


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


class LanguageSurfaceTests(unittest.TestCase):
    def setUp(self):
        self.temporary = tempfile.TemporaryDirectory()
        self.root = Path(self.temporary.name)
        self.addCleanup(self.temporary.cleanup)
        self.write("src/adm_core/a.cpp", 10)
        self.write("include/adm/a.h", 2)
        self.write("src/adm_apple/b.mm", 4)
        self.write("src/adm_render_room/cuda/backend.cpp", 4)
        self.write("tests/unit/c.cpp", 7)
        self.write("tests/reference/hoa/d.h", 3)
        self.write("rust/crates/mradm-dsp/src/lib.rs", 5)
        self.baseline = self.root / "baseline.json"
        self.run_surface("--update")

    def write(self, relative, count):
        path = self.root / relative
        path.parent.mkdir(parents=True, exist_ok=True)
        path.write_text("x\n" * count, encoding="utf-8")

    def run_surface(self, *args):
        output = io.StringIO()
        with redirect_stdout(output), redirect_stderr(output):
            result = surface.main(["--root", str(self.root), "--baseline", str(self.baseline), *args])
        return result, output.getvalue()

    def test_areas_separate_production_islands_tests_and_rust(self):
        areas = surface.measure(self.root)["areas"]
        self.assertEqual(areas["production"]["lines"], 12)
        self.assertEqual(areas["islands"]["lines"], 4)
        self.assertEqual(areas["cuda"]["lines"], 4)
        self.assertEqual(areas["tests"]["lines"], 7)
        self.assertEqual(areas["reference"]["lines"], 3)
        self.assertEqual(areas["rust"]["lines"], 5)

    def test_unchanged_surface_passes(self):
        result, output = self.run_surface("--check")
        self.assertEqual(result, 0, output)

    def test_growing_production_fails_and_names_the_module(self):
        self.write("src/adm_engine/new.cpp", 3)
        result, output = self.run_surface("--check")
        self.assertEqual(result, 1)
        self.assertIn("src/adm_engine +3", output)

    def test_growing_islands_fails(self):
        self.write("src/adm_windows/sink.cpp", 1)
        result, output = self.run_surface("--check")
        self.assertEqual(result, 1)
        self.assertIn("islands", output)

    def test_cuda_growth_is_rejected_independently(self):
        self.write("src/adm_render_room/cuda/new.cpp", 1)
        result, output = self.run_surface("--check")
        self.assertEqual(result, 1, output)
        self.assertIn("cuda", output)

    def test_cuda_classification_does_not_cover_neighboring_modules(self):
        self.write("src/adm_render_room/cuda_other/new.cpp", 1)
        result, output = self.run_surface("--check")
        self.assertEqual(result, 1, output)
        self.assertIn("production", output)

    def test_cuda_shrink_must_tighten_its_own_limit(self):
        self.write("src/adm_render_room/cuda/backend.cpp", 3)
        self.assertEqual(self.run_surface("--check")[0], 1)
        self.assertEqual(self.run_surface("--update")[0], 0)
        self.assertEqual(self.run_surface("--check")[0], 0)
        self.write("src/adm_render_room/cuda/backend.cpp", 4)
        self.assertEqual(self.run_surface("--check")[0], 1)

    def test_tests_and_rust_are_reported_only(self):
        self.write("tests/unit/more.cpp", 50)
        self.write("rust/crates/mradm-dsp/src/more.rs", 50)
        result, output = self.run_surface("--check")
        self.assertEqual(result, 0, output)

    def test_shrinking_production_requires_tightening_before_check_passes(self):
        self.write("src/adm_core/a.cpp", 4)
        result, output = self.run_surface("--check")
        self.assertEqual(result, 1, output)
        self.assertIn("--update", output)
        self.assertEqual(self.run_surface("--update")[0], 0)
        self.assertEqual(self.run_surface("--check")[0], 0)
        self.write("src/adm_core/a.cpp", 5)
        self.assertEqual(self.run_surface("--check")[0], 1)

    def test_update_cannot_raise_existing_limits_or_overwrite_on_failure(self):
        original = self.baseline.read_bytes()
        for relative in ("src/adm_core/new.cpp", "src/adm_windows/new.cpp", "src/adm_render_room/cuda/new.cpp"):
            with self.subTest(path=relative):
                self.write(relative, 1)
                result, output = self.run_surface("--update")
                self.assertEqual(result, 1, output)
                self.assertEqual(self.baseline.read_bytes(), original)
                (self.root / relative).unlink()

    def git(self, *args):
        return subprocess.run(["git", "-c", "user.name=Surface test", "-c", "user.email=test@example.invalid",
                               "-c", "commit.gpgsign=false", "-c", f"core.hooksPath={self.root / 'no-hooks'}",
                               *args], cwd=self.root, check=True, capture_output=True, text=True).stdout.strip()

    def commit_baseline(self):
        self.git("init", "-q")
        self.git("add", ".")
        self.git("commit", "-qm", "baseline")
        return self.git("rev-parse", "HEAD")

    def test_base_ref_rejects_manually_raised_limits(self):
        ref = self.commit_baseline()
        for relative in ("src/adm_core/new.cpp", "src/adm_windows/new.cpp", "src/adm_render_room/cuda/new.cpp"):
            with self.subTest(path=relative):
                self.write(relative, 1)
                self.baseline.write_text(json.dumps(surface.measure(self.root)))
                for mode in ("--check", "--update"):
                    result, output = self.run_surface(mode, "--base-ref", ref)
                    self.assertEqual(result, 1, output)
                    self.assertIn("基线上限", output)
                (self.root / relative).unlink()

    def test_base_ref_allows_tightening_and_first_introduction(self):
        self.baseline.unlink()
        ref = self.commit_baseline()
        self.assertEqual(self.run_surface("--update", "--base-ref", ref)[0], 0)
        self.assertEqual(self.run_surface("--check", "--base-ref", ref)[0], 0)
        self.git("add", ".")
        self.git("commit", "-qm", "first baseline")
        ref = self.git("rev-parse", "HEAD")
        self.write("src/adm_core/a.cpp", 4)
        self.assertEqual(self.run_surface("--update", "--base-ref", ref)[0], 0)
        self.assertEqual(self.run_surface("--check", "--base-ref", ref)[0], 0)

    def test_base_ref_must_exist(self):
        self.commit_baseline()
        result, output = self.run_surface("--check", "--base-ref", "missing-ref")
        self.assertEqual(result, 1, output)
        self.assertIn("无法读取基准提交", output)

    def test_legacy_baseline_seeds_only_the_fixed_cuda_ceiling(self):
        legacy = json.loads(self.baseline.read_text())
        del legacy["areas"]["cuda"]
        self.baseline.write_text(json.dumps(legacy))
        ref = self.commit_baseline()
        self.write("src/adm_render_room/cuda/backend.cpp", surface.LEGACY_CUDA_LIMIT)
        self.assertEqual(self.run_surface("--update", "--base-ref", ref)[0], 0)
        self.assertEqual(self.run_surface("--check", "--base-ref", ref)[0], 0)
        self.write("src/adm_render_room/cuda/backend.cpp", surface.LEGACY_CUDA_LIMIT + 1)
        self.baseline.write_text(json.dumps(surface.measure(self.root)))
        for mode in ("--check", "--update"):
            result, output = self.run_surface(mode, "--base-ref", ref)
            self.assertEqual(result, 1, output)
            self.assertIn("基线上限", output)


if __name__ == "__main__":
    unittest.main()
