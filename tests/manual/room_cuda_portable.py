#!/usr/bin/env python3
"""Relocate a CUDA trial app, remove development PATH entries, and test dependency failures.

Each case runs in a fresh process. Only copies below --output are renamed.
Use --reference with a completed room_cuda_integration.py output directory.
"""
import argparse
import json
import os
from pathlib import Path
import shutil
import subprocess
import sys

CHECK = Path(__file__).with_name('room_cuda_integration.py')


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    for name in ['app', 'room', 'hrir', 'output', 'reference']:
        parser.add_argument('--' + name, type=lambda p: Path(p).resolve(), required=True)
    parser.add_argument('--stale-plugin', type=lambda p: Path(p).resolve())
    args = parser.parse_args()
    assert os.name == 'nt'
    args.output.mkdir(parents=True, exist_ok=False)
    app = args.output / '移位 CUDA app'
    shutil.copytree(args.app, app)
    # Also exercise UTF-8 resource paths without duplicating the large HRIR bank.
    assets = args.output / '移位 资源'
    assets.mkdir()
    def link_or_copy(source, destination):
        try:
            os.link(source, destination)
            return destination
        except OSError:
            return shutil.copy2(source, destination)
    shutil.copytree(args.room.parent, assets / 'room', copy_function=link_or_copy)
    shutil.copytree(args.hrir, assets / 'collection', copy_function=link_or_copy)
    room, hrir = assets / 'room' / args.room.name, assets / 'collection'
    system = Path(os.environ['SystemRoot'])
    env = {key: value for key, value in os.environ.items() if not key.upper().startswith(('CUDA_PATH', 'CUDA_HOME'))}
    env.update(PATH=os.pathsep.join(map(str, [app, system / 'System32', system])),
               MR_ADM_ROOM_WORKERS='8', PYTHONUTF8='1')
    report = {'relocated_unicode_paths': True, 'development_path_removed': True, 'cases': []}
    def run(name, case, selection):
        out = args.output / name
        out.mkdir()
        child_env = dict(env, MR_ADM_ROOM_CUDA=selection)
        command = [sys.executable, str(CHECK), '--case', case,
                   '--library', str(app / 'mradm_capi.dll'), '--plugin', str(app / 'mradm_room_cuda.dll'),
                   '--room', str(room), '--hrir', str(hrir), '--output', str(out)]
        print('portable', name, flush=True)
        result = subprocess.run(command, cwd=app, env=child_env, capture_output=True, timeout=240)
        (out / 'stdout.txt').write_bytes(result.stdout)
        (out / 'stderr.txt').write_bytes(result.stderr)
        assert result.returncode == 0, result.stdout.decode(errors='replace') + result.stderr.decode(errors='replace')
        value = json.loads(result.stdout.decode().strip().splitlines()[-1])
        report['cases'].append({'name': name, 'result': value})
        (args.output / 'report.json').write_text(json.dumps(report, indent=2))
        return out
    def missing(file, cases):
        saved = file.with_name(file.name + '.unavailable')
        file.rename(saved)
        try:
            for values in cases:
                run(*values)
        finally:
            saved.rename(file)
    successful = run('relocated', 'portable', '1')
    assert (successful / 'portable.f32').read_bytes() == (args.reference / 'cuda.f32').read_bytes()
    missing(app / 'mradm_room_cuda.dll', [('missing-plugin', 'missing', '1'),
            ('cpu-without-plugin', 'cpu', '0'), ('exact-without-plugin', 'exact', 'invalid')])
    assert (args.output / 'cpu-without-plugin/cpu.f32').read_bytes() == (args.reference / 'cpu.f32').read_bytes()
    assert (args.output / 'exact-without-plugin/exact.f32').read_bytes() == (args.reference / 'exact.f32').read_bytes()
    missing(next(app.glob('nvrtc64*.dll')), [('missing-nvrtc', 'missing', '1')])
    missing(app / 'room_cuda', [('missing-target', 'missing-target', '1')])
    if args.stale_plugin:
        plugin = app / 'mradm_room_cuda.dll'
        saved = plugin.with_suffix('.current')
        plugin.rename(saved)
        try:
            shutil.copy2(args.stale_plugin, plugin)
            run('stale-plugin', 'wrong-variant', '1')
        finally:
            plugin.unlink(missing_ok=True)
            saved.rename(plugin)
    recovered = run('restored', 'portable', '1')
    assert (successful / 'portable.f32').read_bytes() == (recovered / 'portable.f32').read_bytes()
    report['passed'] = True
    (args.output / 'report.json').write_text(json.dumps(report, indent=2))
    print(json.dumps(report), flush=True)


if __name__ == '__main__':
    main()
