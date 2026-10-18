#!/usr/bin/env python3
"""Build a full-band 96 kHz derivative of the frozen corrected reference room.

Retain the audible late field and synthesize only the previously absent high
band. This is an explicitly synthetic preset, not a new measured room. All
filter support is exported; the common late preparation delay is compensated
by the late-send predelay, and recorded in the generation evidence.
"""
from __future__ import annotations
import argparse
import hashlib
import json
from pathlib import Path
import numpy as np
import scipy
from scipy import signal

RATE = 96000


def read_asset(root, entry):
    name = entry['file']
    if Path(name).name != name or any(c in name for c in '\\:\0'):
        raise ValueError('Invalid asset path')
    raw = (root / name).read_bytes()
    if len(raw) != entry['bytes'] or hashlib.sha256(raw).hexdigest() != entry['sha256']:
        raise ValueError(f'Asset integrity failure: {name}')
    data = np.frombuffer(raw, dtype=entry['dtype']).reshape(entry['shape'])
    if not np.isfinite(data).all():
        raise ValueError('Nonfinite source asset')
    return data


def native_bands(original):
    if original.shape != (8, 512):
        raise ValueError('Expected frozen eight-band, 512-tap material filters')
    # Equal frequency spacing: 48k/512 == 96k/1024. Remove the old linear
    # phase, extend the highest shelf, then install the 512-sample centre.
    old = np.fft.rfft(original, axis=-1) * np.exp(1j * np.pi * np.arange(257))
    grid = np.fft.rfftfreq(1024, 1 / RATE)
    response = np.zeros((8, 513), dtype=complex)
    response[:, :257] = old
    target = np.zeros_like(response)
    target[-1] = 1
    alpha = np.clip((grid - 20000) / 4000, 0, 1)
    response = response * (1 - alpha) + target * alpha
    return np.fft.irfft(response * np.exp(-1j * np.pi * np.arange(513)), n=1024, axis=-1)


def native_eq(original):
    if original.shape != (16384,):
        raise ValueError('Expected frozen 16384-tap EQ')
    db = np.zeros(16385)
    original_db = 20 * np.log10(np.maximum(abs(np.fft.rfft(original)), 1e-20))
    db[:len(original_db)] = original_db
    db[np.fft.rfftfreq(32768, 1 / RATE) >= 300] = 0
    cepstrum = np.fft.irfft(db * np.log(10) / 20, n=32768)
    cepstrum[1:16384] *= 2
    cepstrum[16385:] = 0
    return np.fft.irfft(np.exp(np.fft.rfft(cepstrum)), n=32768)


def native_late(original, seed=1):
    original = np.asarray(original, dtype=np.float64)
    count, frames = original.shape
    # Fit the source's highest useful band using a common energy decay curve.
    sos = signal.butter(4, [12000, 20000], fs=48000, btype='bandpass', output='sos')
    filtered = signal.sosfilt(sos, original, axis=-1)
    energy = np.sum(filtered * filtered, axis=0)
    decay = np.cumsum(energy[::-1])[::-1]
    db = 10 * np.log10(np.maximum(decay / decay[0], 1e-30))
    use = (db <= -5) & (db >= -25)
    slope = np.polyfit(np.arange(frames)[use] / 48000, db[use], 1)[0]
    rt60 = float(-60 / slope)
    if not .05 <= rt60 <= 3:
        raise ValueError('Unusable reference high-band decay')
    # Complete transfer-function resampling: sum(h)=1, not the PCM gain of 2.
    resampler = signal.firwin(41, .5, window=('kaiser', 8.6))
    low = signal.upfirdn(resampler, original, up=2, axis=-1)
    grid = np.linspace(0, RATE / 2, 4097)
    theta = np.clip((grid - 20000) / 4000, 0, 1) * np.pi / 2
    low_filter = signal.firwin2(1025, grid, np.cos(theta), fs=RATE, window=('kaiser', 8.6))
    high_filter = signal.firwin2(1025, grid, np.sin(theta), fs=RATE, window=('kaiser', 8.6))
    low = signal.fftconvolve(low, low_filter[None, :], axes=-1)
    rng = np.random.Generator(np.random.PCG64(seed))
    time = np.arange(frames * 2) / RATE
    noise = rng.standard_normal((count, len(time))) * np.exp(-np.log(1000) * time / rt60)
    noise[:, -480:] *= np.linspace(1, 0, 480)
    high = signal.fftconvolve(noise, high_filter[None, :], axes=-1)
    high = np.pad(high, ((0, 0), (20, 0)))
    # One shared gain, fixed before rendering, preserves directional relations.
    n = 1 << (max(low.shape[1], high.shape[1]) - 1).bit_length()
    old_freq = np.fft.rfftfreq(n, 1 / 48000)
    new_freq = np.fft.rfftfreq(n, 1 / RATE)
    old_power = abs(np.fft.rfft(original.sum(axis=0), n=n)) ** 2
    new_power = abs(np.fft.rfft(high.sum(axis=0), n=n)) ** 2
    target = float(old_power[(old_freq >= 12000) & (old_freq <= 20000)].mean())
    measured = float(new_power[(new_freq >= 26000) & (new_freq <= 44000)].mean())
    gain = float(np.sqrt(target / measured))
    out = np.zeros((count, max(low.shape[1], high.shape[1])))
    out[:, :low.shape[1]] += low
    out[:, :high.shape[1]] += high * gain
    return out, dict(seed=seed, generator='retained-audible-native-high-band-v1',
                    source_band_hz=[12000, 20000], high_band_hz=[24000, 48000],
                    crossover_hz=[20000, 24000], high_band_rt60_seconds=rt60,
                    high_band_common_gain=gain, target_omni_power=target,
                    common_guard_samples=532, source_frames=frames)


