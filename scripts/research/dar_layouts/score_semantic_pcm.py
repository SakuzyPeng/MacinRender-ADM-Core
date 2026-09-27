"""Fixed-level semantic PCM acceptance, reusing the established size metrics."""

import math

import numpy as np

from measure_size_field import spectral_field
from measure_size_motion import energy_envelope


def relative(reference, candidate):
    return float(np.linalg.norm(candidate - reference) / max(np.linalg.norm(reference), 1e-30))


def tail_metrics(reference, candidate):
    indices = np.arange(0, len(reference), 48)
    noise = max(float(np.mean(np.sum(reference[-4800:] ** 2, axis=1))),
                reference.shape[1] * (2 ** -23) ** 2 / 12)
    floor = noise * (len(reference) - indices)
    ref = np.cumsum(np.sum(reference * reference, axis=1)[::-1])[::-1][indices]
    test = np.cumsum(np.sum(candidate * candidate, axis=1)[::-1])[::-1][indices]
    measurable = ref > floor
    error = None
    total_error = None
    if np.any(measurable):
        ref_curve = 10 * np.log10(np.maximum(ref / max(ref[0], 1e-30), 1e-30))
        test_curve = 10 * np.log10(np.maximum(test / max(test[0], 1e-30), 1e-30))
        error = float(np.max(np.abs(test_curve[measurable] - ref_curve[measurable])))
        total_error = 10 * math.log10(max(test[0], 1e-30) / ref[0])
    extra_ok = bool(np.all(test[~measurable] <= floor[~measurable]))
    return ({"curve_error_db": error, "total_error_db": total_error, "extra_tail_ok": extra_ok,
             "passes": bool((error is None or error <= 1) and
                            (total_error is None or abs(total_error) <= 1) and extra_ok)},
            {"tail_reference_cumulative": ref, "tail_candidate_cumulative": test,
             "tail_noise_floor": floor, "tail_measurable": measurable})


