#!/usr/bin/env python3
"""Offline regressions for numeric CMake controls and build provenance."""
import argparse
import json
import shutil
import subprocess
import sys
import tempfile
import unittest
from pathlib import Path

REPO = Path(__file__).resolve().parents[2]
OPTIONS = None


def read_fields(path):
    return dict(line.split('=', 1) for line in path.read_text(encoding='utf-8').splitlines()
                if '=' in line and not line.startswith('#'))


class Harness(unittest.TestCase):
    def setUp(self):
        self.temp = tempfile.TemporaryDirectory(prefix='mradm-build-test-')
        self.addCleanup(self.temp.cleanup)
        self.root = Path(self.temp.name).resolve()

    def run_ok(self, args, cwd=None):
        result = subprocess.run(list(map(str, args)), cwd=cwd, capture_output=True, text=True)
        self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
        return result


class CMakeTests(Harness):
    def project(self, ear=False, flac=False):
        source = self.root / 'source with spaces'
        source.mkdir()
        text = ('cmake_minimum_required(VERSION 3.24)\nproject(probe LANGUAGES C CXX)\n'
                'set(CMAKE_EXPORT_COMPILE_COMMANDS ON)\n'
                f'list(APPEND CMAKE_MODULE_PATH "{(REPO / "cmake").as_posix()}")\n'
                'include(MRStrictFp)\n')
        if ear:
            fixture = source / 'ear fixture'
            (fixture / 'src').mkdir(parents=True)
            (fixture / 'CMakeLists.txt').write_text(
                'cmake_minimum_required(VERSION 3.5)\nproject(ear_fixture LANGUAGES CXX)\n'
                'option(EAR_SIMD "upstream default" ON)\nadd_subdirectory(src)\n')
            (fixture / 'src/CMakeLists.txt').write_text(
                'add_library(ear STATIC ear.cpp)\nif(EAR_SIMD)\n'
                'target_compile_definitions(ear PRIVATE XSIMD_ARCHS=xsimd::default_arch)\nelse()\n'
                'target_compile_definitions(ear PRIVATE XSIMD_ARCHS=xsimd::generic_for_dispatch)\nendif()\n')
            (fixture / 'src/ear.cpp').write_text('int ear_fixture() { return 0; }\n')
            # Stub unrelated packages so the real MRDependencies libear branch can run offline.
            targets = ('fmt::fmt spdlog::spdlog CLI11::CLI11 nlohmann_json::nlohmann_json '
                       'tl::expected ebur128 dr_wav::dr_wav FLAC::FLAC libbw64 adm saf '
                       'Opus::opus miniaudio SampleRate::samplerate')
            if flac:
                targets = targets.replace(' FLAC::FLAC', '')
                flac_dir = source / 'flac fixture'
                flac_dir.mkdir()
                (flac_dir / 'flac.c').write_text('int flac_fixture(void) { return 0; }\n')
                (flac_dir / 'CMakeLists.txt').write_text(
                    'add_library(FLAC STATIC flac.c)\nadd_library(FLAC::FLAC ALIAS FLAC)\n'
                    'if(MSVC)\ntarget_compile_options(FLAC PRIVATE /fp:fast)\nelse()\n'
                    'target_compile_options(FLAC PRIVATE -fassociative-math -fno-signed-zeros '
                    '-fno-trapping-math -freciprocal-math)\nendif()\n')
                text += (f'set(FETCHCONTENT_SOURCE_DIR_FLAC "{flac_dir.as_posix()}")\n'
                         'set(MR_ADM_FLAC_PROVIDER VENDORED)\n')
            text += ('set(MR_ADM_CORE_FETCH_DEPS ON)\nset(MR_ADM_CORE_USE_INSTALLED_DEPS OFF)\n'
                     'set(FETCHCONTENT_FULLY_DISCONNECTED ON)\n'
                     f'set(FETCHCONTENT_SOURCE_DIR_LIBEAR "{fixture.as_posix()}")\n'
                     f'foreach(target IN ITEMS {targets})\n'
                     'add_library(${target} INTERFACE IMPORTED)\nendforeach()\n'
                     'include(MRDependencies)\n'
                     'get_target_property(mode ear COMPILE_DEFINITIONS)\n'
                     'file(WRITE "${CMAKE_BINARY_DIR}/ear-mode.txt" "${mode}")\n')
            if flac:
                text += ('get_target_property(flac_options FLAC COMPILE_OPTIONS)\n'
                         'file(WRITE "${CMAKE_BINARY_DIR}/flac-options.txt" "${flac_options}")\n')
        else:
            (source / 'probe.cpp').write_text(
                '#ifdef __FAST_MATH__\n#error strict FP left fast-math enabled\n#endif\n'
                'int main() { return 0; }\n')
            text += 'add_executable(probe probe.cpp)\n'
        (source / 'CMakeLists.txt').write_text(text)
        return source

    def configure(self, source, build, *flags, success=True, preset=None):
        args = [OPTIONS.cmake]
        if preset:
            args += ['--preset', preset]
        args += ['-S', source, '-B', build, '-G', 'Ninja', '-DCMAKE_BUILD_TYPE=Release']
        if OPTIONS.c_compiler:
            args.append('-DCMAKE_C_COMPILER=' + OPTIONS.c_compiler)
        if OPTIONS.cxx_compiler:
            args.append('-DCMAKE_CXX_COMPILER=' + OPTIONS.cxx_compiler)
        result = subprocess.run(list(map(str, [*args, *flags])), cwd=source, capture_output=True, text=True)
        if success:
            self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
        else:
            self.assertNotEqual(result.returncode, 0, result.stdout + result.stderr)
        return result

    def test_scalar_reference_does_not_overwrite_user_cache(self):
        source = self.project(ear=True)
        build = self.root / 'build'
        for scalar, preference, expected in [
            ('ON', None, 'generic_for_dispatch'), ('OFF', None, 'default_arch'),
            ('OFF', 'ON', 'default_arch'), ('ON', None, 'generic_for_dispatch'),
            ('OFF', None, 'default_arch'), ('ON', 'OFF', 'generic_for_dispatch'),
            ('OFF', None, 'generic_for_dispatch'), ('ON', 'ON', 'generic_for_dispatch'),
            ('OFF', None, 'default_arch'),
        ]:
            with self.subTest(scalar=scalar, preference=preference):
                flags = ['-DMR_ADM_EAR_SCALAR_REFERENCE=' + scalar]
                if preference:
                    flags.append('-DEAR_SIMD=' + preference)
                self.configure(source, build, *flags)
                self.assertIn('XSIMD_ARCHS=xsimd::' + expected, (build / 'ear-mode.txt').read_text())
                if preference:
                    line = next(line for line in (build / 'CMakeCache.txt').read_text().splitlines()
                                if line.startswith('EAR_SIMD:'))
                    self.assertEqual(line.split('=', 1)[1], preference)
        manifest = json.loads((build / 'consistency-dependencies.json').read_text())
        ear = next(d for d in manifest['dependencies'] if d['name'] == 'libear')
        self.assertEqual(Path(ear['source_dir']).resolve(), (source / 'ear fixture').resolve())
        self.assertEqual(ear['provider'], 'source')
        self.assertEqual(next(d for d in manifest['dependencies'] if d['name'] == 'fmt')['provider'], 'external')

    def test_default_preset_clears_experimental_overrides(self):
        source = self.project(ear=True)
        shutil.copyfile(REPO / 'CMakePresets.json', source / 'CMakePresets.json')
        build = self.root / 'preset-build'
        self.configure(source, build, '-DMR_ADM_STRICT_FP=ON', '-DMR_ADM_EAR_SCALAR_REFERENCE=ON',
                       preset='consistency-a')
        self.configure(source, build, preset='consistency-a')
        cache = (build / 'CMakeCache.txt').read_text()
        self.assertIn('MR_ADM_STRICT_FP:BOOL=OFF', cache)
        self.assertIn('MR_ADM_EAR_SCALAR_REFERENCE:BOOL=OFF', cache)
        self.assertIn('default_arch', (build / 'ear-mode.txt').read_text())

    def test_flac_target_overrides_are_removed_only_when_controlled(self):
        source = self.project(ear=True, flac=True)
        build = self.root / 'flac-build'
        self.configure(source, build, '-DMR_ADM_STRICT_FP=OFF')
        baseline = (build / 'flac-options.txt').read_text()
        self.assertTrue(baseline)
        self.configure(source, build, '-DMR_ADM_STRICT_FP=ON')
        self.assertEqual((build / 'flac-options.txt').read_text(), '')
        self.run_ok([OPTIONS.cmake, '--build', build, '--target', 'FLAC'])
        self.configure(source, build, '-DMR_ADM_STRICT_FP=OFF')
        self.assertEqual((build / 'flac-options.txt').read_text(), baseline)

    def test_strict_fp_rejects_common_and_release_conflicts(self):
        source = self.project()
        build = self.root / 'strict-build'
        self.configure(source, build, '-DMR_ADM_STRICT_FP=ON')
        self.run_ok([OPTIONS.cmake, '--build', build])
        conflicts = [('CMAKE_CXX_FLAGS_RELEASE', '-Ofast'),
                     ('CMAKE_C_FLAGS_RELEASE', '-ffast-math'),
                     ('CMAKE_CXX_FLAGS', '-ffp-contract=fast')]
        if sys.platform == 'win32':
            conflicts = [('CMAKE_CXX_FLAGS_RELEASE', '/fp:fast'),
                         ('CMAKE_C_FLAGS_RELEASE', '/fp:fast'),
                         ('CMAKE_CXX_FLAGS', '/fp:contract')]
        for variable, flag in conflicts:
            with self.subTest(variable=variable, flag=flag):
                result = self.configure(source, build, '-DMR_ADM_STRICT_FP=ON',
                                        '-DCMAKE_CXX_FLAGS=', '-DCMAKE_C_FLAGS=',
                                        '-DCMAKE_CXX_FLAGS_RELEASE=', '-DCMAKE_C_FLAGS_RELEASE=',
                                        '-D' + variable + '=' + flag, success=False)
                self.assertIn('MR_ADM_STRICT_FP=ON conflicts', result.stdout + result.stderr)
                self.assertIn(variable, result.stdout + result.stderr)
        # Disabling the control must also remove the normal-variable strict flags.
        self.configure(source, build, '-DMR_ADM_STRICT_FP=OFF', '-DCMAKE_CXX_FLAGS=',
                       '-DCMAKE_C_FLAGS=', '-DCMAKE_C_FLAGS_RELEASE=', '-DCMAKE_CXX_FLAGS_RELEASE=')
        rows = json.loads((build / 'compile_commands.json').read_text())
        self.assertTrue(all('-ffp-contract=off' not in row['command'] for row in rows))


