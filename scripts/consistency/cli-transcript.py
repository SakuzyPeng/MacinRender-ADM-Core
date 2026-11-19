#!/usr/bin/env python3
"""Record the CLI contract over a fixed command table and compare two records of the same build.

record: runs every command of cli-commands.json with `mradm` from the build directory (or a candidate
given with --tool mradm=PATH) and stores the exit code, stdout, stderr and every file the command
wrote. compare: checks that two records come from the same source, build and platform, then compares
them byte for byte. Only boundary.NORMALIZATIONS apply; class help and parse-error exempt text
differences only. Exit codes and file changes are compared for every class.

A command can require a successful exit and a non-empty output file. An output requirement may
depend on a build feature reported by an earlier `formats` command in the same table.

Exit codes of compare: 0 identical, 1 differences, 2 invalid or incomparable records.
"""
import argparse
import json
import os
from pathlib import Path
import platform
import re
import shutil
import subprocess
import sys
import tempfile
import time

import boundary
from boundary import PreconditionError, digest
import collection as common

SCHEMA = 'mradm.consistency.cli-transcript.v2'
TABLE_SCHEMA = 'mradm.consistency.cli-commands.v1'
TABLE = Path(__file__).with_name('cli-commands.json')
CLASSES = ('contract', 'help', 'parse-error')
TOOLS = ('mradm', 'mr_adm_make_fixture')
TIMEOUT_SECONDS = 900
PLACEHOLDER = re.compile(r'\{(fixture|json|room|sofa|text|missing|out):([^{}]+)\}')
COMMAND_ID = re.compile(r'[a-z0-9][a-z0-9-]*')


def load_table(path=TABLE):
    table = json.loads(path.read_text(encoding='utf-8'))
    if table.get('schema') != TABLE_SCHEMA or not isinstance(table.get('commands'), list):
        raise ValueError(f'{path}: not a {TABLE_SCHEMA} table')
    seen = set()
    has_formats = False
    for row in table['commands']:
        if not isinstance(row.get('id'), str) or not COMMAND_ID.fullmatch(row['id']):
            raise ValueError(f'invalid command id: {row.get("id")!r}')
        if row['id'] in seen:
            raise ValueError('duplicate command id: ' + row['id'])
        seen.add(row['id'])
        if not isinstance(row.get('args'), list) or not all(isinstance(a, str) for a in row['args']):
            raise ValueError(row['id'] + ': args must be a list of strings')
        if row.get('class', 'contract') not in CLASSES:
            raise ValueError(row['id'] + ': unknown class ' + str(row.get('class')))
        output = row.get('expected_output')
        if output is not None and (not isinstance(output, str) or f'{{out:{output}}}' not in row['args']):
            raise ValueError(row['id'] + ': expected_output must name an {out:FILE} argument')
        feature = row.get('output_requires_feature')
        if feature is not None and (not isinstance(feature, str) or not feature or output is None or not has_formats):
            raise ValueError(row['id'] + ': output_requires_feature needs expected_output and an earlier formats command')
        has_formats = has_formats or row['args'] == ['formats']
        for arg in row['args']:
            for kind, name in PLACEHOLDER.findall(arg):
                if kind == 'json' and name not in common.JSON_FIXTURES:
                    raise ValueError(f'{row["id"]}: unknown JSON fixture {name}')
                if kind == 'room' and name not in common.ROOM_FIXTURES:
                    raise ValueError(f'{row["id"]}: unknown room file {name}')
    return table


def placeholders(table, kind):
    return {name for row in table['commands'] for arg in row['args'] for k, name in PLACEHOLDER.findall(arg) if k == kind}


def resolve(arg, command_id):
    """A table argument with its placeholders replaced by paths relative to the record directory."""
    def replace(match):
        kind, name = match.groups()
        path = {'fixture': f'fixtures/adm/{name}.wav', 'json': f'fixtures/json/{name}', 'room': f'fixtures/room/{name}',
                'sofa': f'fixtures/sofa/{name}', 'text': f'fixtures/text/{name}', 'missing': f'fixtures/missing/{name}',
                'out': f'outputs/{command_id}/{name}'}[kind]
        return str(Path(path))
    return PLACEHOLDER.sub(replace, arg)


