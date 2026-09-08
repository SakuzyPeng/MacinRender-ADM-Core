#!/usr/bin/env python3
"""Record CMake's actual dependency sources and audit exported compile commands."""
import argparse
import json
import os
import platform
import re
import shlex
import subprocess
import sys
from pathlib import Path


UNSAFE_FP = {
    '-Ofast', '-ffast-math', '-funsafe-math-optimizations', '-fassociative-math',
    '-ffinite-math-only', '-freciprocal-math', '-fno-signed-zeros',
}
FP_SUFFIXES = {'.c', '.cc', '.cpp', '.cxx', '.m', '.mm'}
CACHE_KEYS = (
    'CMAKE_BUILD_TYPE', 'CMAKE_CXX_COMPILER', 'CMAKE_C_COMPILER',
    'CMAKE_C_FLAGS', 'CMAKE_CXX_FLAGS', 'CMAKE_C_FLAGS_RELEASE', 'CMAKE_CXX_FLAGS_RELEASE',
    'CMAKE_OSX_ARCHITECTURES', 'MR_ADM_STRICT_FP', 'MR_ADM_EAR_SCALAR_REFERENCE',
    'MR_ADM_EAR_SIMD_EFFECTIVE', 'MR_ADM_HAVE_MSVC_FP_CONTRACT_OFF',
    'MR_ADM_CONSISTENCY_DIAGNOSTICS', 'MR_ADM_DIAGNOSTIC_PORTABLE_RNG',
    'MR_ADM_CORE_USE_INSTALLED_DEPS', 'MR_ADM_FLAC_PROVIDER', 'MR_ADM_OPUS_PROVIDER',
    'MR_ADM_ENABLE_SOFA', 'MR_ADM_ENABLE_IAMF', 'EAR_SIMD', 'SAF_PERFORMANCE_LIB',
    'SAF_ENABLE_SIMD', 'SAF_USE_FAST_MATH_FLAG', 'SAF_ENABLE_FAST_MATH_FLAG', 'FETCHCONTENT_BASE_DIR',
)


def read_cache(path):
    result = {}
    for line in path.read_text(encoding='utf-8-sig').splitlines():
        if line.startswith(('#', '//')) or '=' not in line:
            continue
        field, value = line.split('=', 1)
        if ':' in field:
            key, _ = field.split(':', 1)
            result[key] = value
    return result


def enabled(value):
    return value.upper() in {'1', 'ON', 'YES', 'TRUE', 'Y'}


def compiler_property(build, cache, lang, prop):
    version = '.'.join(cache.get('CMAKE_CACHE_' + part + '_VERSION', '')
                       for part in ('MAJOR', 'MINOR', 'PATCH'))
    info = build / 'CMakeFiles' / version / ('CMake' + lang + 'Compiler.cmake')
    if not info.is_file():
        candidates = list((build / 'CMakeFiles').glob('*/CMake' + lang + 'Compiler.cmake'))
        if len(candidates) != 1:
            return 'unavailable'
        info = candidates[0]
    match = re.search(r'^set\(CMAKE_' + lang + '_' + prop + r' "([^"]*)"\)',
                      info.read_text(encoding='utf-8-sig'), re.MULTILINE)
    return match.group(1) if match else 'unavailable'


def git_run(source, *args):
    try:
        result = subprocess.run(['git', '-C', str(source), *args], capture_output=True, text=True, encoding="utf-8", errors="replace")
    except OSError:
        return None
    return result.stdout.strip() if result.returncode == 0 else None


