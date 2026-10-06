#!/usr/bin/env python3
"""Collect a Release Rust baseline. Numerical divergence is evidence; invalid evidence fails."""
import argparse
import json
import os
from pathlib import Path
import shutil
import subprocess
import sys
import phase2_common as common
from build_info import collect, read_cache


def run(command, log, trace=None, extra_env=None):
    env = os.environ.copy()
    for key in ('MR_ADM_TRACE_DIR', 'MR_ADM_DIAGNOSTIC_WORKERS', 'MR_ADM_DIAGNOSTIC_GROUP_BUDGET'):
        env.pop(key, None)
    if trace is not None:
        env['MR_ADM_TRACE_DIR'] = str(trace.resolve())
    env.update(extra_env or {})
    log.parent.mkdir(parents=True, exist_ok=True)
    with log.open('wb') as output:
        result = subprocess.run([str(v) for v in command], stdout=output, stderr=subprocess.STDOUT, env=env)
    if result.returncode:
        raise RuntimeError(f'command failed ({result.returncode}): {command[0]}: {log.read_text(encoding="utf-8", errors="replace")[-3000:]}')


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('build', type=Path)
    parser.add_argument('output', type=Path)
    parser.add_argument('--config', required=True, choices=['a', 'b'])
    parser.add_argument('--baseline', type=Path, help='diagnostic run must match this uninstrumented run')
    args = parser.parse_args()
    build, out = args.build.resolve(), args.output.resolve()
    if out.exists() and any(out.iterdir()):
        parser.error('output must be empty; do not mix runs')
    cache = read_cache(build / 'CMakeCache.txt')
    required = {'CMAKE_BUILD_TYPE': 'Release', 'MR_ADM_ENABLE_IAMF': 'OFF', 'MR_ADM_ENABLE_SOFA': 'OFF',
                'MR_ADM_FLAC_PROVIDER': 'VENDORED', 'MR_ADM_OPUS_PROVIDER': 'VENDORED',
                'MR_ADM_CORE_USE_INSTALLED_DEPS': 'OFF', 'MR_ADM_STRICT_FP': 'ON' if args.config == 'b' else 'OFF'}
    for key, value in required.items():
        if cache.get(key) != value:
            parser.error(f'{key} must be {value}; found {cache.get(key)}')
    diagnostics = cache.get('MR_ADM_CONSISTENCY_DIAGNOSTICS') == 'ON'
    if diagnostics != bool(args.baseline):
        parser.error('diagnostic builds require --baseline; plain builds must not use it')
    binaries = {name: build / (name + ('.exe' if os.name == 'nt' else '')) for name in
                ('mradm', 'mr_adm_make_fixture', 'mr_adm_pcm_bits', 'mr_adm_phase2_render', 'mr_adm_phase2_scene', 'mr_adm_phase2_kernels', 'mr_adm_repeat_render')}
    for binary in binaries.values():
        if not binary.is_file():
            parser.error(f'missing tool: {binary}')
    out.mkdir(parents=True, exist_ok=True)
    for name in ('fixtures', 'pcm', 'kernels', 'cases', 'logs'):
        (out / name).mkdir()
    source = common.source_fingerprint()
    stamp = json.loads((build / 'phase2-build-stamp.json').read_text(encoding='utf-8'))
    if stamp['source'] != source or stamp['binaries'] != {k: common.digest(v.read_bytes()) for k, v in binaries.items()}:
        raise ValueError('stale source/binaries; build target mr_adm_phase2_tools first')
    offline, scenes = common.offline_cases(), common.scene_cases()
    build_record, build_errors = collect(build)
    if build_errors:
        raise ValueError('invalid build provenance: ' + '; '.join(build_errors))
    manifest = {'schema': 'mradm.phase2.v1', 'complete': False, 'config': args.config, 'diagnostics': diagnostics,
                'source': source, 'offline': offline, 'scene': scenes, 'build': build_record,
                'binaries': {k: common.digest(v.read_bytes()) for k, v in binaries.items()},
                'comparisons': {}, 'outputs': {}, 'fixtures': {}, 'replay_status': {}}
    if manifest['build'].get('rust.compiler_verbose') == 'unavailable':
        raise ValueError('actual Rust compiler provenance unavailable')
    common.save(out / 'manifest.json', manifest)
    def record(name, path):
        shape, bits = common.pcm_bytes(path)
        manifest['outputs'][name] = {'path': path.relative_to(out).as_posix(), 'shape': shape, 'sha256': common.digest(bits)}
    work = out / '_work'; work.mkdir()
    try:
        for fixture in sorted({c['fixture'] for c in offline}):
            run([binaries['mr_adm_make_fixture'], fixture, out / 'fixtures' / (fixture + '.wav')], out / 'logs' / (fixture + '.log'))
        for row in offline:
            name = row['id']; print('offline:', name, flush=True)
            case = out / 'cases' / (name + '.json'); common.save(case, row)
            fixture = out / 'fixtures' / (row['fixture'] + '.wav')
            destination = out / 'pcm' / (name + '.pcmbits')
            trace = out / 'checkpoints' / name if diagnostics else None
            for process in (1, 2):
                wav, bits = work / 'cli.wav', work / 'cli.pcmbits'
                run([binaries['mradm'], 'render', '-i', fixture, '-o', wav, '--output-bit-depth', 'f32', *row['args']],
                    out / 'logs' / f'{name}-cli{process}.log', trace if process == 1 else None)
                run([binaries['mr_adm_pcm_bits'], 'extract', wav, bits], out / 'logs' / f'{name}-extract{process}.log')
                wav.unlink()
                if process == 1:
                    shutil.move(bits, destination)
                else:
                    manifest['comparisons'][name + '-new-process'] = common.require_same(destination, bits); bits.unlink()
            run([binaries['mr_adm_phase2_render'], case, fixture, work / 'repeat'], out / 'logs' / (name + '-repeat.log'))
            for repeat in (1, 2):
                bits = work / 'repeat' / f'pass-{repeat}.pcmbits'
                manifest['comparisons'][name + f'-same-process-{repeat}'] = common.require_same(destination, bits)
                bits.unlink()
            record(name, destination)
        for row in scenes:
            name = row['id']; print('scene:', name, flush=True)
            case = out / 'cases' / (name + '.json'); common.save(case, row)
            for process in (1, 2):
                directory = work / f'scene{process}'
                run([binaries['mr_adm_phase2_scene'], case, directory], out / 'logs' / f'{name}-process{process}.log',
                    out / 'checkpoints' / name if diagnostics and process == 1 else None)
                manifest['replay_status'][name + f'-process{process}'] = [
                    json.loads((directory / f'pass-{i}.json').read_text(encoding='utf-8')) for i in (1, 2)]
                if process == 1:
                    for file in ('input.pcmbits', 'events.json'):
                        shutil.copyfile(directory / file, out / 'fixtures' / (name + '-' + file))
                else:
                    for file in ('input.pcmbits', 'events.json'):
                        if (directory / file).read_bytes() != (out / 'fixtures' / (name + '-' + file)).read_bytes():
                            raise ValueError('scene replay changed input or event manifest')
                for epoch in (1, 2):
                    case_id = name + f'-epoch{epoch}'
                    first, second = (directory / f'pass-{i}-epoch-{epoch}.pcmbits' for i in (1, 2))
                    manifest['comparisons'][case_id + f'-same-process{process}'] = common.require_same(first, second)
                    destination = out / 'pcm' / (case_id + '.pcmbits')
                    if process == 1:
                        shutil.copyfile(first, destination); record(case_id, destination)
                    else:
                        manifest['comparisons'][case_id + '-new-process'] = common.require_same(destination, first)
                shutil.rmtree(directory)
        for process in (1, 2):
            directory = work / f'kernels{process}'
            run([binaries['mr_adm_phase2_kernels'], directory], out / 'logs' / f'kernels{process}.log',
                out / 'kernel-checkpoints' if diagnostics and process == 1 else None)
            for path in sorted((directory / 'pass-1').iterdir()):
                other = directory / 'pass-2' / path.name
                if path.suffix in common.FORMATS:
                    comparison = common.compare_words(path.read_bytes(), other.read_bytes(), path.suffix)
                    if not comparison['identical']:
                        raise ValueError('kernel same-process repeat failed: ' + path.name)
                    if process == 2:
                        comparison = common.compare_words(path.read_bytes(), (out / 'kernels' / path.name).read_bytes(), path.suffix)
                        if not comparison['identical']:
                            raise ValueError('kernel new-process repeat failed: ' + path.name)
                if process == 1:
                    shutil.copyfile(path, out / 'kernels' / path.name)
            shutil.rmtree(directory)
        # Partition sensitivity is evidence, never an invented partition-invariance promise.
        for row in scenes:
            name = row['id']
            if name.endswith('-fixed'):
                for epoch in (1, 2):
                    a = out / 'pcm' / f'{name}-epoch{epoch}.pcmbits'
                    b = out / 'pcm' / f'{name[:-5]}fragmented-epoch{epoch}.pcmbits'
                    manifest['comparisons'][name + f'-partition-epoch{epoch}'] = common.compare_pcm(a, b)
        if diagnostics:
            for row in offline:
                if not list((out / 'checkpoints' / row['id'] / 'writer').glob('*.f32')):
                    raise ValueError('missing offline PCM checkpoint: ' + row['id'])
            for row in scenes:
                if not list((out / 'checkpoints' / row['id'] / 'scene').glob('*.40-render.f32')):
                    raise ValueError('missing Scene renderer checkpoint: ' + row['id'])
            if not list((out / 'kernel-checkpoints').rglob('*.f32')):
                raise ValueError('Rust diagnostics feature produced no checkpoints')
            worker_experiments(binaries, out, work, manifest)
            baseline = json.loads((args.baseline / 'manifest.json').read_text(encoding='utf-8'))
            if not baseline['complete'] or baseline['source'] != source or baseline['config'] != args.config:
                raise ValueError('baseline source/config/completeness mismatch')
            if set(baseline['outputs']) != set(manifest['outputs']):
                raise ValueError('diagnostic case inventory changed')
            for name, row in manifest['outputs'].items():
                common.require_same(out / row['path'], args.baseline / baseline['outputs'][name]['path'])
            for path in (out / 'kernels').iterdir():
                if path.suffix in common.FORMATS and path.read_bytes() != (args.baseline / 'kernels' / path.name).read_bytes():
                    raise ValueError('kernel diagnostic noninterference failed: ' + path.name)
            manifest['noninterference'] = {'passed': True, 'baseline_manifest_sha256': common.digest((args.baseline / 'manifest.json').read_bytes())}
        manifest['fixtures'] = {p.name: common.digest(p.read_bytes()) for p in sorted((out / 'fixtures').iterdir())}
        manifest['artifacts'] = {p.relative_to(out).as_posix(): common.digest(p.read_bytes())
                                 for folder in ('kernels', 'checkpoints', 'kernel-checkpoints')
                                 for p in sorted((out / folder).rglob('*')) if p.is_file()}
        if source != common.source_fingerprint():
            raise ValueError('source changed during collection')
        manifest['complete'] = True
        common.save(out / 'manifest.json', manifest)
    finally:
        shutil.rmtree(work)
    print('baseline complete:', out, flush=True)