def check_expected_output(row, features, work, result):
    output = row.get('expected_output')
    if output is None:
        return
    feature = row.get('output_requires_feature')
    if feature is not None:
        if feature not in features:
            raise ValueError(f'{row["id"]}: formats did not report feature {feature}')
        if not features[feature]:
            return
    path = work / 'outputs' / row['id'] / output
    if result.returncode or not path.is_file() or path.stat().st_size == 0:
        log = (result.stderr or result.stdout).decode(errors='replace').strip()
        raise ValueError(f'{row["id"]}: expected exit 0 and non-empty {output}, got exit {result.returncode}\n{log}')


def record(build, out, tool=None, table_path=TABLE):
    build, out = build.resolve(), out.resolve()
    if out.exists() and any(out.iterdir()):
        raise ValueError('output must be empty; do not mix records')
    table = load_table(table_path)
    binaries = {name: boundary.exe(build, name) for name in TOOLS}
    for path in binaries.values():
        if not path.is_file():
            raise ValueError(f'missing tool: {path} (build target mr_adm_consistency_tools)')
    identity = {'configuration': digest((build / 'CMakeCache.txt').read_bytes()),
                'binaries': {name: digest(path.read_bytes()) for name, path in binaries.items()}}
    cli, override = binaries['mradm'], None
    if tool is not None:
        cli = tool.resolve()
        if not cli.is_file() or not os.access(cli, os.X_OK):
            raise ValueError(f'--tool mradm: not an executable file: {cli}')
        override = {'path': str(cli), 'sha256': digest(cli.read_bytes())}
    out.mkdir(parents=True, exist_ok=True)
    fixtures = boundary.prepare_fixtures(out, build, adm=placeholders(table, 'fixture'), sofa=placeholders(table, 'sofa'),
                                         text=placeholders(table, 'text'))
    (out / 'commands').mkdir()
    env = {k: v for k, v in os.environ.items() if not k.startswith(('MR_ADM_', 'MRADM_'))}
    manifest = {'schema': SCHEMA, 'complete': False, 'source': common.source_fingerprint(),
                'table': digest(table_path.read_bytes()),
                'platform': {'system': platform.system(), 'machine': platform.machine()},
                'build_identity': identity, 'tool_override': override, 'cli_sha256': digest(cli.read_bytes()),
                'fixtures': fixtures, 'normalizations': list(boundary.NORMALIZATIONS), 'commands': {}}
    # Keep the recorder's own files outside the command's working directory. Snapshot every changed
    # file after each command, including files outside outputs/<id> and overwrites of earlier outputs.
    # This also preserves intermediate contents if a later command overwrites or removes the same path.
    with tempfile.TemporaryDirectory(prefix='mradm-cli-') as temporary:
        work = Path(temporary).resolve()
        shutil.copytree(out / 'fixtures', work / 'fixtures')
        manifest['root'] = str(work)
        before = {'fixtures/' + name: sha for name, sha in fixtures.items()}
        start = int(time.time())
        features = {}
        for row in table['commands']:
            command_id = row['id']
            print('cli:', command_id, flush=True)
            (work / 'outputs' / command_id).mkdir(parents=True)
            args = [resolve(arg, command_id) for arg in row['args']]
            result = subprocess.run([str(cli), *args], cwd=work, env=env, capture_output=True, timeout=TIMEOUT_SECONDS)
            (out / 'commands' / (command_id + '.stdout')).write_bytes(result.stdout)
            (out / 'commands' / (command_id + '.stderr')).write_bytes(result.stderr)
            if row['args'] == ['formats']:
                first_line = result.stdout.partition(b'\n')[0]
                features = {}
                if result.returncode == 0 and first_line.startswith(b'Build features: '):
                    features = {name.decode(): state == b'yes'
                                for name, state in re.findall(rb'\b([a-z_][a-z0-9_]*)=(yes|no)\b', first_line)}
            check_expected_output(row, features, work, result)
            after = {n: digest(p.read_bytes()) for n, p in boundary.files_under(work).items()}
            changed = {name: sha for name, sha in after.items() if before.get(name) != sha}
            for name in changed:
                saved = out / 'files' / command_id / name
                saved.parent.mkdir(parents=True, exist_ok=True)
                shutil.copyfile(work / name, saved)
            manifest['commands'][command_id] = {
                'class': row.get('class', 'contract'), 'args': args, 'exit_code': result.returncode,
                'stdout': digest(result.stdout), 'stderr': digest(result.stderr),
                'files': changed, 'removed_files': sorted(set(before) - set(after))}
            before = after
        manifest['clock'] = {'start': start, 'end': int(time.time()) + 1}
    manifest['complete'] = True
    common.save(out / 'manifest.json', manifest)
    return manifest


