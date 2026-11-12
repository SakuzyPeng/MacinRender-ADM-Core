#!/usr/bin/env python3
"""Run fixed C ABI call sequences against one libmradm_capi and compare two captures of the same build.

record: stages mr_adm_capi_capture beside the build's libmradm_capi (or a candidate given with --library)
and writes the capture: every recorded return code, result field, error message, JSON document, PCM and
output file of the call sequences (tests/tools/capi_capture.cpp).
tests: stages the C ABI consumer tests (the contract tests that use only c_api.h) beside the same library
and runs them; they must pass against either implementation.
compare: checks that two captures come from the same source, build and platform, then compares them byte
for byte after boundary.NORMALIZATIONS only. Exit codes: 0 identical, 1 differences, 2 invalid captures.

Both executables carry an $ORIGIN / @loader_path rpath (Windows searches the executable's directory first),
so the staged copy of the library is the one loaded; LD_LIBRARY_PATH, LD_PRELOAD and DYLD_* are cleared.
"""
import argparse
import json
import os
from pathlib import Path
import platform
import shutil
import subprocess
import sys
import tempfile
import time

import boundary
from boundary import PreconditionError, digest
import collection as common

SCHEMA = 'mradm.consistency.capi-capture.v1'
CAPTURE_TOOL = 'mr_adm_capi_capture'
# The contract tests that use only c_api.h and call into the library (the header smoke test links no
# function, so it cannot tell two libraries apart and keeps its ordinary ctest target only).
CONSUMER_TESTS = ('mr_adm_c_api_consumer_tests', 'mr_adm_scene_stream_c_api_consumer_tests')
FAMILIES = ('base', 'render', 'preview', 'scene', 'monitor')
ADM_FIXTURES = ('objects-point', 'objects-extent', 'objects-extent-multi', 'objects-cartesian', 'objects-room',
                'directspeakers', 'hoa', 'policy-objects')
SOFA = ('simple.sofa',)
TEXT = ('not-wave.wav',)
TIMEOUT_SECONDS = 1800


def library_name():
    """The file name the consumers load: the SONAME / install name / DLL name of libmradm_capi."""
    if os.name == 'nt':
        return 'mradm_capi.dll'
    if sys.platform == 'darwin':
        return 'libmradm_capi.1.dylib'
    return 'libmradm_capi.so.1'


def clean_env():
    return {k: v for k, v in os.environ.items()
            if not k.startswith(('MR_ADM_', 'MRADM_', 'DYLD_')) and k not in ('LD_LIBRARY_PATH', 'LD_PRELOAD')}


def stage(build, library, executables, directory):
    """Copy the executables and the library under its load name into directory; return their digests."""
    directory.mkdir(parents=True)
    staged = {}
    for name in executables:
        source = boundary.exe(build, name)
        if not source.is_file():
            raise ValueError(f'missing {source} (build target mr_adm_capi_consumers with MR_ADM_BUILD_CAPI_BUNDLE=ON)')
        shutil.copy2(source, directory / source.name)
        staged[name] = digest(source.read_bytes())
    if not library.is_file():
        raise ValueError(f'missing library: {library}')
    shutil.copyfile(library, directory / library_name())
    staged['library'] = digest(library.read_bytes())
    return staged


def resolve_library(build, library):
    default = build / library_name()
    if library is None:
        return default, None
    library = library.resolve()
    return library, {'path': str(library), 'sha256': digest(library.read_bytes()) if library.is_file() else None}


