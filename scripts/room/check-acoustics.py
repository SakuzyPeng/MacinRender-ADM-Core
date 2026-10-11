#!/usr/bin/env python3
"""Measure the native room model with an ideal omni receiver at fixed positions.

Resource validation and acoustic results are separate. This covers model RIRs,
not a physical room, headphones, background noise or full standards certification.
"""
from __future__ import annotations
import argparse
import hashlib
import json
from pathlib import Path
import subprocess
import tempfile
import numpy as np
import scipy
from scipy import signal

RATE = 48000
RT_BANDS = np.array([63, 80, 100, 125, 160, 200, 250, 315, 400, 500, 630, 800,
                     1000, 1250, 1600, 2000, 2500, 3150, 4000, 5000, 6300, 8000], dtype=float)
FR_BANDS = np.r_[50., RT_BANDS, 10000., 12500., 16000.]
FR_CENTERS = 1000 * 10. ** (np.arange(-13, 13) / 10.)
RT_CENTERS = FR_CENTERS[1:23]
BAND_EDGE_RATIO = 10. ** .05


def fit_decay(response, low=-5, high=-25):
    energy = np.cumsum(response[::-1] ** 2, dtype=np.float64)[::-1]
    if energy[0] <= 0 or not np.isfinite(energy).all():
        return None
    db = 10 * np.log10(np.maximum(energy / energy[0], 1e-30))
    use = (db <= low) & (db >= high)
    if use.sum() < 10:
        return None
    t = np.arange(len(response), dtype=np.float64)[use] / RATE
    values = db[use]
    slope, intercept = np.polyfit(t, values, 1)
    if slope >= 0:
        return None
    variance = np.sum((values - values.mean()) ** 2)
    r2 = 1 - np.sum((values - (slope * t + intercept)) ** 2) / variance
    return {'rt60_seconds': float(-60 / slope), 'fit_r2': float(r2), 'fit_samples': int(use.sum()),
            'fit_interval_seconds': [float(t[0]), float(t[-1])]}


def frequency_levels(response):
    n = 1 << (max(len(response) + RATE, RATE * 2) - 1).bit_length()
    f = np.fft.rfftfreq(n, 1 / RATE)
    spectrum = np.abs(np.fft.rfft(response, n=n)) ** 2
    levels = []
    for centre in FR_CENTERS:
        use = (f >= centre / BAND_EDGE_RATIO) & (f <= centre * BAND_EDGE_RATIO)
        # Pink-noise power per FFT bin is proportional to 1/f. Average in the
        # linear power domain before converting to dB; no per-direction EQ.
        weights = 1 / f[use]
        levels.append(float(10 * np.log10(max(np.dot(spectrum[use], weights) / weights.sum(), 1e-30))))
    return np.array(levels)


def limits(volume):
    nominal = .25 * (volume / 100.) ** (1 / 3)
    upper = nominal + np.interp(np.log(RT_BANDS), np.log([63, 200]), [.3, .05])
    # Both published masks have no specified lower limit below 100 Hz.
    lower = [None if f < 100 else nominal - (.05 if f <= 4000 else .1) for f in RT_CENTERS]
    # Intersection of the two operational-response masks (EBU is stricter).
    fr_lower = -3 - np.maximum(0, np.log2(FR_BANDS / 2000.))
    return nominal, lower, upper, fr_lower


def measure(response, filters, filter_decays, lower, upper):
    rows = []
    for i, (hz, sos) in enumerate(zip(RT_BANDS, filters)):
        filtered = signal.sosfilt(sos, np.pad(response, (0, RATE)))
        t20, t30 = fit_decay(filtered), fit_decay(filtered, high=-35)
        # EBU 3276 section 2.3 specifies the usual -5..-25 dB interval.
        # Keep T30 visible as a supplementary decay-shape diagnostic.
        passed = (t20 is not None and (lower[i] is None or t20['rt60_seconds'] >= lower[i])
                  and t20['rt60_seconds'] <= upper[i])
        rows.append({'hz': hz, 'exact_centre_hz': float(RT_CENTERS[i]), 't20': t20, 't30': t30, 'lower_seconds': lower[i],
                     'upper_seconds': float(upper[i]), 'filter_t20_seconds': filter_decays[i],
                     'filter_shorter_than_measured': bool(t20 is not None and filter_decays[i] < t20['rt60_seconds']),
                     'passed': bool(passed and t20 is not None and filter_decays[i] < t20['rt60_seconds'])})
    return rows