def source_revision(source):
    if not source.is_dir():
        return None, None, 'source directory is unavailable'
    top = git_run(source, 'rev-parse', '--show-toplevel')
    if top is None:
        return None, None, 'no Git checkout or Git unavailable'
    top = Path(top).resolve()
    source = source.resolve()
    if top != source:
        # Avoid attributing an untracked FetchContent archive to the surrounding project repo.
        try:
            relative = source.relative_to(top).as_posix()
        except ValueError:
            return None, None, 'Git root does not contain the source directory'
        if not git_run(top, 'ls-files', '--', ':(literal)' + relative):
            return None, None, 'source directory is not tracked by its enclosing Git repository'
    revision = git_run(source, 'rev-parse', 'HEAD')
    if not revision:
        return None, None, 'Git revision unavailable'
    status = git_run(source, 'status', '--porcelain', '--', '.')
    return revision, 'unavailable' if status is None else str(bool(status)).lower(), ''


def command_tokens(entry):
    if isinstance(entry.get('arguments'), list):
        return entry['arguments']
    command = entry.get('command')
    if not isinstance(command, str):
        raise ValueError('compile command has neither command nor arguments')
    if os.name != 'nt':
        return shlex.split(command)
    # shlex(posix=False) splits /DMSG="text -Ofast text" incorrectly. Use the native
    # Windows argv rules so quoted macro contents cannot be mistaken for compiler flags.
    import ctypes
    from ctypes import wintypes
    shell = ctypes.WinDLL('shell32', use_last_error=True)
    shell.CommandLineToArgvW.argtypes = [wintypes.LPCWSTR, ctypes.POINTER(ctypes.c_int)]
    shell.CommandLineToArgvW.restype = ctypes.POINTER(wintypes.LPWSTR)
    count = ctypes.c_int()
    argv = shell.CommandLineToArgvW(command, ctypes.byref(count))
    if not argv:
        raise OSError('cannot parse Windows compile command')
    kernel = ctypes.WinDLL('kernel32', use_last_error=True)
    kernel.LocalFree.argtypes = [ctypes.c_void_p]
    kernel.LocalFree.restype = ctypes.c_void_p
    try:
        return [argv[index] for index in range(count.value)]
    finally:
        kernel.LocalFree(ctypes.cast(argv, ctypes.c_void_p))


def unsafe_flag(token):
    return (token in UNSAFE_FP or token.lower() in {'/fp:fast', '/fp:contract'}
            or (token.startswith('-ffp-contract=') and token != '-ffp-contract=off'))