def record(build, out, library=None, families=FAMILIES):
    build, out = build.resolve(), out.resolve()
    if out.exists() and any(out.iterdir()):
        raise ValueError('output must be empty; do not mix captures')
    unknown = set(families) - set(FAMILIES)
    if unknown:
        raise ValueError('unknown families: ' + ', '.join(sorted(unknown)))
    library, override = resolve_library(build, library)
    built_library = build / library_name()
    identity = {'configuration': digest((build / 'CMakeCache.txt').read_bytes()),
                'binaries': {CAPTURE_TOOL: digest(boundary.exe(build, CAPTURE_TOOL).read_bytes()),
                             'mr_adm_make_fixture': digest(boundary.exe(build, 'mr_adm_make_fixture').read_bytes()),
                             'library': digest(built_library.read_bytes())}}
    out.mkdir(parents=True, exist_ok=True)
    fixtures = boundary.prepare_fixtures(out, build, adm=ADM_FIXTURES, sofa=SOFA, text=TEXT)
    with tempfile.TemporaryDirectory(prefix='mradm-capi-') as temporary:
        staged_dir = Path(temporary) / 'stage'
        staged = stage(build, library, (CAPTURE_TOOL,), staged_dir)
        start = int(time.time())
        result = subprocess.run([str(boundary.exe(staged_dir, CAPTURE_TOOL)), 'capture', *families], cwd=out,
                                env=clean_env(), capture_output=True, timeout=TIMEOUT_SECONDS)
        end = int(time.time()) + 1
    (out / 'tool.stdout').write_bytes(result.stdout)
    (out / 'tool.stderr').write_bytes(result.stderr)
    manifest = {'schema': SCHEMA, 'complete': True, 'source': common.source_fingerprint(),
                'platform': {'system': platform.system(), 'machine': platform.machine()},
                'build_identity': identity, 'library_override': override, 'library_sha256': staged['library'],
                'families': list(families), 'root': str(out), 'fixtures': fixtures,
                'normalizations': list(boundary.NORMALIZATIONS), 'clock': {'start': start, 'end': end},
                'exit_code': result.returncode, 'tool_stdout': digest(result.stdout),
                'tool_stderr': digest(result.stderr),
                'capture': {n: digest(p.read_bytes()) for n, p in boundary.files_under(out / 'capture').items()}}
    common.save(out / 'manifest.json', manifest)
    if result.returncode:
        raise RuntimeError(f'{CAPTURE_TOOL} failed ({result.returncode}): '
                           f'{result.stderr.decode(errors="replace")[-3000:]}')
    return manifest


def run_tests(build, library=None):
    build = build.resolve()
    library, override = resolve_library(build, library)
    report = {'library': str(library), 'library_override': override, 'tests': {}}
    with tempfile.TemporaryDirectory(prefix='mradm-capi-tests-') as temporary:
        staged_dir = Path(temporary) / 'stage'
        stage(build, library, CONSUMER_TESTS, staged_dir)
        work = Path(temporary) / 'work'
        work.mkdir()
        for name in CONSUMER_TESTS:
            print('test:', name, flush=True)
            result = subprocess.run([str(boundary.exe(staged_dir, name))], cwd=work, env=clean_env(),
                                    capture_output=True, timeout=TIMEOUT_SECONDS)
            report['tests'][name] = {'exit_code': result.returncode,
                                     'output_tail': (result.stdout + result.stderr).decode(errors='replace')[-2000:]}
    report['passed'] = all(row['exit_code'] == 0 for row in report['tests'].values())
    return report


def load(root):
    try:
        manifest = json.loads((root / 'manifest.json').read_text(encoding='utf-8'))
        if manifest.get('schema') != SCHEMA or manifest.get('complete') is not True:
            raise ValueError('not a complete ' + SCHEMA + ' capture')
        if manifest['exit_code'] != 0:
            raise ValueError('the capture tool failed')
        boundary.check_clock(manifest)
        boundary.check_record_fixtures(root, manifest)
        for stream in ('stdout', 'stderr'):
            if digest((root / f'tool.{stream}').read_bytes()) != manifest['tool_' + stream]:
                raise ValueError(f'tool {stream} integrity failure')
        actual = boundary.files_under(root / 'capture')
        if set(actual) != set(manifest['capture']) or 'capture.txt' not in actual:
            raise ValueError('capture inventory differs from manifest')
        for name, path in actual.items():
            if digest(path.read_bytes()) != manifest['capture'][name]:
                raise ValueError('capture integrity failure: ' + name)
        override = manifest['library_override']
        expected = override['sha256'] if override else manifest['build_identity']['binaries']['library']
        if manifest['library_sha256'] != expected:
            raise ValueError('library digest differs from the build and from the declared --library')
        return manifest
    except (OSError, ValueError, KeyError, TypeError) as error:
        raise PreconditionError(f'invalid capture: {root}: {error}') from error


