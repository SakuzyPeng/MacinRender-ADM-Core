#!/usr/bin/env python3
"""Release ADM migration regression: PCM plus effective semantic metadata.

Use the optional mr_adm_make_libadm_fixture tool for the six input fixtures.
Both binaries must be Release builds. Temporary PCM outputs are removed.
"""
import argparse
import hashlib
import importlib.util
import json
import platform
import tempfile
from pathlib import Path


def load(name):
    spec = importlib.util.spec_from_file_location(name, Path(__file__).with_name(name + '.py'))
    module = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(module)
    return module


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    for name in ('reference', 'candidate', 'pcm-bits', 'fixtures', 'output'):
        parser.add_argument('--' + name, type=Path, required=True)
    parser.add_argument('--baseline-context-head', required=True,
                        help='Source HEAD at baseline archival; binary identity is its SHA-256.')
    args = parser.parse_args()
    common = load('compare-pcm-mix')
    cases = list(common.CASES)
    for name, fixture, flags in load('compare-binaural-dsp').CASES:
        cases.append(('binaural-' + name, fixture,
                      ['--renderer', 'saf-binaural', '--output-layout', 'binaural', *flags]))
    for name, fixture, flags in (
        ('point', 'objects-point', []), ('extent', 'objects-extent', []),
        ('multi', 'objects-extent-multi', []), ('cartesian', 'objects-cartesian', []),
        ('bed', 'directspeakers', []),
        ('smoothing', 'objects-extent-multi', ['--object-smoothing-frames', '1537']),
        ('window', 'objects-cartesian', ['--start', '0.037', '--end', '0.193']),
        ('smoothing-window', 'objects-extent-multi',
         ['--object-smoothing-frames', '1537', '--start', '0.081', '--end', '0.431']),
    ):
        cases.append(('hoa-' + name, fixture, ['--renderer', 'hoa', '--output-layout', 'hoa3', *flags]))
    policy = {'schema': 'mradm.semantic-policy.v1', 'global': {'diffuse': {'enabled': False}}}
    report = {'schema': 'mradm.rust-adm-audio.v1', 'platform': platform.platform(),
              'baseline_context_head': args.baseline_context_head, 'maximum_absolute_tolerance': 2e-6,
              'binary_sha256': {key: hashlib.sha256(getattr(args, key).read_bytes()).hexdigest()
                                for key in ('reference', 'candidate')}, 'cases': []}
    args.output.parent.mkdir(parents=True, exist_ok=True)
    with tempfile.TemporaryDirectory(prefix='adm-compare-', dir=args.output.parent) as directory:
        temp = Path(directory)
        policy_path = (temp / 'policy.json').resolve()
        semantic_path = (temp / 'semantics.json').resolve()
        policy_path.write_text(json.dumps(policy), encoding='utf-8')
        for name, flags in (
            ('ear-direct-extent-policy', ['--renderer', 'ear', '--output-layout', '9.1.6']),
            ('binaural-direct-extent-policy', ['--renderer', 'saf-binaural', '--output-layout', 'binaural',
                                             '--binaural-spread-mode', 'saf-spreader']),
        ):
            cases.append((name, 'objects-extent-multi',
                          flags + ['--semantic-policy', str(policy_path), '--write-semantic-report', str(semantic_path)]))
        for name, fixture, flags in cases:
            source = (args.fixtures / (fixture + '.wav')).resolve()
            outputs, semantics = [], []
            for binary in (args.reference, args.candidate):
                common.run([binary.resolve(), 'render', '-i', source, '-o', temp / 'audio.wav',
                            '--output-bit-depth', 'f32', '--no-peak-limit', *flags])
                common.run([args.pcm_bits.resolve(), 'extract', temp / 'audio.wav', temp / 'audio.pcmbits'])
                outputs.append(common.pcm(temp / 'audio.pcmbits'))
                if '--semantic-policy' in flags:
                    semantics.append(json.loads(semantic_path.read_text(encoding='utf-8')))
            (shape_a, bits_a, a), (shape_b, bits_b, b) = outputs
            if shape_a != shape_b or not a:
                raise ValueError(name + ': changed/empty frame count or channel format')
            maximum = max(abs(x - y) for x, y in zip(a, b))
            entry = {'case': name, 'fixture': fixture, 'fixture_sha256': hashlib.sha256(source.read_bytes()).hexdigest(),
                     'channels': shape_a[0], 'sample_rate': shape_a[1], 'frames': shape_a[2],
                     'max_absolute_error': maximum, 'bit_identical': bits_a == bits_b,
                     'reference_pcm_sha256': hashlib.sha256(bits_a).hexdigest(),
                     'candidate_pcm_sha256': hashlib.sha256(bits_b).hexdigest(),
                     'passed': maximum <= 2e-6}
            if semantics:
                if semantics[0] != semantics[1]:
                    raise ValueError(name + ': effective semantic reports differ')
                blocks = [block for obj in semantics[1]['objects'] for block in obj['blocks']
                          if block['kind'] == 'objects']
                if not blocks or not all(block['effective']['diffuse'] == 0 and
                                         block['effective']['width'] > 0 and
                                         all(block['effective'][k] == block['original'][k]
                                             for k in ('width', 'height', 'depth')) for block in blocks):
                    raise ValueError(name + ': expected direct extent metadata was not exercised')
                entry.update(semantic_policy=policy, semantic_reports_equal=True, direct_extent_blocks=len(blocks))
            report['cases'].append(entry)
            print(f'{name}: max={maximum:.9g}, identical={bits_a == bits_b}', flush=True)
    report['passed'] = all(case['passed'] for case in report['cases'])
    args.output.write_text(json.dumps(report, ensure_ascii=False, indent=2) + '\n', encoding='utf-8')
    return 0 if report['passed'] else 1


if __name__ == '__main__':
    raise SystemExit(main())
