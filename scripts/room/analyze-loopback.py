#!/usr/bin/env python3
"""Correlate native probe output/capture windows without claiming physical latency."""
import argparse
import hashlib
import json
from pathlib import Path
import numpy as np
from scipy import signal


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('recordings', type=Path)
    parser.add_argument('--output', type=Path, required=True)
    args = parser.parse_args()
    metadata = json.loads((args.recordings / 'recordings.json').read_text(encoding='utf-8'))
    sides = [metadata['output_recording'], metadata['input_recording']]
    if any(s['sample_rate'] != 48000 or s['channels'] != 2 for s in sides):
        raise ValueError('Expected two 48 kHz stereo captures')
    samples = [np.memmap(args.recordings / s['file'], dtype='<f4', mode='r').reshape(-1, 2) for s in sides]
    rows = []
    for i, (out, cap) in enumerate(zip(sides[0]['windows'], sides[1]['windows'])):
        count = min(out['frames'], cap['frames'])
        if count < 24000:
            rows.append({'window': i, 'valid': False, 'reason': 'incomplete capture window'})
            continue
        x, y = [np.asarray(a[s['offset_frames']:s['offset_frames'] + count], dtype=np.float64).mean(axis=1)
                for a, s in zip(samples, (out, cap))]
        x -= x.mean()
        y -= y.mean()
        corr = signal.correlate(y, x, mode='full', method='fft')
        lags = signal.correlation_lags(len(y), len(x))
        allowed = np.abs(lags) < count // 2
        peak = int(np.argmax(np.where(allowed, np.abs(corr), -1)))
        lag = int(lags[peak])
        a, b = (x[:count-lag], y[lag:]) if lag >= 0 else (x[-lag:], y[:count+lag])
        norm = np.linalg.norm(a) * np.linalg.norm(b)
        coherence = float(abs(np.dot(a, b)) / norm) if norm else 0.
        other = allowed & (np.abs(lags - lag) > 16)
        ambiguity = float(np.max(np.abs(corr[other])) / max(abs(corr[peak]), 1e-30))
        # Account for independent callback start times in one host monotonic domain.
        delay_ms = (cap['first_callback_ns'] - out['first_callback_ns']) / 1e6 + lag / 48.
        uncertainty_floor = (out['maximum_callback_frames'] + cap['maximum_callback_frames'] + 1) / 48.
        rows.append({'window': i, 'valid': coherence >= .8 and ambiguity < .8, 'correlation': coherence,
                     'secondary_peak_ratio': ambiguity,
                     'lag_frames': lag, 'callback_referenced_delay_ms': delay_ms,
                     'minimum_uncertainty_ms': uncertainty_floor})
    valid = [r['callback_referenced_delay_ms'] for r in rows if r['valid']]
    report = {'schema': 'mradm.room-loopback-latency', 'version': 1, 'windows': rows,
              'valid_windows': len(valid),
              'delay_distribution_ms': dict(zip(['min', 'p50', 'p99', 'max'], map(float, np.quantile(valid, [0, .5, .99, 1])))) if valid else None,
              'scope': 'PCM entering output callback to PCM delivered to selected capture callback. Sensor, OSC, renderer and pre-render queue are excluded.',
              'uncertainty': 'At least the sum of observed maximum output/input callback periods plus one sample. Driver timestamp conventions, clock drift, resampling and scheduler delay are uncalibrated; this is not sensor-to-headphone or acoustic latency.',
              'recording_sha256': {s['file']: hashlib.sha256((args.recordings / s['file']).read_bytes()).hexdigest() for s in sides}}
    args.output.write_text(json.dumps(report, indent=2, allow_nan=False) + '\n', encoding='utf-8')
    print(json.dumps(report, indent=2))
    if not valid:
        raise SystemExit(1)


if __name__ == '__main__':
    main()
