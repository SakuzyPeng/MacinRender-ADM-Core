#!/usr/bin/env python3
"""Compare two complete collections from the same build and platform, byte for byte (phase 5.1).

Used to prove that a replacement replay tool (run-rust-phase2.py --tool) reproduces the tool it
replaces: same PCM, same in-collection comparisons and replay status, same kernels and diagnostic
checkpoints, and the same tool logs. It is not platform evidence; compare-rust-phase2.py does that.

Preconditions (exit 2 when unmet): both collections are complete, and share the schema, source
fingerprint, configuration, diagnostics flag, platform and case catalog.

Compared (exit 1 on any difference):
- manifest outputs (path and PCM sha256), comparisons and replay_status;
- manifest fixtures by sha256 (they include the Scene replay inputs and event lists the Scene tool writes);
- manifest artifacts (kernels, checkpoints, kernel-checkpoints) by sha256;
- every file under logs/, after one normalization: each collection's absolute root path (as
  written by the collector) is replaced by "<collection>". Logs carry no other volatile text:
  the collector runs tools without -v, and the tools print neither timings nor thread ids.
"""
import argparse
import json
from pathlib import Path
import sys

import phase2_common as common

PRECONDITION_KEYS = ('schema', 'source', 'config', 'diagnostics', 'offline', 'scene', 'stream', 'sofa',
                     'json_fixtures', 'room')
COMPARED = ('outputs', 'fixtures', 'comparisons', 'replay_status', 'artifacts', 'logs')
PLATFORM_KEYS = ('platform.system', 'platform.machine')
CHECKPOINT_SUFFIXES = ('.f32', '.c32', '.f64', '.i32')


class PreconditionError(ValueError):
    pass


def load(root):
    manifest = json.loads((root / 'manifest.json').read_text(encoding='utf-8'))
    if manifest.get('complete') is not True:
        raise PreconditionError(f'incomplete collection: {root}')
    return manifest


def check_preconditions(a, b):
    for key in PRECONDITION_KEYS:
        if a.get(key) != b.get(key):
            raise PreconditionError(f'collections differ in {key}; compare runs of the same build')
    for key in PLATFORM_KEYS:
        if a['build'].get(key) != b['build'].get(key):
            raise PreconditionError(f'collections differ in {key}; use compare-rust-phase2.py across platforms')


def normalize_log(data, root):
    for form in {str(root), root.as_posix()}:
        data = data.replace(form.encode('utf-8'), b'<collection>')
    return data


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
    logs = [{p.relative_to(root / 'logs').as_posix(): normalize_log(p.read_bytes(), root)
             for p in sorted((root / 'logs').rglob('*')) if p.is_file()} for root in (reference, candidate)]
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
