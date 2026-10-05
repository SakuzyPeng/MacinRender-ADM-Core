#!/usr/bin/env python3
"""Release HOA migration comparison with unchanged backend controls."""
import importlib.util
import json
import sys
from pathlib import Path


def main():
    spec = importlib.util.spec_from_file_location('pcm_compare', Path(__file__).with_name('compare-pcm-mix.py'))
    common = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(common)
    for name, fixture, extra in [
        ('hoa-point', 'objects-point', []),
        ('hoa-extent', 'objects-extent', []),
        ('hoa-multi', 'objects-extent-multi', []),
        ('hoa-cartesian', 'objects-cartesian', []),
        ('hoa-bed', 'directspeakers', []),
        ('hoa-smoothing', 'objects-extent-multi', ['--object-smoothing-frames', '1537']),
        ('hoa-window', 'objects-cartesian', ['--start', '0.037', '--end', '0.193']),
        ('hoa-smoothing-window', 'objects-extent-multi',
         ['--object-smoothing-frames', '1537', '--start', '0.081', '--end', '0.431']),
    ]:
        common.CASES.append((name, fixture, ['--renderer', 'hoa', '--output-layout', 'hoa3', *extra]))
    status = common.main()
    output = Path(sys.argv[sys.argv.index('--output') + 1])
    report = json.loads(output.read_text(encoding='utf-8'))
    report['baseline_commit'] = 'ba96bac'
    output.write_text(json.dumps(report, indent=2) + '\n', encoding='utf-8')
    return status


if __name__ == '__main__':
    sys.exit(main())
