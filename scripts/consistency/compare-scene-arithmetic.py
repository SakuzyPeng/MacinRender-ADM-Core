#!/usr/bin/env python3
"""Compare Release CLIs across the shared Scene arithmetic policy change.

Reuse the PCM comparator's finite-value, shape, hash and 2e-6 error checks;
extend its EAR/VBAP/Triple Balance matrix with binaural geometry and HOA.
"""
import importlib.util
from pathlib import Path


def main():
    source = Path(__file__).with_name('compare-pcm-mix.py')
    spec = importlib.util.spec_from_file_location('scene_pcm_comparison', source)
    module = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(module)
    module.CASES.extend([
        ('binaural-point', 'objects-point',
         ['--renderer', 'saf-binaural', '--output-layout', 'binaural']),
        ('binaural-cloud', 'objects-extent-multi',
         ['--renderer', 'saf-binaural', '--output-layout', 'binaural', '--binaural-spread-mode', 'cloud']),
        ('binaural-rotated', 'objects-extent',
         ['--renderer', 'saf-binaural', '--output-layout', 'binaural', '--binaural-spread-mode', 'cloud',
          '--listener-yaw', '37', '--listener-pitch', '23', '--listener-roll', '-19']),
        ('hoa-extent', 'objects-extent-multi', ['--renderer', 'hoa', '--output-layout', 'hoa3']),
    ])
    return module.main()


if __name__ == '__main__':
    raise SystemExit(main())
