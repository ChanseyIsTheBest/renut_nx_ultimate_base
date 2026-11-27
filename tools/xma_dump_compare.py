#!/usr/bin/env python3
"""ssasr-nx (2026-11-25): compares two PC runs' XMA dumps (audio_dump_xma_s: xma_<context>_<n>_output_<hz>.wav, the
samples as they reach the game's buffer), for a run on the installer's Opus stand-ins (NAME.xmop) against one on the
original XMA files. Contexts are handed out as the game asks, so the dumps are paired by content: each dump of run B
with the dump of run A (same channels and rate) whose samples line up best (normalised cross-correlation of the mono
mix, any lag), then the aligned overlap's signal-to-difference ratio (a gain fitted).

Opus is lossy: a match is a correlation near 1 and a ratio of some dB, not equal samples. A B dump with no A dump
above 0.5 is reported as unmatched (a sound only one run played, or silence).

  python tools/xma_dump_compare.py <run A dump folder> <run B dump folder> [--seconds 30]
"""
import argparse
import glob
import os
import re
import wave

import numpy as np

NAME = re.compile(r'xma_(\d+)_(\d+)_output_(\d+)\.wav$')


def load(path):
    with wave.open(path, 'rb') as w:
        ch, rate, n = w.getnchannels(), w.getframerate(), w.getnframes()
        x = np.frombuffer(w.readframes(n), dtype='<i2').astype(np.float64) / 32768.0
    x = x.reshape(-1, ch) if ch > 1 else x.reshape(-1, 1)
    return x, ch, rate


def xcorr_peak(a, b):
    """The lag (b against a) and normalised correlation of the best alignment of two mono signals."""
    n = 1 << int(np.ceil(np.log2(len(a) + len(b))))
    fa, fb = np.fft.rfft(a, n), np.fft.rfft(b, n)
    c = np.fft.irfft(fa * np.conj(fb), n)
    k = int(np.argmax(c))
    lag = k if k < n // 2 else k - n
    # normalise by the overlap's energies
    a0, b0 = (lag, 0) if lag >= 0 else (0, -lag)
    m = min(len(a) - a0, len(b) - b0)
    if m <= 0:
        return lag, 0.0, 0
    sa, sb = a[a0:a0 + m], b[b0:b0 + m]
    den = np.sqrt(np.dot(sa, sa) * np.dot(sb, sb))
    return lag, (float(np.dot(sa, sb) / den) if den > 0 else 0.0), m


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('a')
    ap.add_argument('b')
    ap.add_argument('--seconds', type=float, default=30)
    o = ap.parse_args()
    runs = []
    for folder in (o.a, o.b):
        dumps = []
        for p in sorted(glob.glob(os.path.join(folder, 'xma_*_output_*.wav'))):
            x, ch, rate = load(p)
            if len(x) < rate // 2 or not np.any(x):
                continue
            mono = x.mean(axis=1)
            dumps.append({'name': os.path.basename(p), 'x': x, 'mono': mono[:int(o.seconds * rate)], 'ch': ch,
                          'rate': rate})
        runs.append(dumps)
    A, B = runs
    print(f'run A: {len(A)} dumps with sound; run B: {len(B)}')
    matched = 0
    for b in B:
        best = None
        for a in A:
            if a['ch'] != b['ch'] or a['rate'] != b['rate']:
                continue
            lag, corr, m = xcorr_peak(a['mono'], b['mono'])
            if best is None or corr > best[1]:
                best = (a, corr, lag, m)
        if not best or best[1] < 0.5:
            print(f'{b["name"]}: {b["ch"]} ch {b["rate"]} Hz {len(b["x"]) / b["rate"]:.1f} s: no match'
                  + (f' (best {best[1]:.3f} {best[0]["name"]})' if best else ''))
            continue
        a, corr, lag, _ = best
        # the whole overlap at the found lag, every channel: B scaled to A by least squares
        xa, xb = a['x'], b['x']
        a0, b0 = (lag, 0) if lag >= 0 else (0, -lag)
        m = min(len(xa) - a0, len(xb) - b0)
        sa, sb = xa[a0:a0 + m].ravel(), xb[b0:b0 + m].ravel()
        g = float(np.dot(sa, sb) / np.dot(sb, sb)) if np.dot(sb, sb) > 0 else 0.0
        err = sa - g * sb
        snr = 10 * np.log10(np.dot(sa, sa) / np.dot(err, err)) if np.dot(err, err) > 0 else float('inf')
        matched += 1
        print(f'{b["name"]} ~ {a["name"]}: {b["ch"]} ch {b["rate"]} Hz, correlation {corr:.4f}, lag {lag} samples, '
              f'{m / b["rate"]:.1f} s overlap, gain {g:.3f}, {snr:.1f} dB signal to difference')
    print(f'{matched} of {len(B)} B dumps matched an A dump')


if __name__ == '__main__':
    main()
