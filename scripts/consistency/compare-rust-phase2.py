#!/usr/bin/env python3
"""Validate complete phase-2 evidence, then locate the first observed numerical divergence."""
import argparse
import fnmatch
import itertools
import json
from pathlib import Path
import re
import phase2_common as common

REQUIRED_PLATFORMS = {('Darwin', 'arm64'), ('Windows', 'x86_64'), ('Linux', 'x86_64')}
DEFAULT_GATES = Path(__file__).with_name('phase2-gates.json')
FFT_SIMD_FEATURES = {'default', 'avx', 'sse', 'neon', 'wasm_simd'}


def safe_file(root, relative):
    path = root / relative
    if path.resolve().is_relative_to(root.resolve()) and path.is_file():
        return path
    raise ValueError('missing or escaping artifact: ' + relative)


def compiler_identity(build):
    fields = dict(line.split(': ', 1) for line in build.get('rust.compiler_verbose', '').splitlines() if ': ' in line)
    if fields.get('release') != '1.98.0' or not fields.get('commit-hash') or not fields.get('LLVM version'):
        raise ValueError('actual Rust compiler does not match the pinned toolchain')
    return {key: fields[key] for key in ('release', 'commit-hash', 'LLVM version')}


def fft_features(build):
    """Resolved RustFFT/realfft features; any SIMD feature restores run-time dispatch (ADR 0015)."""
    try:
        features = json.loads(build['rust.fft_features'])
    except (KeyError, TypeError, ValueError):
        raise ValueError('missing resolved FFT feature record') from None
    if set(features) != {'rustfft', 'realfft'}:
        raise ValueError('FFT feature record does not cover rustfft and realfft')
    enabled = sorted(f'{name}/{feature}' for name, values in features.items() for feature in values
                     if feature in FFT_SIMD_FEATURES)
    if enabled:
        raise ValueError('FFT SIMD dispatch is enabled: ' + ', '.join(enabled))
    return features


def load_gates(path):
    data = json.loads(Path(path).read_text(encoding='utf-8'))
    if data.get('schema') != 'mradm.phase2.gates.v1' or not isinstance(data.get('gates'), list):
        raise ValueError('unsupported gate file')
    for gate in data['gates']:
        if gate.get('kind') not in {'kernel', 'case'} or not gate.get('pattern') or not gate.get('reason'):
            raise ValueError('invalid gate entry')
    return data['gates']


def gate_failures(report, gates):
    """Gated kernels/cases must be bit-identical on every platform pair; a gate must match something."""
    failures = []
    for gate in gates:
        matched = set()
        for pair in report['pairs']:
            rows = ({r['checkpoint']: r for r in pair['kernels']} if gate['kind'] == 'kernel' else pair['cases'])
            for name, row in rows.items():
                if fnmatch.fnmatchcase(name, gate['pattern']):
                    matched.add(name)
                    if not row['identical']:
                        failures.append({'pattern': gate['pattern'], 'name': name, 'platforms': pair['platforms']})
        if not matched:
            failures.append({'pattern': gate['pattern'], 'name': None, 'platforms': None})
    return failures


def validate_experiments(root, manifest):
    experiments = manifest.get('worker_experiments', {})
    expected = {'w1-g4': (1, 4), 'w2-g4': (2, 4), 'w1-g1': (1, 1)}
    if set(experiments) != set(expected):
        raise ValueError('worker experiment inventory is incomplete')
    for name, (workers, budget) in expected.items():
        row = experiments[name]
        if (row['workers'], row['group_budget']) != (workers, budget) or len(row['actual_grouping']) != 2:
            raise ValueError('invalid worker experiment configuration')
        if any(not 1 <= int(groups) <= min(int(tracks), budget) for tracks, groups in row['actual_grouping']):
            raise ValueError('invalid observed group count')
        common.pcm_bytes(safe_file(root, row['path']))
        if name != 'w1-g4':
            measured = common.compare_pcm(safe_file(root, experiments['w1-g4']['path']), safe_file(root, row['path']))
            if common.json_digest(measured) != common.json_digest(row['versus_w1_g4']):
                raise ValueError('worker comparison does not match artifact bytes')