def collect(build):
    cache = read_cache(build / 'CMakeCache.txt')
    manifest_path = build / 'consistency-dependencies.json'
    if not manifest_path.is_file():
        raise ValueError('missing consistency-dependencies.json; reconfigure this build before recording it')
    manifest = json.loads(manifest_path.read_text(encoding='utf-8-sig'))
    if manifest.get('version') != 1 or not isinstance(manifest.get('dependencies'), list):
        raise ValueError('unsupported dependency record format')
    entries = json.loads((build / 'compile_commands.json').read_text(encoding='utf-8-sig'))
    if not isinstance(entries, list) or not entries:
        raise ValueError('compile_commands.json must contain compile entries')
    tokens = [command_tokens(entry) for entry in entries]
    record = {
        'record.version': '2', 'record.build_dir': build.name,
        'platform.system': platform.system(), 'platform.machine': platform.machine(),
        'platform.release': platform.release(),
    }
    for lang in ('C', 'CXX'):
        for prop in ('COMPILER_ID', 'COMPILER_VERSION'):
            record[f'compiler.{lang}.{prop}'] = compiler_property(build, cache, lang, prop)
    for key in CACHE_KEYS:
        record['cmake.' + key] = cache.get(key, 'unavailable')
    record['compile.entries'] = str(len(entries))
    for key, flag in [('ffp_contract_off', '-ffp-contract=off'), ('fp_precise', '/fp:precise'),
                      ('fp_contract_off_msvc', '/fp:contract-'), ('fp_fast', '/fp:fast'),
                      ('ofast', '-Ofast')]:
        record['compile.' + key] = str(sum(flag in row for row in tokens))
    record['compile.fast_math'] = str(sum(any(t in {'-ffast-math', '-Ofast'} or t.lower() == '/fp:fast'
                                            for t in row) for row in tokens))
    record['compile.unsafe_fp'] = str(sum(any(unsafe_flag(t) for t in row) for row in tokens))
    record['compile.sample.ear_renderer'] = 'unavailable'
    for entry in entries:
        if Path(entry['file']).name == 'ear_renderer.cpp':
            record['compile.sample.ear_renderer'] = entry.get('command') or json.dumps(entry['arguments'])
            break

    errors = []
    if enabled(cache.get('MR_ADM_STRICT_FP', 'OFF')):
        checked = 0
        for entry, row in zip(entries, tokens):
            if Path(entry['file']).suffix.lower() not in FP_SUFFIXES:
                continue  # Assembly/resource compilers do not take these language flags.
            checked += 1
            strict = (('-fno-fast-math' in row and '-ffp-contract=off' in row)
                      or any(t.lower() in {'/fp:precise', '/fp:strict'} for t in row))
            if not strict or any(unsafe_flag(t) for t in row):
                errors.append('invalid strict FP command: ' + entry['file'])
        if checked == 0:
            errors.append('no C/C++/Objective-C compile commands to validate')
        record['validation.strict_fp'] = 'failed' if errors else 'passed'
    else:
        record['validation.strict_fp'] = 'not-requested'
    if enabled(cache.get('MR_ADM_EAR_SCALAR_REFERENCE', 'OFF')):
        if cache.get('MR_ADM_EAR_SIMD_EFFECTIVE') != 'OFF':
            errors.append('libear scalar reference was not established by this configuration')

    project_source = Path(manifest['project_source_dir'])
    revision, dirty, reason = source_revision(project_source)
    record['project.source_commit'] = revision or 'unavailable'
    record['project.source_dirty'] = dirty or 'unavailable'
    if reason:
        record['project.source_note'] = reason
    unavailable = 0
    for dependency in manifest['dependencies']:
        name = dependency['name']
        provider = dependency['provider']
        prefix = 'dep.' + name
        record[prefix + '.provider'] = provider
        record[prefix + '.source_dir'] = dependency.get('source_dir', '')
        record[prefix + '.config_dir'] = dependency.get('config_dir', '')
        if provider == 'source' and dependency.get('source_dir'):
            revision, dirty, reason = source_revision(Path(dependency['source_dir']))
            record[prefix] = revision or 'unavailable'
            record[prefix + '.dirty'] = dirty or 'unavailable'
        else:
            revision = None
            reason = 'external target; no FetchContent source revision'
            record[prefix] = 'unavailable'
        if reason:
            record[prefix + '.note'] = reason
        if not revision:
            unavailable += 1
            print(f'warning: {name}: {reason}', file=sys.stderr)
    record['dep.count'] = str(len(manifest['dependencies']))
    record['dep.unavailable'] = str(unavailable)
    for index, error in enumerate(errors):
        record[f'validation.error.{index}'] = error
    return record, errors


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('build_dir', type=Path)
    parser.add_argument('out_file', type=Path)
    args = parser.parse_args()
    try:
        record, errors = collect(args.build_dir.resolve())
        args.out_file.parent.mkdir(parents=True, exist_ok=True)
        lines = ['# MacinRender ADM Core — consistency build record']
        for key in sorted(record):
            value = str(record[key]).replace('\r', r'\r').replace('\n', r'\n')
            lines.append(key + '=' + value)
        args.out_file.write_text('\n'.join(lines) + '\n', encoding='utf-8')
    except (OSError, ValueError, KeyError, TypeError) as error:
        print('error: ' + str(error), file=sys.stderr)
        return 2
    if errors:
        for error in errors:
            print('error: ' + error, file=sys.stderr)
        return 2
    print('wrote ' + str(args.out_file))
    return 0


if __name__ == '__main__':
    sys.exit(main())
