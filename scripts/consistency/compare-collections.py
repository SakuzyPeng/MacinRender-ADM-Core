#!/usr/bin/env python3
"""Compare two complete collections from the same build and platform, byte for byte (phase 5.1).

Used to prove that a replacement replay tool (run-rust-phase2.py --tool) reproduces the tool it
replaces: same PCM, same in-collection comparisons and replay status, same kernels and diagnostic
checkpoints, and the same tool logs. It is not platform evidence; compare-rust-phase2.py does that.

Preconditions (exit 2 when unmet): both collections pass evidence integrity validation, and share
the source fingerprint, build identity (configuration and original binaries), build provenance,
diagnostics flag, platform and case catalog. Collections need the build identity, log inventory
and original root recorded by run-rust-phase2.py; recollect older evidence without these fields.

Compared (exit 1 on any difference):
- manifest outputs (path and PCM sha256), comparisons and replay_status;
- manifest fixtures by sha256 (they include the Scene replay inputs and event lists the Scene tool writes);
- manifest artifacts (kernels, checkpoints, kernel-checkpoints) by sha256;
- every file under logs/, after one normalization: each collection's absolute root path (as
  written by the collector) is replaced by "<collection>". Logs carry no other volatile text:
  the collector runs tools without -v, and the tools print neither timings nor thread ids.
"""
import argparse
from pathlib import Path, PurePosixPath, PureWindowsPath
import sys

import phase2_common as common

PRECONDITION_KEYS = ('schema', 'source', 'config', 'diagnostics', 'offline', 'scene', 'stream', 'sofa',
                     'json_fixtures', 'room')
COMPARED = ('outputs', 'fixtures', 'comparisons', 'replay_status', 'artifacts', 'logs')
PLATFORM_KEYS = ('platform.system', 'platform.machine')
CHECKPOINT_SUFFIXES = ('.f32', '.c32', '.f64', '.i32')
evidence = common.load_module('phase2_evidence', Path(__file__).with_name('compare-rust-phase2.py'))


class PreconditionError(ValueError):
    pass


def load(root):
    try:
        manifest = evidence.validate(root, allow_tool_overrides=True)
        if manifest['config'] not in ('a', 'b') or type(manifest['diagnostics']) is not bool:
            raise ValueError('missing or invalid configuration/diagnostics flag')
        if any(not isinstance(manifest['build'].get(key), str) or not manifest['build'][key]
               for key in PLATFORM_KEYS):
            raise ValueError('missing platform identity')
        if any(key not in manifest for key in ('build_identity', 'collection_root', 'logs')):
            raise ValueError('missing build identity, collection root or log inventory; recollect this evidence')
        identity = manifest['build_identity']
        if (set(identity) != {'configuration', 'binaries'}
                or set(identity['configuration']) != set(common.BUILD_CONFIGURATION_FILES)
                or set(identity['binaries']) != set(common.BUILD_TOOLS)):
            raise ValueError('incomplete build identity; recollect this evidence')
        overrides = manifest.get('tool_overrides', {})
        if set(overrides) - set(common.REPLAY_TOOLS):
            raise ValueError('only replay tools may be replaced')
        expected = dict(identity['binaries'])
        for name, row in overrides.items():
            if not isinstance(row['path'], str) or not row['path']:
                raise ValueError('missing replacement tool path: ' + name)
            expected[name] = row['sha256']
        if manifest['binaries'] != expected:
            raise ValueError('binary identity differs outside the declared tool overrides')
        origin = manifest['collection_root']
        if not isinstance(origin, str) or not origin or not original_root(manifest).is_absolute():
            raise ValueError('missing original collection root; recollect this evidence')
        logs = manifest['logs']
        actual = {p.relative_to(root / 'logs').as_posix() for p in (root / 'logs').rglob('*') if p.is_file()}
        if not logs or set(logs) != actual:
            raise ValueError('log inventory differs from manifest')
        for name, sha in logs.items():
            if common.digest(evidence.safe_file(root, 'logs/' + name).read_bytes()) != sha:
                raise ValueError('log integrity failure: ' + name)
        return manifest
    except (OSError, ValueError, KeyError, TypeError, AttributeError) as error:
        raise PreconditionError(f'invalid collection: {root}: {error}') from error