def compare(reference, candidate):
    a, b = load(reference), load(candidate)
    for key in ('source', 'platform', 'fixtures', 'families', 'build_identity'):
        if a[key] != b[key]:
            raise PreconditionError(f'captures differ in {key}; compare captures of one source, build and platform')
    differences = []
    for stream in ('stdout', 'stderr'):
        xa = boundary.normalize((reference / f'tool.{stream}').read_bytes(), a, text=True)
        xb = boundary.normalize((candidate / f'tool.{stream}').read_bytes(), b, text=True)
        if xa != xb:
            differences.append({'item': 'tool.' + stream, 'first_difference': boundary.first_difference(xa, xb)})
    files_a, files_b = boundary.files_under(reference / 'capture'), boundary.files_under(candidate / 'capture')
    text_a, text_b = (boundary.normalize(f.pop('capture.txt').read_bytes(), m, text=True)
                      for f, m in ((files_a, a), (files_b, b)))
    lines_a, lines_b = text_a.split(b'\n'), text_b.split(b'\n')
    if lines_a != lines_b:
        section = b''
        for index in range(max(len(lines_a), len(lines_b))):
            la = lines_a[index] if index < len(lines_a) else None
            lb = lines_b[index] if index < len(lines_b) else None
            if la is not None and la.startswith(b'['):
                section = la
            if la != lb:
                differences.append({'item': 'capture.txt', 'line': index + 1, 'section': section.decode(errors='replace'),
                                    'reference': None if la is None else la.decode(errors='replace')[:300],
                                    'candidate': None if lb is None else lb.decode(errors='replace')[:300]})
                if len(differences) >= 200:
                    break
    differences += boundary.compare_files(a, b, files_a, files_b, 'capture')
    return {'schema': 'mradm.consistency.capi-capture-comparison.v1', 'identical': not differences,
            'lines': len(lines_a), 'files': len(files_a), 'differences': differences,
            'library_override': {'reference': a['library_override'], 'candidate': b['library_override']},
            'normalizations': list(boundary.NORMALIZATIONS)}


def main(argv=None):
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    sub = parser.add_subparsers(dest='action', required=True)
    rec = sub.add_parser('record', help='capture the call sequences')
    rec.add_argument('build', type=Path)
    rec.add_argument('output', type=Path)
    rec.add_argument('--library', type=Path, help='a candidate libmradm_capi instead of the build\'s')
    rec.add_argument('--family', action='append', choices=FAMILIES, help='only these families (default: all)')
    tst = sub.add_parser('tests', help='run the C ABI consumer tests against one library')
    tst.add_argument('build', type=Path)
    tst.add_argument('--library', type=Path)
    tst.add_argument('--output', type=Path)
    cmp = sub.add_parser('compare', help='compare two captures of the same build')
    cmp.add_argument('reference', type=Path)
    cmp.add_argument('candidate', type=Path)
    cmp.add_argument('--output', type=Path)
    args = parser.parse_args(argv)
    try:
        if args.action == 'record':
            manifest = record(args.build, args.output, args.library, tuple(args.family or FAMILIES))
            lines = (args.output / 'capture' / 'capture.txt').read_bytes().count(b'\n')
            print(f'captured {lines} values and {len(manifest["capture"]) - 1} files in {args.output}')
            return 0
        if args.action == 'tests':
            report = run_tests(args.build, args.library)
            if args.output:
                common.save(args.output, report)
            for name, row in report['tests'].items():
                print(('PASS' if row['exit_code'] == 0 else 'FAIL') + ':', name)
            return 0 if report['passed'] else 1
    except (ValueError, RuntimeError) as error:
        print(error, file=sys.stderr)
        return 2
    try:
        report = compare(args.reference.resolve(), args.candidate.resolve())
    except PreconditionError as error:
        print(error, file=sys.stderr)
        return 2
    if args.output:
        common.save(args.output, report)
    for row in report['differences']:
        print('DIFFERENT:', json.dumps(row, ensure_ascii=False))
    print(f'{report["lines"]} lines, {report["files"]} files, {len(report["differences"])} differences')
    return 0 if report['identical'] else 1


if __name__ == '__main__':
    sys.exit(main())