def score(x, reference, candidate, case, *, expected_lfe=None):
    if reference.shape != candidate.shape or len(x) != len(reference):
        raise ValueError("PCM duration or channel count differs; no implicit crop/delay correction")
    if not np.isfinite(reference).all() or not np.isfinite(candidate).all():
        return {"passes": False, "failure": "nonfinite_pcm"}, {}
    stop = case["signal_stop_sample"]
    active = slice(4800, stop - 4800)
    xx = x[active].astype(np.float64)
    yy = reference[active].astype(np.float64)
    zz = candidate[active].astype(np.float64)
    input_energy = float(xx @ xx)
    energy = np.sum(yy * yy, axis=0)
    observed_energy = np.sum(zz * zz, axis=0)
    total, observed_total = float(energy.sum()), float(observed_energy.sum())
    if total <= 1e-20:
        return {"passes": bool(np.max(np.abs(candidate)) <= 2 ** -23), "reference_silent": True}, {}
    if observed_total <= 1e-20:
        return {"passes": False, "failure": "unexpected_candidate_silence"}, {}
    power_db = 10 * math.log10(max(observed_total, 1e-30) / total)
    share = energy / total
    observed_share = observed_energy / max(observed_total, 1e-30)
    gains, observed_gains = xx @ yy / input_energy, xx @ zz / input_energy
    band, matrix, coherence = spectral_field(xx, yy)
    try:
        observed_band, observed_matrix, observed_coherence = spectral_field(xx, zz)
    except ValueError as error:
        if str(error) != "zero band energy in size probe":
            raise
        return {"passes": False, "failure": "missing_candidate_band_energy"}, {}
    audible = band >= band.sum(axis=1, keepdims=True) * 1e-4
    band_error = 10 * np.log10(np.maximum(observed_band[audible], 1e-30) / band[audible])
    matrix_error = np.linalg.norm(observed_matrix - matrix, axis=(1, 2)) / np.maximum(
        np.linalg.norm(matrix, axis=(1, 2)), 1e-30)
    times, envelope = energy_envelope(x[:stop], reference[:stop])
    _, observed_envelope = energy_envelope(x[:stop], candidate[:stop])
    dynamic = relative(envelope, observed_envelope)
    events = []
    event_times = {item["relative_start_sample"] for obj in case["identity"]["objects"] for item in obj["events"]}
    event_times.update(case.get("signal_event_samples", []))
    for at in sorted(event_times):
        if at < 12000 or at + 24000 >= stop:
            continue
        before = envelope[(times >= at - 12000) & (times < at - 2400)].mean(axis=0)
        after = envelope[(times >= at + 12000) & (times < at + 24000)].mean(axis=0)
        delta = after - before
        transition = (times >= at - 2400) & (times < at + 12000)
        significant = np.linalg.norm(delta) > .02 * max(np.linalg.norm(before), np.linalg.norm(after), 1e-12)
        offset = None
        found = not significant
        if significant:
            norm = float(delta @ delta)
            def onset(values):
                crossing = np.flatnonzero(((values[transition] - before) @ delta) / norm >= .1)
                return int(times[transition][crossing[0]]) if len(crossing) else None
            ref_at, test_at = onset(envelope), onset(observed_envelope)
            found = ref_at is not None and test_at is not None
            if found:
                offset = abs(ref_at - test_at)
        events.append({"rtime_sample": at, "reference_has_major_change": bool(significant),
                       "onset_error_samples": offset,
                       "passes": bool(found and (offset is None or offset <= 48))})
    tail, tail_arrays = tail_metrics(reference[stop:stop + 36000].astype(np.float64),
                                    candidate[stop:stop + 36000].astype(np.float64))
    gain_error = relative(gains, observed_gains)
    gain_abs = float(np.max(np.abs(gains - observed_gains)))
    size_point = all(item["size"] == 0 for obj in case["identity"]["objects"] for item in obj["events"])
    lfe = float(np.max(np.abs(candidate[:, 3])))
    if expected_lfe is not None and expected_lfe.shape != candidate[:, 3].shape:
        raise ValueError("expected bed LFE must cover the exact file timeline")
    lfe_error = lfe if expected_lfe is None else float(np.max(np.abs(candidate[:, 3] - expected_lfe)))
    unexpected_silence = int(np.count_nonzero((np.max(np.abs(reference), axis=1) > 2 ** -22) &
                                             (np.max(np.abs(candidate), axis=1) <= 2 ** -23)))
    metrics = {"normalized_energy_share_l2": relative(share, observed_share),
               "total_power_error_db": power_db, "signed_gain_relative_l2": gain_error,
               "max_absolute_gain_error": gain_abs, "max_active_band_error_db": float(np.max(np.abs(band_error))),
               "max_cross_spectrum_error": float(np.max(matrix_error)),
               "energy_envelope_nrmse": dynamic, "events": events, "tail": tail, "lfe_max": lfe,
               "lfe_route_max_abs_error": lfe_error,
               "unexpected_silent_frames": unexpected_silence,
               "reference_signed_gain": gains.tolist(), "candidate_signed_gain": observed_gains.tolist(),
               "reference_rms_amplitude": np.sqrt(energy / input_energy).tolist(),
               "candidate_rms_amplitude": np.sqrt(observed_energy / input_energy).tolist(),
               "reference_channel_energy": energy.tolist(), "candidate_channel_energy": observed_energy.tolist(),
               "reference_energy_share": share.tolist(), "candidate_energy_share": observed_share.tolist()}
    metrics["passes"] = bool(metrics["normalized_energy_share_l2"] <= .05 and abs(power_db) <= .1 and
                             metrics["max_active_band_error_db"] <= .5 and
                             metrics["max_cross_spectrum_error"] <= .05 and dynamic <= .02 and
                             all(item["passes"] for item in events) and tail["passes"] and lfe_error <= 1e-7 and
                             unexpected_silence == 0 and
                             (not size_point or (gain_error <= .01 and gain_abs <= .02)))
    arrays = {"reference_band_power": band, "candidate_band_power": observed_band,
              "reference_cross_spectrum": matrix, "candidate_cross_spectrum": observed_matrix,
              "reference_coherence": coherence, "candidate_coherence": observed_coherence,
              "time_samples": times, "reference_energy_envelope": envelope,
              "candidate_energy_envelope": observed_envelope, **tail_arrays}
    return metrics, arrays