def early_metrics(metadata, room, root):
    def asset(name):
        entry = room['assets'][name]
        filename = entry['file']
        if Path(filename).name != filename or filename in ('.', '..') or ':' in filename:
            raise ValueError('Invalid acoustic component name')
        payload = (root / filename).read_bytes()
        if len(payload) != entry['bytes'] or hashlib.sha256(payload).hexdigest() != entry['sha256']:
            raise ValueError('Acoustic component hash mismatch')
        return np.frombuffer(payload, dtype=entry['dtype']).reshape(entry['shape'])
    bands = asset('bands')
    phases = asset('propagation_phases')
    frequencies = np.fft.rfftfreq(8192, 1 / RATE)
    selected = (frequencies >= 1000) & (frequencies <= 8000)
    def transfer(row):
        material = np.zeros(bands.shape[1])
        for gain, band in zip(row['band_gains'], bands):
            material += band * float(np.float32(gain))
        position = min(1., max(0., row['fraction'])) * 1024
        low = min(int(position), 1023)
        alpha = np.float32(position - low)
        kernel = phases[low] + (phases[low+1] - phases[low]) * alpha
        return (np.fft.rfft(material.astype(np.float32), 8192)
                * np.fft.rfft(kernel, 8192) * float(np.float32(row['gain'])))[selected]
    direct = np.maximum(np.abs(transfer(metadata['direct_analysis'])), 1e-30)
    individual = []
    groups = {}
    for row in metadata['early_reflections']:
        response = transfer(row)
        peak = float(20 * np.log10(max(float(np.max(np.abs(response) / direct)), 1e-30)))
        individual.append(peak)
        row['maximum_1_8khz_db'] = peak
        key = round(row['delay_after_direct_ms'], 6)
        groups[key] = groups.get(key, 0) + response
    combined = [float(20 * np.log10(max(float(np.max(np.abs(v) / direct)), 1e-30))) for v in groups.values()]
    return {'maximum_individual_db': max(individual, default=-600.),
            'maximum_coincident_group_db': max(combined, default=-600.),
            'frequency_grid_hz': RATE / 8192,
            'coincidence_resolution_ms': 1e-6,
            'scope': 'Maximum over 1-8 kHz transfer functions of individual paths and coherently summed coincident arrivals'}


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--probe', type=Path, required=True, help='Release room_omni')
    parser.add_argument('--room', type=Path, required=True)
    parser.add_argument('--positions', type=Path, default=Path(__file__).with_name('reference-positions.json'))
    parser.add_argument('--output', type=Path, required=True)
    args = parser.parse_args()
    room = json.loads(args.room.read_text(encoding='utf-8'))
    definition = json.loads(args.positions.read_text(encoding='utf-8'))
    if definition.get('schema') != 'mradm.room-measurements' or definition.get('version') != 1:
        raise ValueError('Unsupported measurement set')
    nominal, lower, upper, fr_lower = limits(float(np.prod(room['dimensions'])))
    filters = [signal.butter(4, [f / BAND_EDGE_RATIO, f * BAND_EDGE_RATIO], fs=RATE, btype='bandpass', output='sos')
               for f in RT_CENTERS]
    unit = np.zeros(RATE * 2)
    unit[0] = 1.
    floor = [fit_decay(signal.sosfilt(sos, unit))['rt60_seconds'] for sos in filters]
    results = []
    with tempfile.TemporaryDirectory(prefix='mradm-omni-') as tmp:
        for point in definition['positions']:
            output = Path(tmp) / 'response.f32'
            metadata = json.loads(subprocess.check_output([str(args.probe.resolve()), str(args.room.resolve()),
                                  str(point['azimuth']), str(point['elevation']), str(point['distance']), str(output)], text=True, encoding='utf-8'))
            raw = output.read_bytes()
            channels = np.frombuffer(raw, dtype='<f4').reshape(-1, 3).astype(np.float64)
            output.unlink()
            if not np.isfinite(channels).all():
                raise ValueError('Nonfinite measurement')
            response = channels.sum(axis=1)
            decay = measure(response, filters, floor, lower, upper)
            rt_pass = all(row['passed'] for row in decay)
            adjacent = []
            means = {}
            for method in ['t20', 't30']:
                rt = [row[method]['rt60_seconds'] if row[method] else None for row in decay]
                for i in range(1, len(rt)):
                    if rt[i] is None or rt[i-1] is None:
                        adjacent.append({'method': method, 'bands_hz': RT_BANDS[i-1:i+1].tolist(),
                                         'passed': False, 'reason': 'no usable decay fit'})
                        continue
                    limit = .25 * max(rt[i-1:i+1]) if RT_BANDS[i] < 200 else .05
                    adjacent.append({'method': method, 'bands_hz': RT_BANDS[i-1:i+1].tolist(),
                                     'difference_seconds': abs(rt[i] - rt[i-1]), 'limit_seconds': limit,
                                     'passed': bool(abs(rt[i] - rt[i-1]) < limit)})
                means[method] = (float(np.mean(np.array(rt)[(RT_BANDS >= 200) & (RT_BANDS <= 4000)]))
                                 if all(t is not None for t in rt) else None)
            # EBU's mask is also checked around the measured mean, as in the
            # frozen reference validator. Keep the fixed-volume mask above too.
            for row in decay:
                value = row['t20']['rt60_seconds'] if row['t20'] else None
                low_offset = None if row['lower_seconds'] is None else row['lower_seconds'] - nominal
                high_offset = row['upper_seconds'] - nominal
                mean = means['t20']
                row['ebu_measured_mean_passed'] = bool(value is not None and mean is not None
                    and (low_offset is None or value >= mean + low_offset) and value <= mean + high_offset)
            rt_pass = rt_pass and all(row['ebu_measured_mean_passed'] for row in decay)
            early_report = early_metrics(metadata, room, args.room.parent)
            early = max(early_report['maximum_individual_db'], early_report['maximum_coincident_group_db'])
            # Fixed geometric level calibration is specified in the measurement
            # set before rendering. Retain uncalibrated levels and energy too.
            gain = float(point['source_gain'])
            levels = frequency_levels(response)
            rows = {'label': point['label'], 'position_definition': point, 'native': metadata,
                    'response_sha256': hashlib.sha256(raw).hexdigest(), 'branch_energy': np.sum(channels ** 2, axis=0).tolist(),
                    'decay': decay, 'adjacent_bands': adjacent, 'mean_rt_seconds': means,
                    'rt_passed': bool(rt_pass and all(v['passed'] for v in adjacent if v['method'] == 't20') and means['t20'] is not None and .2 < means['t20'] < .4),
                    'maximum_early_reflection_db': early, 'early_passed': early <= -10, 'early_analysis': early_report,
                    'uncalibrated_frequency_levels_db': levels.tolist(),
                    'frequency_levels_db': (levels + 20 * np.log10(gain)).tolist()}
            results.append(rows)
            print(f"{point['label']}: RT={rows['rt_passed']}, early={early:.3f} dB", flush=True)
    # One common reference level for the entire fixed measurement set. The
    # reference is never recomputed per direction to make a failing curve fit.
    mid = (FR_BANDS >= 200) & (FR_BANDS <= 4000)
    common = float(np.mean([np.array(row['frequency_levels_db'])[mid].mean() for row in results[:22]]))
    curves = np.array([r['frequency_levels_db'] for r in results]) - common
    for row, curve in zip(results, curves):
        row['frequency_relative_db'] = curve.tolist()
        row['frequency_passed'] = bool(np.all(curve >= fr_lower) and np.all(curve <= 3.))
        relative = np.array(row['frequency_levels_db']) - np.mean(np.array(row['frequency_levels_db'])[mid])
        row['standard_frequency_relative_db'] = relative.tolist()
        row['frequency_passed'] = row['frequency_passed'] and bool(np.all(relative >= fr_lower) and np.all(relative <= 3.))
        row['passed'] = row['rt_passed'] and row['early_passed'] and row['frequency_passed']
    spread = np.ptp(curves, axis=0)
    report = {'schema': 'mradm.room-acoustic-validation', 'version': 1,
              'passed': bool(all(r['passed'] for r in results) and np.all(spread <= 2.)),
              'resource_integrity': {'passed': True, 'scope': 'Every native invocation loaded and validated the room assets'},
              'nominal_rt_seconds': nominal, 'positions': results, 'frequencies_hz': FR_BANDS.tolist(), 'exact_centres_hz': FR_CENTERS.tolist(),
              'common_reference_level_db': common, 'frequency_lower_db': fr_lower.tolist(), 'frequency_upper_db': 3.,
              'channel_spread_db': spread.tolist(), 'channel_spread_limit_db': 2.,
              'probe_sha256': hashlib.sha256(args.probe.read_bytes()).hexdigest(),
              'tool_sha256': hashlib.sha256(Path(__file__).read_bytes()).hexdigest(),
              'room_manifest_sha256': hashlib.sha256(args.room.read_bytes()).hexdigest(),
              'positions_sha256': hashlib.sha256(args.positions.read_bytes()).hexdigest(),
              'numpy': np.__version__, 'scipy': scipy.__version__,
              'rt_acceptance_method': 'T20 (-5..-25 dB), EBU 3276 section 2.3; T30 is reported separately without replacing T20',
              'method': 'Schroeder backward energy, T20 [-5,-25] and supplementary T30 [-5,-35] least squares; causal order-4 Butterworth decimal third-octave analysis at 1000*10^(k/10), edges 10^(+/-0.05), with one-second zero padding. Both fixed-volume and measured-mean masks apply; the conservative 160/200 Hz adjacent limit is 0.05 s. Filter decay is reported as measurement uncertainty, not subtracted.',
              'level_calibration': definition['level_calibration'],
              'scope': 'Static ideal-omni model at the listed listener/sources and source gains; no physical room, background-noise, headphone or complete standards certification.',
              'references': ['https://www.itu.int/dms_pubrec/itu-r/rec/bs/R-REC-BS.1116-3-201502-I!!PDF-E.pdf',
                             'https://tech.ebu.ch/docs/tech/tech3276.pdf']}
    args.output.write_text(json.dumps(report, indent=2, ensure_ascii=False, allow_nan=False) + '\n', encoding='utf-8')
    print(json.dumps({'passed': report['passed'], 'rt_failures': [r['label'] for r in results if not r['rt_passed']],
                      'frequency_failures': [r['label'] for r in results if not r['frequency_passed']],
                      'maximum_channel_spread_db': float(max(spread))}), flush=True)
    if not report['passed']:
        raise SystemExit(1)


if __name__ == '__main__':
    main()
