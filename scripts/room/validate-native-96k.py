#!/usr/bin/env python3
"""Release-only full-band room impulse and real-collection tonal validation."""
import argparse
import importlib.util
import json
from pathlib import Path
import subprocess
import tempfile
import numpy as np


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--mradm', required=True, type=Path)
    parser.add_argument('--omni', required=True, type=Path)
    parser.add_argument('--room', required=True, type=Path)
    parser.add_argument('--collection', required=True, type=Path)
    parser.add_argument('--output', required=True, type=Path)
    args = parser.parse_args()
    helper = Path(__file__).resolve().parents[2] / 'tests/unit/room_96k_test.py'
    spec = importlib.util.spec_from_file_location('room96_io', helper)
    io = importlib.util.module_from_spec(spec); spec.loader.exec_module(io)
    cli, omni, room, collection = [str(p.resolve()) for p in (args.mradm, args.omni, args.room, args.collection)]
    info = json.loads(subprocess.check_output([cli, 'room-check', room, '--hrir', collection], text=True))
    assert info['sample_rate'] == info['source_sample_rate'] == 96000 and not info['hrir_resampled']
    assert info['hrir_taps'] == 1920 and info['hrir_preprocessing_guard_frames'] == 0
    assert info['hrtf_collection']['reference_radius_m'] == 1.5
    assert len(info['hrtf_collection']['radii_m']) == 9
    frequencies = [30000, 40000]
    with tempfile.TemporaryDirectory(prefix='mradm-native96-') as directory:
        root = Path(directory); response = root / 'impulse.f32'
        metadata = json.loads(subprocess.check_output([omni, room, '0', '0', '2.1', str(response)], text=True))
        impulse = np.fromfile(response, dtype='<f4').reshape(-1, 3).astype(float)
        assert metadata['sample_rate'] == 96000 and np.isfinite(impulse).all()
        time = np.arange(len(impulse)) / 96000
        transfer = {str(hz): np.abs(np.exp(-2j * np.pi * hz * time) @ impulse).tolist() for hz in frequencies}
        assert all(min(row) > 1e-6 for row in transfer.values()), transfer
        time = np.arange(192000) / 96000
        source, output = root / 'tones.wav', root / 'rendered.wav'
        samples = .005 * (np.sin(2*np.pi*30000*time) + np.sin(2*np.pi*40000*time))
        io.wav_write(source, samples, 96000)
        subprocess.run([cli, 'render', '-i', str(source), '-o', str(output), '--input-channels', 'C',
                        '--renderer', 'room-binaural', '--room', room, '--room-hrir', collection,
                        '--no-peak-limit'], check=True, capture_output=True)
        rate, channels, raw = io.wav_read(output)
        assert (rate, channels) == (96000, 2)
        pcm = np.frombuffer(raw, dtype='<f4').reshape(-1, 2).astype(float)
        assert np.isfinite(pcm).all()
        section = pcm[144000:168000]
        time = np.arange(len(section)) / rate
        tones = {str(hz): (2*np.abs(np.exp(-2j*np.pi*hz*time) @ section) / len(section)).tolist() for hz in frequencies}
        assert all(min(row) > 1e-6 for row in tones.values()), tones
    report = dict(schema='mradm.room-native-96k-validation', version=1, passed=True,
                  source_collection=Path(collection).name, sample_rate=rate,
                  prepared=info, branch_order=['direct','early','late'], impulse_transfer_magnitude=transfer,
                  rendered_tone_amplitude=tones, rendered_frames=len(pcm),
                  scope='Numerical full-band transfer and native-rate loading; synthetic high-band room model.')
    args.output.parent.mkdir(parents=True, exist_ok=True)
    args.output.write_text(json.dumps(report, ensure_ascii=False, indent=2) + '\n', encoding='utf-8')
    print(json.dumps(dict(passed=True, impulse=transfer, tones=tones, frames=len(pcm))))


if __name__ == '__main__':
    main()
