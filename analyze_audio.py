#!/usr/bin/env python3
"""Spectral analysis of a sound file, as plain text.

Decodes any format via ffmpeg (mono float), runs an STFT, and reports:
duration/levels, dominant frequencies over time, spectral centroid, and an
ASCII spectrogram. Stdlib only - no numpy/pip needed.

Usage: python3 analyze_audio.py <file> [segments]
"""
import cmath
import math
import struct
import subprocess
import sys


def decode_mono(path):
    """Return (samples list, sample_rate) decoded to mono float32."""
    probe = subprocess.run(
        ["ffprobe", "-v", "quiet", "-show_streams", "-of", "default=nw=1", path],
        capture_output=True, text=True)
    rate = 44100
    for line in probe.stdout.splitlines():
        if line.startswith("sample_rate="):
            rate = int(line.split("=")[1])
            break
    raw = subprocess.run(
        ["ffmpeg", "-v", "quiet", "-i", path,
         "-ac", "1", "-f", "f32le", "-"],
        capture_output=True).stdout
    samples = list(struct.unpack(f"<{len(raw)//4}f", raw[:len(raw)//4*4]))
    return samples, rate


def fft(x):
    """In-place-ish radix-2 Cooley-Tukey. len(x) must be a power of 2."""
    n = len(x)
    if n == 1:
        return x
    even = fft(x[0::2])
    odd = fft(x[1::2])
    out = [0j] * n
    for k in range(n // 2):
        t = cmath.exp(-2j * math.pi * k / n) * odd[k]
        out[k] = even[k] + t
        out[k + n // 2] = even[k] - t
    return out


def magnitude_spectrum(frame):
    """Hann-windowed FFT magnitudes (first half)."""
    n = len(frame)
    windowed = [frame[i] * (0.5 - 0.5 * math.cos(2*math.pi*i/(n-1)))
                for i in range(n)]
    spec = fft([complex(v, 0) for v in windowed])
    mags = [abs(spec[i]) * 2 / n for i in range(n // 2)]
    peak = max(mags) or 1e-12
    return [m / peak for m in mags]


def top_freqs(mags, rate, count=6, floor=20.0):
    """Peak-pick: local maxima sorted by magnitude, merged if within 2%."""
    cands = []
    for i in range(2, len(mags) - 2):
        f = i * rate / (2 * len(mags))
        if f < floor:
            continue
        if mags[i] > 0.02 and mags[i] >= mags[i-1] and mags[i] >= mags[i+1]:
            # parabolic interpolation for sub-bin accuracy
            a, b, c = mags[i-1], mags[i], mags[i+1]
            denom = a - 2*b + c
            shift = 0.5*(a - c)/denom if abs(denom) > 1e-30 else 0.0
            fpk = ((i + shift) * rate) / (2 * len(mags))
            cands.append((b, fpk))
    cands.sort(reverse=True)
    picked = []
    for mag, f in cands:
        if all(abs(f - g) > 0.02 * g for _, g in picked):
            picked.append((mag, f))
        if len(picked) == count:
            break
    return picked


def centroid(mags, rate):
    num = sum(i*m for i, m in enumerate(mags))
    den = sum(mags) or 1e-12
    return num / den * rate / (2 * len(mags))


def note_name(freq):
    if freq <= 0:
        return "?"
    n = round(12 * math.log2(freq / 440.0)) + 69
    names = ["C","C#","D","D#","E","F","F#","G","G#","A","A#","B"]
    return f"{names[n % 12]}{n // 12 - 1}"


ASCII = " .:-=+*#%@"
def spectrogram_row(mags, lo_bin, hi_bin, width):
    """Log-magnitude row by averaging bins lo..hi into width cells."""
    cells = []
    for c in range(width):
        b0 = lo_bin + (hi_bin - lo_bin) * c // width
        b1 = lo_bin + (hi_bin - lo_bin) * (c + 1) // width
        seg = mags[b0:max(b1, b0 + 1)]
        v = max(seg) ** 0.5  # gamma for visibility
        cells.append(ASCII[min(int(v * len(ASCII)), len(ASCII) - 1)])
    return "".join(cells)


def main():
    if len(sys.argv) < 2:
        sys.exit(__doc__)
    path = sys.argv[1]
    nseg = int(sys.argv[2]) if len(sys.argv) > 2 else 8

    samples, rate = decode_mono(path)
    dur = len(samples) / rate
    peak = max(abs(s) for s in samples) or 1e-12
    rms = math.sqrt(sum(s*s for s in samples) / len(samples))

    print(f"file:     {path}")
    print(f"duration: {dur:.3f}s   sample rate: {rate} Hz   "
          f"peak: {peak:.3f}   rms: {rms:.3f}")

    N = 4096
    hop = max((len(samples) - N) // nseg, 1)
    print(f"\ndominant frequencies ({nseg} segments, {N}-pt STFT):")
    print(f"{'time':>8}  {'centroid':>9}  {'rms':>5}  top frequencies (Hz + note)")
    all_mags = []
    for s in range(nseg):
        start = min(s * hop, len(samples) - N)
        frame = samples[start:start + N]
        frms = math.sqrt(sum(v*v for v in frame) / N)
        mags = magnitude_spectrum(frame)
        all_mags.append(mags)
        tops = top_freqs(mags, rate)
        tfmt = ", ".join(f"{f:7.1f}({note_name(f)})" for _, f in tops[:5])
        print(f"{start/rate:7.2f}s  {centroid(mags, rate):8.0f}  "
              f"{frms:5.2f}  {tfmt}")

    print("\nspectrogram (time ->, freq ^, log-magnitude):")
    width = 78
    rows = 22
    lo_bin = int(20 * 2 * N / rate)      # ~20 Hz
    hi_bin = min(N // 2, int(16000 * 2 * N / rate))  # ~16 kHz
    for r in range(rows, 0, -1):
        # merge spectrum rows geometrically (log frequency axis)
        f_lo = 20.0 * (16000/20) ** ((r - 1) / rows)
        f_hi = 20.0 * (16000/20) ** (r / rows)
        b_lo = max(lo_bin, int(f_lo * 2 * N / rate))
        b_hi = max(b_lo + 1, int(f_hi * 2 * N / rate))
        # average magnitude across segments per column of time
        ncols = []
        for mags in all_mags:
            seg = mags[b_lo:min(b_hi, len(mags))]
            ncols.append(max(seg) if seg else 0.0)
        col_w = max(len(ncols) // 24, 1)
        cells = []
        for c in range(width):
            seg = ncols[(c * len(ncols)) // width:
                        (c * len(ncols)) // width + col_w]
            v = (max(seg) if seg else 0.0) ** 0.5
            cells.append(ASCII[min(int(v * len(ASCII)), len(ASCII)-1)])
        print(f"{f_hi:8.0f} | {''.join(cells)}")
    print("         +" + "-" * width)


if __name__ == "__main__":
    main()