def generate(source, output):
    source, output = Path(source), Path(output)
    raw = source.read_bytes()
    room = json.loads(raw)
    if (room['schema'], room['version'], room['kind'], room['sample_rate']) != ('mradm.room', 1, 'hybrid-shoebox-v1', 48000):
        raise ValueError('Requires a 48 kHz hybrid room template')
    arrays = {key: read_asset(source.parent, value) for key, value in room['assets'].items()}
    arrays['bands'] = native_bands(arrays['bands'])
    arrays['eq'] = native_eq(arrays['eq'])
    arrays['late'], late = native_late(arrays['late'])
    room['id'] = 'reference-7x6x3.2-fullband-96k-v1'
    room['name'] = 'Full-band 96 kHz reference listening room'
    room['sample_rate'] = RATE
    room['band_filter_offset'] = 512
    requested_predelay = room['late_predelay_seconds']
    room['late_predelay_seconds'] -= late['common_guard_samples'] / RATE
    if room['late_predelay_seconds'] < 0:
        raise ValueError('Late preparation guard exceeds intended predelay')
    room['generator'] = 'mradm-generate-96k-v1'
    room['generation'] = dict(template_manifest_sha256=hashlib.sha256(raw).hexdigest(),
                              tool_sha256=hashlib.sha256(Path(__file__).read_bytes()).hexdigest(),
                              numpy=np.__version__, scipy=scipy.__version__,
                              material_taps=1024, material_high_band='highest-band absorption to Nyquist',
                              eq_taps=32768, eq_above_300hz_db=0,
                              requested_late_predelay_seconds=requested_predelay, late=late)
    room['validation_scope'] = 'Synthetic 96 kHz extension. Acoustic acceptance is reported separately; no measured ultrasonic room data.'
    output.mkdir(parents=True, exist_ok=False)
    hashes = []
    for name, values in arrays.items():
        entry = room['assets'][name]
        data = np.asarray(values, dtype=entry['dtype'], order='C')
        if not np.isfinite(data).all():
            raise ValueError('Nonfinite generated resource')
        payload = data.tobytes()
        (output / entry['file']).write_bytes(payload)
        entry.update(shape=list(data.shape), bytes=len(payload), sha256=hashlib.sha256(payload).hexdigest())
        hashes.append(f"{entry['sha256']}  {entry['file']}")
    (output / 'room.json').write_text(json.dumps(room, indent=2, ensure_ascii=False) + '\n', encoding='utf-8')
    (output / 'SHA256SUMS').write_text('\n'.join(hashes) + '\n', encoding='utf-8')
    return room


if __name__ == '__main__':
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--template-package', required=True, type=Path)
    parser.add_argument('--output', required=True, type=Path)
    args = parser.parse_args()
    result = generate(args.template_package, args.output)
    print(json.dumps(dict(id=result['id'], sample_rate=RATE, late=result['generation']['late'])))