def is_sha256(value):
    return isinstance(value, str) and re.fullmatch(r'[0-9a-f]{64}', value) is not None


def check_record_identity(manifest):
    source = manifest['source']
    if not isinstance(source['files'], dict) or source['sha256'] != common.json_digest(source['files']):
        raise ValueError('invalid source fingerprint')
    platform_info = manifest['platform']
    if not all(isinstance(platform_info[key], str) for key in ('system', 'machine')):
        raise ValueError('invalid capture platform')
    build = manifest['build_identity']
    binaries = build['binaries']
    if not all(is_sha256(value) for value in (manifest['table'], build['configuration'], *(binaries[name] for name in TOOLS))):
        raise ValueError('invalid command table or build digest')
    override = manifest['tool_override']
    expected = binaries['mradm']
    if override is not None:
        if not isinstance(override['path'], str) or not is_sha256(override['sha256']):
            raise ValueError('invalid CLI override')
        expected = override['sha256']
    if manifest['cli_sha256'] != expected:
        raise ValueError('CLI digest differs from the build and from the declared --tool mradm')


def load(root):
    try:
        manifest = json.loads((root / 'manifest.json').read_text(encoding='utf-8'))
        if not isinstance(manifest, dict) or manifest.get('schema') != SCHEMA or manifest.get('complete') is not True:
            raise ValueError('not a complete ' + SCHEMA + ' record')
        check_record_identity(manifest)
        boundary.check_clock(manifest)
        if not isinstance(manifest['fixtures'], dict) or not isinstance(manifest['commands'], dict):
            raise ValueError('invalid fixture or command inventory')
        boundary.check_record_fixtures(root, manifest)
        for command_id, row in manifest['commands'].items():
            if not COMMAND_ID.fullmatch(command_id) or not isinstance(row, dict):
                raise ValueError('invalid recorded command: ' + command_id)
            if (row['class'] not in CLASSES or not isinstance(row['args'], list)
                    or not all(isinstance(arg, str) for arg in row['args']) or type(row['exit_code']) is not int):
                raise ValueError('invalid recorded command outcome: ' + command_id)
            if not isinstance(row['files'], dict):
                raise ValueError('invalid output inventory: ' + command_id)
            for stream in ('stdout', 'stderr'):
                if digest((root / 'commands' / f'{command_id}.{stream}').read_bytes()) != row[stream]:
                    raise ValueError(f'{stream} integrity failure: {command_id}')
            removed = row['removed_files']
            if (not isinstance(removed, list) or not all(isinstance(name, str) for name in removed)
                    or removed != sorted(set(removed)) or set(removed) & set(row['files'])):
                raise ValueError('invalid removed file inventory: ' + command_id)
            actual = boundary.files_under(root / 'files' / command_id)
            if set(actual) != set(row['files']):
                raise ValueError('output inventory differs from manifest: ' + command_id)
            for name, path in actual.items():
                if digest(path.read_bytes()) != row['files'][name]:
                    raise ValueError(f'output integrity failure: {command_id}/{name}')
        return manifest
    except (OSError, ValueError, KeyError, TypeError) as error:
        raise PreconditionError(f'invalid record: {root}: {error}') from error