def check_preconditions(a, b):
    for key in PRECONDITION_KEYS:
        if a.get(key) != b.get(key):
            raise PreconditionError(f'collections differ in {key}; compare runs of the same build')
    for key in PLATFORM_KEYS:
        if a['build'].get(key) != b['build'].get(key):
            raise PreconditionError(f'collections differ in {key}; use compare-rust-phase2.py across platforms')
    for key in ('build_identity', 'build'):
        if a[key] != b[key]:
            raise PreconditionError(f'collections differ in {key}; compare runs of the same build')


def normalize_log(data, root):
    for form in {str(root), root.as_posix()}:
        data = data.replace(form.encode('utf-8'), b'<collection>')
    return data


def original_root(manifest):
    # Archived Windows collections can be compared on a POSIX host, and vice versa.
    path_type = PureWindowsPath if manifest['build']['platform.system'] == 'Windows' else PurePosixPath
    return path_type(manifest['collection_root'])


def compare_maps(name, first, second, report):
    same = sorted(k for k in first if k in second and first[k] == second[k])
    different = sorted(k for k in first if k in second and first[k] != second[k])
    report[name] = {'identical': len(same), 'different': different,
                    'only_reference': sorted(set(first) - set(second)),
                    'only_candidate': sorted(set(second) - set(first))}


def first_difference(path_a, path_b):
    """First differing word of a checkpoint, as compare-checkpoints.py reports it."""
    checkpoints = common.load_module('compare_checkpoints', Path(__file__).with_name('compare-checkpoints.py'))
    width = checkpoints.FORMATS[path_a.suffix][0]
    a, b = path_a.read_bytes(), path_b.read_bytes()
    for index in range(min(len(a), len(b)) // width):
        if a[index * width:(index + 1) * width] != b[index * width:(index + 1) * width]:
            return {'word': index}
    return {'word': min(len(a), len(b)) // width, 'lengths': [len(a) // width, len(b) // width]}


def compare(reference, candidate):
    a, b = load(reference), load(candidate)
    check_preconditions(a, b)
    report = {'schema': 'mradm.compare-collections.v1', 'reference': str(reference), 'candidate': str(candidate),
              'tool_overrides': {'reference': a.get('tool_overrides', {}), 'candidate': b.get('tool_overrides', {})}}
    for key in ('outputs', 'fixtures', 'comparisons', 'replay_status', 'artifacts'):
        compare_maps(key, a[key], b[key], report)
    details = {}
    for name in report['artifacts']['different']:
        if name.endswith(CHECKPOINT_SUFFIXES):
            details[name] = first_difference(reference / name, candidate / name)
    report['artifacts']['first_difference'] = details
    logs = [{name: normalize_log((root / 'logs' / name).read_bytes(), original_root(manifest))
             for name in manifest['logs']} for root, manifest in ((reference, a), (candidate, b))]
    compare_maps('logs', logs[0], logs[1], report)
    report['identical'] = all(not report[k]['different'] and not report[k]['only_reference']
                              and not report[k]['only_candidate']
                              for k in COMPARED)
    return report


def main():
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument('reference', type=Path)
    parser.add_argument('candidate', type=Path)
    parser.add_argument('--output', type=Path, help='write the full report as JSON')
    args = parser.parse_args()
    try:
        report = compare(args.reference.resolve(), args.candidate.resolve())
    except PreconditionError as error:
        print(f'error: {error}', file=sys.stderr)
        return 2
    if args.output:
        common.save(args.output, report)
    for key in COMPARED:
        row = report[key]
        print(f"{key}: identical={row['identical']} different={len(row['different'])} "
              f"only_reference={len(row['only_reference'])} only_candidate={len(row['only_candidate'])}")
        for name in (row['different'] + row['only_reference'] + row['only_candidate'])[:20]:
            print('  !', name, row.get('first_difference', {}).get(name, ''))
    print('identical' if report['identical'] else 'DIFFERENT')
    return 0 if report['identical'] else 1


if __name__ == '__main__':
    sys.exit(main())