class BuildInfoTests(Harness):
    def setUp(self):
        super().setUp()
        self.build = self.root / 'build with spaces'
        self.build.mkdir()
        self.manifest = {'version': 1, 'project_source_dir': str(self.root), 'dependencies': []}
        self.write_manifest()
        self.write_cache()
        self.write_commands(['-fno-fast-math', '-ffp-contract=off'])

    def write_manifest(self):
        (self.build / 'consistency-dependencies.json').write_text(json.dumps(self.manifest))

    def write_cache(self, strict='OFF'):
        (self.build / 'CMakeCache.txt').write_text(
            'CMAKE_BUILD_TYPE:STRING=Release\nMR_ADM_STRICT_FP:BOOL=' + strict + '\n'
            'FETCHCONTENT_BASE_DIR:PATH=' + str(self.root / 'different cache') + '\n')

    def write_commands(self, flags):
        self.rows = [{'file': str(self.root / 'ear_renderer.cpp'),
                      'arguments': ['c++', *flags, '-c', str(self.root / 'ear_renderer.cpp')]}]
        (self.build / 'compile_commands.json').write_text(json.dumps(self.rows, separators=(',', ':')))

    def record(self, status=0):
        out = self.root / 'record.txt'
        result = subprocess.run([sys.executable, str(REPO / 'scripts/consistency/build_info.py'),
                                 str(self.build), str(out)], capture_output=True, text=True)
        self.assertEqual(result.returncode, status, result.stdout + result.stderr)
        return read_fields(out) if out.exists() else {}, result

    def git_source(self, name, separate=False):
        source = self.root / name
        source.mkdir(parents=True)
        args = ['git', 'init', '--quiet']
        if separate:
            args += ['--separate-git-dir', str(self.root / (name.replace('/', '-') + '-git'))]
        self.run_ok([*args, source])
        (source / 'source.c').write_text('int source() { return 0; }\n')
        self.run_ok(['git', '-C', source, 'add', 'source.c'])
        self.run_ok(['git', '-C', source, '-c', 'user.name=Consistency Test',
                     '-c', 'user.email=tests@example.invalid', '-c', 'commit.gpgsign=false',
                     'commit', '--quiet', '-m', 'test source'])
        sha = self.run_ok(['git', '-C', source, 'rev-parse', 'HEAD']).stdout.strip()
        return source, sha

    def test_manifest_sources_and_git_files_are_used(self):
        source, sha = self.git_source('source outside cache', separate=True)
        project, _ = self.git_source('project')
        archive = project / 'untracked-archive'
        archive.mkdir()
        (archive / 'source.c').write_text('int untracked();\n')
        self.manifest['project_source_dir'] = str(project)
        self.manifest['dependencies'] = [
            {'name': 'libear', 'provider': 'source', 'source_dir': str(source)},
            {'name': 'archive', 'provider': 'source', 'source_dir': str(archive)},
            {'name': 'installed', 'provider': 'external', 'config_dir': 'package config'},
        ]
        self.write_manifest()
        self.git_source('different cache/stale-src')
        fields, result = self.record()
        self.assertEqual(fields['dep.libear'], sha)
        self.assertEqual(fields['dep.libear.dirty'], 'false')
        self.assertEqual(fields['dep.count'], '3')
        self.assertEqual(fields['dep.archive'], 'unavailable')
        self.assertEqual(fields['dep.installed'], 'unavailable')
        self.assertIn('warning: archive', result.stderr)
        self.assertFalse(any('stale' in key for key in fields))
        (source / 'source.c').write_text('int modified();\n')
        self.assertEqual(self.record()[0]['dep.libear.dirty'], 'true')

    def test_missing_manifest_fails_explicitly(self):
        (self.build / 'consistency-dependencies.json').unlink()
        _, result = self.record(status=2)
        self.assertIn('reconfigure', result.stderr)

    def test_ofast_is_counted_and_rejected_for_strict_records(self):
        self.write_commands(['-fno-fast-math', '-ffp-contract=off', '-Ofast'])
        fields, _ = self.record()
        self.assertEqual(fields['compile.fast_math'], '1')
        self.assertEqual(fields['compile.ofast'], '1')
        self.write_cache(strict='ON')
        fields, _ = self.record(status=2)
        self.assertEqual(fields['validation.strict_fp'], 'failed')

    def test_strict_record_checks_each_language_command(self):
        self.write_cache(strict='ON')
        self.assertEqual(self.record()[0]['validation.strict_fp'], 'passed')
        self.rows.append({'file': 'no-controls.c', 'arguments': ['cc', '-O3', '-c', 'no-controls.c']})
        (self.build / 'compile_commands.json').write_text(json.dumps(self.rows))
        self.record(status=2)
        self.write_commands(['/fp:precise', '/fp:contract-'])
        self.assertEqual(self.record()[0]['validation.strict_fp'], 'passed')
        self.write_commands(['/fp:precise', '/fp:fast'])
        self.record(status=2)

    def test_sample_command_uses_json_keys_not_field_order(self):
        # Deliberately put file before command and put an unrelated command after it.
        command = 'c++ -DMSG="a b" -fno-fast-math -ffp-contract=off -c ear_renderer.cpp'
        rows = [{'file': 'ear_renderer.cpp', 'command': command},
                {'file': 'last.cpp', 'command': 'c++ -c last.cpp'}]
        (self.build / 'compile_commands.json').write_text(json.dumps(rows))
        fields, _ = self.record()
        self.assertEqual(fields['compile.sample.ear_renderer'], command)

    def test_quoted_definitions_are_not_compiler_flags(self):
        self.write_cache(strict='ON')
        guard = '/fp:precise' if sys.platform == 'win32' else '-fno-fast-math -ffp-contract=off'
        command = f'compiler /DMSG="not -Ofast here" {guard} -c ear_renderer.cpp'
        (self.build / 'compile_commands.json').write_text(json.dumps([
            {'file': 'ear_renderer.cpp', 'command': command}
        ]))
        fields, _ = self.record()
        self.assertEqual(fields['compile.fast_math'], '0')
        self.assertEqual(fields['validation.strict_fp'], 'passed')


if __name__ == '__main__':
    parser = argparse.ArgumentParser()
    parser.add_argument('--cmake', default=shutil.which('cmake'))
    parser.add_argument('--c-compiler')
    parser.add_argument('--cxx-compiler')
    OPTIONS, remaining = parser.parse_known_args()
    if not OPTIONS.cmake:
        parser.error('CMake is required')
    unittest.main(argv=[sys.argv[0], *remaining])
