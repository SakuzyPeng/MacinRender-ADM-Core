#!/usr/bin/env python3
"""Release migration comparison: shared backend controls plus Triple Balance size/window cases."""
import importlib.util
import json
import sys
import tempfile
from pathlib import Path


def main():
    spec = importlib.util.spec_from_file_location('pcm_compare', Path(__file__).with_name('compare-pcm-mix.py'))
    common = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(common)
    output = Path(sys.argv[sys.argv.index('--output') + 1])
    output.parent.mkdir(parents=True, exist_ok=True)
    policies = {}
    with tempfile.TemporaryDirectory(prefix='triple-policy-', dir=output.parent) as temp:
        for size in [0.01, 0.2, 0.5, 1.0]:
            policy = {'schema': 'mradm.semantic-policy.v1', 'global': {
                'diffuse': {'enabled': False},
                'extent': {'width_scale': 2 * size, 'height_scale': 4 * size, 'depth_scale': 8 * size}}}
            path = Path(temp) / f'size-{size}.json'
            path.write_text(json.dumps(policy), encoding='utf-8')
            policies[str(path.resolve())] = policy
            for layout in ['7.1.4', '9.1.6', '22.2']:
                options = ['--renderer', 'triple-balance', '--output-layout', layout,
                           '--semantic-policy', str(path.resolve())]
                common.CASES.append((f'triple-size-{size}-{layout}', 'objects-cartesian', options))
                if size == 0.5:
                    common.CASES.append((f'triple-size-window-{layout}', 'objects-cartesian',
                                         options + ['--start', '0.037', '--end', '0.193']))
        status = common.main()
        report = json.loads(output.read_text(encoding='utf-8'))
        for case in report['cases']:
            for index, argument in enumerate(case['arguments']):
                if argument in policies:
                    case['semantic_policy'] = policies[argument]
                    case['arguments'][index] = '<generated-semantic-policy>'
        report['baseline_commit'] = '19728179608c4cfe550dc11ff2b3b4425db37963'
        output.write_text(json.dumps(report, indent=2) + '\n', encoding='utf-8')
        return status


if __name__ == '__main__':
    sys.exit(main())