def check_preconditions(a, b):
    for key in ('source', 'table', 'platform', 'fixtures'):
        if a[key] != b[key]:
            raise PreconditionError(f'records differ in {key}; compare records of one source, table and platform')
    if a['build_identity'] != b['build_identity']:
        raise PreconditionError('records come from different builds')
    if set(a['commands']) != set(b['commands']):
        raise PreconditionError('records ran different commands')


def compare(reference, candidate):
    a, b = load(reference), load(candidate)
    check_preconditions(a, b)
    differences, informational = [], []
    for command_id in a['commands']:
        ra, rb = a['commands'][command_id], b['commands'][command_id]
        if ra['class'] != rb['class'] or ra['args'] != rb['args']:
            raise PreconditionError('command differs between records: ' + command_id)
        if ra['exit_code'] != rb['exit_code']:
            differences.append({'command': command_id, 'item': 'exit_code', 'reference': ra['exit_code'],
                                'candidate': rb['exit_code']})
        rows = []
        for stream in ('stdout', 'stderr'):
            xa = boundary.normalize((reference / 'commands' / f'{command_id}.{stream}').read_bytes(), a, text=True)
            xb = boundary.normalize((candidate / 'commands' / f'{command_id}.{stream}').read_bytes(), b, text=True)
            if xa != xb:
                rows.append({'item': stream, 'first_difference': boundary.first_difference(xa, xb),
                             'reference_bytes': len(xa), 'candidate_bytes': len(xb)})
        for row in rows:
            (differences if ra['class'] == 'contract' else informational).append({'command': command_id, **row})
        for row in boundary.compare_files(a, b, boundary.files_under(reference / 'files' / command_id),
                                          boundary.files_under(candidate / 'files' / command_id), 'files'):
            differences.append({'command': command_id, **row})
        if ra['removed_files'] != rb['removed_files']:
            differences.append({'command': command_id, 'item': 'removed_files',
                                'reference': ra['removed_files'], 'candidate': rb['removed_files']})
    return {'schema': 'mradm.consistency.cli-transcript-comparison.v1', 'identical': not differences,
            'commands': len(a['commands']), 'differences': differences, 'informational': informational,
            'tool_override': {'reference': a['tool_override'], 'candidate': b['tool_override']},
            'normalizations': list(boundary.NORMALIZATIONS)}


def main(argv=None):
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    sub = parser.add_subparsers(dest='action', required=True)
    rec = sub.add_parser('record', help='record the command table')
    rec.add_argument('build', type=Path)
    rec.add_argument('output', type=Path)
    rec.add_argument('--tool', metavar='mradm=PATH', help='record a candidate CLI instead of the build\'s mradm')
    rec.add_argument('--table', type=Path, default=TABLE)
    cmp = sub.add_parser('compare', help='compare two records of the same build')
    cmp.add_argument('reference', type=Path)
    cmp.add_argument('candidate', type=Path)
    cmp.add_argument('--output', type=Path)
    args = parser.parse_args(argv)
    if args.action == 'record':
        tool = None
        if args.tool is not None:
            name, sep, path = args.tool.partition('=')
            if name != 'mradm' or not sep or not path:
                parser.error('--tool expects mradm=PATH')
            tool = Path(path)
        try:
            manifest = record(args.build, args.output, tool, args.table)
        except ValueError as error:
            parser.error(str(error))
        print(f'recorded {len(manifest["commands"])} commands in {args.output}')
        return 0
    try:
        report = compare(args.reference.resolve(), args.candidate.resolve())
    except (PreconditionError, OSError) as error:
        print(error, file=sys.stderr)
        return 2
    if args.output:
        common.save(args.output, report)
    for row in report['differences']:
        print('DIFFERENT:', json.dumps(row, ensure_ascii=False))
    for row in report['informational']:
        print('text only:', json.dumps(row, ensure_ascii=False))
    print(f'{report["commands"]} commands, {len(report["differences"])} differences, '
          f'{len(report["informational"])} help / parse-error text differences')
    return 0 if report['identical'] else 1


if __name__ == '__main__':
    sys.exit(main())