def validate(root):
    manifest = json.loads((root / 'manifest.json').read_text(encoding='utf-8'))
    if manifest.get('schema') != 'mradm.phase2.v1' or manifest.get('complete') is not True:
        raise ValueError('incomplete or unsupported baseline')
    if manifest['source']['sha256'] != common.json_digest(manifest['source']['files']):
        raise ValueError('source fingerprint does not match inventory')
    if manifest['offline'] != common.offline_cases() or manifest['scene'] != common.scene_cases():
        raise ValueError('baseline does not cover the versioned case catalog')
    expected = [c['id'] for c in manifest['offline']] + [c['id'] + f'-epoch{e}' for c in manifest['scene'] for e in (1, 2)]
    if not expected or len(set(expected)) != len(expected) or set(expected) != set(manifest['outputs']):
        raise ValueError('missing or duplicate case outputs')
    disk_pcm = {p.name for p in (root / 'pcm').glob('*.pcmbits')}
    if disk_pcm != {Path(v['path']).name for v in manifest['outputs'].values()}:
        raise ValueError('PCM inventory differs from manifest')
    for row in manifest['outputs'].values():
        shape, bits = common.pcm_bytes(safe_file(root, row['path']))
        if list(shape) != row['shape'] or common.digest(bits) != row['sha256']:
            raise ValueError('PCM shape/hash differs from manifest')
    if not manifest['fixtures'] or set(manifest['fixtures']) != {p.name for p in (root / 'fixtures').iterdir()}:
        raise ValueError('fixture inventory differs from manifest')
    for name, sha in manifest['fixtures'].items():
        path = safe_file(root, 'fixtures/' + name)
        if common.digest(path.read_bytes()) != sha:
            raise ValueError('fixture integrity failure: ' + name)
        if path.suffix == '.pcmbits':
            common.pcm_bytes(path)
    for row in manifest['offline'] + manifest['scene']:
        if json.loads(safe_file(root, 'cases/' + row['id'] + '.json').read_text(encoding='utf-8')) != row:
            raise ValueError('case parameters differ from manifest')
    actual = {p.relative_to(root).as_posix() for folder in ('kernels', 'checkpoints', 'kernel-checkpoints')
              for p in (root / folder).rglob('*') if p.is_file()}
    if actual != set(manifest['artifacts']):
        raise ValueError('checkpoint inventory differs from manifest')
    for name, sha in manifest['artifacts'].items():
        path = safe_file(root, name); data = path.read_bytes()
        if common.digest(data) != sha:
            raise ValueError('checkpoint integrity failure: ' + name)
        if path.suffix in common.FORMATS:
            common.validate_words(data, path.suffix)
    kernels = {p for p in actual if p.startswith('kernels/') and Path(p).suffix in common.FORMATS}
    if kernels != {'kernels/' + name for name in common.kernel_outputs()}:
        raise ValueError('production-kernel measurements differ from the inventory')
    if manifest['diagnostics'] and not manifest.get('noninterference', {}).get('passed'):
        raise ValueError('diagnostic noninterference was not verified')
    if manifest['diagnostics']:
        validate_experiments(root, manifest)
    build = manifest['build']
    compiler_identity(build)
    fft_features(build)
    if build.get('cmake.CMAKE_BUILD_TYPE') != 'Release' or build.get('rust.compiler_verbose') in (None, 'unavailable'):
        raise ValueError('missing Release/compiler provenance')
    repeat_keys = {r['id'] + suffix for r in manifest['offline'] for suffix in
                   ('-new-process', '-same-process-1', '-same-process-2')}
    repeat_keys.update(r['id'] + f'-epoch{e}' + suffix for r in manifest['scene'] for e in (1, 2)
                       for suffix in ('-same-process1', '-same-process2', '-new-process'))
    repeat_keys.update(r['id'] + f'-partition-epoch{e}' for r in manifest['scene'] if r['id'].endswith('-fixed') for e in (1, 2))
    if repeat_keys != set(manifest['comparisons']):
        raise ValueError('repeat/partition evidence inventory is incomplete')
    status_keys = {r['id'] + f'-process{i}' for r in manifest['scene'] for i in (1, 2)}
    if set(manifest['replay_status']) != status_keys:
        raise ValueError('Scene health evidence inventory is incomplete')
    for row in manifest['scene']:
        expected_epochs = [{'epoch': e['epoch'], 'frames': ((e['end'] - e['target']) * row['output_rate'] + row['input_rate'] - 1) // row['input_rate']}
                           for e in row['epochs']]
        for process in (1, 2):
            records = manifest['replay_status'][row['id'] + f'-process{process}']
            if len(records) != 2 or any(r != {'epochs': expected_epochs, 'status': 'passed', 'underruns': 0} for r in records):
                raise ValueError('invalid Scene state/frame-count evidence')
    for name, row in manifest['comparisons'].items():
        if '-partition-' not in name and not row['identical']:
            raise ValueError('repeatability failure was marked complete')
    return manifest


def checkpoint_order(name):
    # Explicit causal stage order. Filenames within a stage only order observations,
    # never imply that unrelated branches cause each other.
    natural = tuple(int(v) if v.isdecimal() else v for v in re.split(r'(\d+)', name))
    if name.startswith('writer/'):
        return (80 if name.startswith('writer/pass0-') else 99), natural
    filename = Path(name).name
    match = re.search(r'\.(\d\d)-', filename)
    if name.startswith('post/') and match:
        return (85 if 'loudness' in name else 87) + int(match.group(1)) // 10, natural
    if match and not filename.startswith(('ear.', 'vbap.', 'binaural.', 'hoa.', 'cloud.')):
        return int(match.group(1)), natural
    mappings = [('ear.01', 20), ('vbap.01', 20), ('vbap.02', 20), ('binaural.01', 10),
                ('binaural.02', 20), ('binaural.03', 30), ('binaural.04', 30),
                ('cloud.01', 20), ('cloud.02', 20), ('ear.', 30), ('vbap.', 30), ('hoa.', 30), ('hrtf-', 35)]
    return next(((stage, natural) for prefix, stage in mappings if filename.startswith(prefix)), (50, natural))


def checkpoints(a, b):
    paths = [{p.relative_to(root).as_posix() for p in root.rglob('*') if p.suffix in common.FORMATS} for root in (a, b)]
    rows = []
    for name in sorted(paths[0] | paths[1], key=checkpoint_order):
        if name not in paths[0] or name not in paths[1]:
            rows.append({'checkpoint': name, 'status': 'different-observation-topology', 'identical': False})
        else:
            row = common.compare_words((a / name).read_bytes(), (b / name).read_bytes(), Path(name).suffix)
            row.update(checkpoint=name, status='identical' if row['identical'] else 'different')
            rows.append(row)
    return rows


def compare(directories, require_platforms=True, gates=()):
    manifests = [validate(p) for p in directories]
    first = manifests[0]
    for manifest in manifests[1:]:
        for field in ('config', 'diagnostics', 'source', 'offline', 'scene', 'fixtures'):
            if manifest[field] != first[field]:
                raise ValueError('incompatible baseline: ' + field)
        if compiler_identity(manifest['build']) != compiler_identity(first['build']):
            raise ValueError('Rust compiler identity differs across platforms')
        for field in ('rust.cargo_lock_sha256', 'rust.toolchain_config_sha256', 'scene.arithmetic', 'rust.resolved_features',
                      'rust.fft_features'):
            if manifest['build'].get(field) != first['build'].get(field):
                raise ValueError('incompatible build: ' + field)
    platforms = set()
    for m in manifests:
        arch = m['build']['platform.machine'].lower()
        arch = {'amd64': 'x86_64', 'aarch64': 'arm64'}.get(arch, arch)
        platforms.add((m['build']['platform.system'], arch))
    if require_platforms and platforms != REQUIRED_PLATFORMS:
        raise ValueError('expected macOS arm64, Windows x64 and Linux x64')
    report = {'schema': 'mradm.phase2.comparison.v1', 'source_sha256': first['source']['sha256'],
              'config': first['config'], 'diagnostics': first['diagnostics'],
              'full_platform_set': platforms == REQUIRED_PLATFORMS,
              'platforms': sorted(platforms), 'pairs': [],
              'interpretation': 'first_observed is an observation boundary, not a proven root cause'}
    for i, j in itertools.combinations(range(len(directories)), 2):
        a, b = directories[i], directories[j]
        pair = {'platforms': [a.name, b.name], 'cases': {}, 'kernels': checkpoints(a / 'kernels', b / 'kernels'),
                'kernel_checkpoints': checkpoints(a / 'kernel-checkpoints', b / 'kernel-checkpoints')}
        for name in sorted(first['outputs']):
            row = common.compare_pcm(a / first['outputs'][name]['path'], b / first['outputs'][name]['path'])
            if first['diagnostics']:
                case = re.sub(r'-epoch[12]$', '', name)
                observations = checkpoints(a / 'checkpoints' / case, b / 'checkpoints' / case)
                if name.endswith(('-epoch1', '-epoch2')):
                    epoch_marker = '/e' + name[-1] + '-'
                    observations = [r for r in observations if not r['checkpoint'].startswith(('scene/', 'device/')) or epoch_marker in r['checkpoint'] or ('/e' + name[-1] + '.') in r['checkpoint']]
                row['checkpoints'] = observations
                row['first_observed'] = next((r['checkpoint'] for r in observations if not r['identical']), None)
                if not row['identical'] and row['first_observed'] is None:
                    row['first_observed'] = 'final PCM; preceding sampled boundaries identical or not instrumented'
                row['root_cause'] = 'not established by this comparison'
            pair['cases'][name] = row
        report['pairs'].append(pair)
    report['identical_cases'] = [name for name in sorted(first['outputs']) if all(p['cases'][name]['identical'] for p in report['pairs'])]
    report['differing_cases'] = sorted(set(first['outputs']) - set(report['identical_cases']))
    # Only the listed kernels/cases are bit-equality gates; every other difference is measured.
    report['numerical_gate'] = {'gates': list(gates), 'failures': gate_failures(report, gates)}
    return report


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--output', required=True, type=Path)
    parser.add_argument('--allow-partial', action='store_true', help='diagnostic comparisons only; report cannot claim three-platform acceptance')
    parser.add_argument('--gates', type=Path, default=DEFAULT_GATES, help='cross-platform bit-equality gates')
    parser.add_argument('directories', nargs='+', type=Path)
    args = parser.parse_args()
    if len(args.directories) < 2:
        parser.error('at least two baselines required')
    result = compare(args.directories, not args.allow_partial, load_gates(args.gates))
    common.save(args.output, result)
    failures = result['numerical_gate']['failures']
    print(f"identical={len(result['identical_cases'])} differing={len(result['differing_cases'])} "
          f"full_platform_set={result['full_platform_set']} gate_failures={len(failures)}")
    for failure in failures:
        print('gate failure:', json.dumps(failure, sort_keys=True))
    if failures:
        raise SystemExit(1)


if __name__ == '__main__':
    main()