def worker_experiments(tools, out, work, manifest):
    results = {}
    for name, workers, groups in [('w1-g4', 1, 4), ('w2-g4', 2, 4), ('w1-g1', 1, 1)]:
        prefix = work / name
        log = out / 'logs' / (name + '.log')
        run([tools['mr_adm_repeat_render'], out / 'fixtures/objects-extent-multi.wav', prefix], log,
            extra_env={'MR_ADM_DIAGNOSTIC_WORKERS': str(workers), 'MR_ADM_DIAGNOSTIC_GROUP_BUDGET': str(groups)})
        text = log.read_text(encoding='utf-8', errors='replace')
        import re
        grouping = re.findall(r'(\d+) spreader track\(s\) in (\d+) adapter group\(s\)', text)
        if not grouping or any(int(g) > groups for _, g in grouping):
            raise ValueError('missing or invalid actual spreader grouping')
        if f'ola_workers={workers} spreader_workers={workers}' not in text:
            raise ValueError('worker override did not reach actual pools')
        files = []
        for repeat in (1, 2):
            wav = Path(str(prefix) + f'-{repeat}.wav'); bits = Path(str(prefix) + f'-{repeat}.pcmbits')
            run([tools['mr_adm_pcm_bits'], 'extract', wav, bits], out / 'logs' / f'{name}-extract{repeat}.log')
            wav.unlink(); files.append(bits)
        common.require_same(*files)
        destination = out / 'experiments' / (name + '.pcmbits'); destination.parent.mkdir(exist_ok=True)
        shutil.copyfile(files[0], destination)
        results[name] = {'workers': workers, 'group_budget': groups, 'actual_grouping': grouping, 'path': destination.relative_to(out).as_posix()}
    for name in ('w2-g4', 'w1-g1'):
        results[name]['versus_w1_g4'] = common.compare_pcm(out / results['w1-g4']['path'], out / results[name]['path'])
    manifest['worker_experiments'] = results


if __name__ == '__main__':
    main()
