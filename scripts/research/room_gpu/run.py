#!/usr/bin/env python3
"""Build/run the independent Windows CUDA ROOM feasibility probe.

Use the repository's canonical MSVC environment. No toolkit installation,
driver setting, production backend or dependency cache is changed.
"""
import argparse
import hashlib
import json
import os
from pathlib import Path
import shutil
import subprocess
import sys


def digest(path):
    result = hashlib.sha256()
    with path.open('rb') as source:
        for chunk in iter(lambda: source.read(1024 * 1024), b''):
            result.update(chunk)
    return result.hexdigest()


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--fixture', type=Path, required=True)
    parser.add_argument('--output', type=Path, required=True)
    parser.add_argument('--env-file', type=Path, help='private canonical MSVC environment JSON')
    parser.add_argument('--cuda-root', type=Path)
    parser.add_argument('--binary', type=Path, help='reuse a previously built probe')
    parser.add_argument('--route', choices=['transfer', 'response', 'resident'], default='resident')
    parser.add_argument('--targets', type=int, choices=[1, 4, 8, 16], default=16)
    parser.add_argument('--calls', type=int, choices=[0, 64, 1024], default=64)
    parser.add_argument('--scenario', choices=['both', 'ramps', 'distance-ramps'], default='both')
    parser.add_argument('--submission', choices=['graph', 'launch'], default='graph')
    parser.add_argument('--paced', action='store_true')
    args = parser.parse_args()
    if sys.platform != 'win32':
        parser.error('This probe requires Windows and an existing NVIDIA CUDA toolkit')
    source = Path(__file__).resolve().parent
    fixture = args.fixture.resolve()
    output = args.output.resolve()
    output.mkdir(parents=True, exist_ok=False)
    env = os.environ.copy()
    if args.env_file:
        env.update(json.loads(args.env_file.read_text(encoding='utf-8')))
    nvcc = shutil.which('nvcc.exe', path=env.get('PATH')) or shutil.which('nvcc.exe')
    cuda = args.cuda_root or (Path(nvcc).parent.parent if nvcc else None)
    if cuda is None or not (cuda / 'include/nvrtc.h').is_file():
        parser.error('CUDA toolkit not found; supply --cuda-root')
    env['PATH'] = str(cuda / 'bin') + os.pathsep + env['PATH']
    manifest = json.loads((fixture / 'manifest.json').read_text(encoding='utf-8'))
    if manifest.get('schema') != 'mradm.room-gpu-fixture' or manifest.get('version') != 1:
        parser.error('Unsupported fixture manifest')
    for name, entry in manifest['files'].items():
        if Path(name).name != name:
            parser.error('Invalid fixture payload name')
        path = fixture / name
        if path.stat().st_size != entry['bytes'] or digest(path) != entry['sha256']:
            parser.error('Fixture length or SHA-256 mismatch: ' + name)
    binary = args.binary.resolve() if args.binary else output / 'probe.exe'
    if not args.binary:
        compiler = shutil.which('cl.exe', path=env['PATH'])
        if not compiler:
            parser.error('Load the canonical MSVC environment or supply --env-file')
        command = [compiler, '/nologo', '/utf-8', '/O2', '/EHsc', '/std:c++20', '/DNDEBUG', '/W4',
                   '/I' + str(cuda / 'include'), str(source / 'probe.cpp'),
                   '/Fo' + str(output / 'probe.obj'), '/Fe' + str(binary), '/link',
                   '/LIBPATH:' + str(cuda / 'lib/x64'), 'cuda.lib', 'cudart.lib', 'nvrtc.lib', 'cufft.lib']
        with (output / 'build.log').open('w', encoding='utf-8') as log:
            subprocess.run(command, env=env, cwd=output, stdout=log, stderr=subprocess.STDOUT, check=True)
    monitor = ['nvidia-smi', '--query-gpu=name,driver_version,pstate,utilization.gpu,clocks.current.sm,power.draw,memory.used',
               '--format=csv']
    before = subprocess.check_output(monitor, text=True, env=env)
    command = [str(binary), str(fixture), str(source / 'kernels.cu'), args.route, str(args.targets),
               str(args.calls), args.submission, args.scenario, str(int(args.paced))]
    with (output / 'probe.json').open('w', encoding='utf-8') as out, (output / 'probe.stderr').open('w', encoding='utf-8') as err:
        subprocess.run(command, env=env, cwd=output, stdout=out, stderr=err, check=True)
    after = subprocess.check_output(monitor, text=True, env=env)
    probe = json.loads((output / 'probe.json').read_text(encoding='utf-8'))
    report = dict(schema='mradm.room-gpu-probe-run', version=1, probe=probe,
                  binary_sha256=digest(binary), fixture_manifest_sha256=digest(fixture / 'manifest.json'),
                  source_sha256={name: digest(source / name) for name in ['probe.cpp', 'kernels.cu']},
                  gpu_before=before, gpu_after=after,
                  full_room_acceptance=False, scope='Partial arithmetic pipeline with precomputed geometry. Host packing, transfers, submission, synchronization and output copy are timed. No production Session scheduling or finite fade validation.')
    (output / 'report.json').write_text(json.dumps(report, indent=2) + '\n', encoding='utf-8')
    print(json.dumps(report))
    if probe['deadline_misses']:
        raise SystemExit(1)


if __name__ == '__main__':
    main()
