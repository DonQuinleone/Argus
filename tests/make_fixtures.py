#!/usr/bin/env python3
"""Generate synthetic WAV fixtures exercising each QA detector.

Usage: python3 make_fixtures.py <output_dir>
Each file is 24-bit/96k stereo, ~3 s, and named for the defect it carries.
"""
import os
import struct
import sys
import wave

import numpy as np

SR = 96000
DUR = 3.0
N = int(SR * DUR)


def tone(freq=220.0, amp=0.2, n=N):
    t = np.arange(n) / SR
    return amp * np.sin(2 * np.pi * freq * t)


def noise_floor(amp=10 ** (-85 / 20), n=N):
    # steady low-level noise = the recording "room tone"
    return amp * np.random.RandomState(1).randn(n)


def write24(path, left, right):
    """Write stereo float arrays (-1..1) as 24-bit PCM WAV."""
    data = np.stack([left, right], axis=1)
    data = np.clip(data, -1.0, 1.0)
    ints = np.round(data * (2 ** 23 - 1)).astype(np.int32)
    raw = bytearray()
    for frame in ints:
        for s in frame:
            raw += struct.pack('<i', int(s))[0:3]  # little-endian 24-bit
    with wave.open(path, 'wb') as w:
        w.setnchannels(2)
        w.setsampwidth(3)
        w.setframerate(SR)
        w.writeframes(bytes(raw))
    print('wrote', os.path.basename(path))


def main(outdir):
    os.makedirs(outdir, exist_ok=True)
    base = tone() + noise_floor()

    # 1. Clean reference
    write24(os.path.join(outdir, 'clean.wav'), base.copy(), base.copy())

    # 2. Dropout: 80 ms where signal (incl. noise floor) is replaced by near-silence
    d = base.copy()
    a = int(1.0 * SR); b = a + int(0.08 * SR)
    d2 = d.copy(); d2[a:b] *= 0.02
    write24(os.path.join(outdir, 'dropout.wav'), d2, d2.copy())

    # 3. Hard digital-zero gap (exact zeros) for 40 ms
    z = base.copy(); z2 = z.copy()
    a = int(1.5 * SR); b = a + int(0.04 * SR)
    z[a:b] = 0.0; z2[a:b] = 0.0
    write24(os.path.join(outdir, 'zerogap.wav'), z, z2)

    # 4. Clipping: loud tone driven past full scale
    c = tone(amp=1.4) + noise_floor()
    write24(os.path.join(outdir, 'clipping.wav'), c, c.copy())

    # 5. Clicks: isolated full-scale spikes
    k = base.copy(); k2 = base.copy()
    for pos in [0.5, 1.2, 2.1]:
        k[int(pos * SR)] = 0.9
        k2[int(pos * SR)] = -0.9
    write24(os.path.join(outdir, 'clicks.wav'), k, k2)

    # 6. DC offset
    write24(os.path.join(outdir, 'dc_offset.wav'), base + 0.05, base + 0.05)

    # 7. Dead right channel
    write24(os.path.join(outdir, 'dead_channel.wav'), base.copy(), np.zeros(N))

    # 8. Out of phase (R = -L)
    write24(os.path.join(outdir, 'out_of_phase.wav'), base.copy(), -base.copy())

    # 9. Leading/trailing silence
    s = np.zeros(N); s2 = np.zeros(N)
    a = int(0.5 * SR); b = N - int(0.7 * SR)
    seg = tone(n=b - a) + noise_floor(n=b - a)
    s[a:b] = seg; s2[a:b] = seg
    write24(os.path.join(outdir, 'head_tail_silence.wav'), s, s2)

    # 10. Fake hi-res: a perfect brickwall at 20 kHz (upsample/lossy signature).
    fh = 0.1 * np.random.RandomState(3).randn(N) + tone(440.0, 0.2)
    spec = np.fft.rfft(fh)
    spec[np.fft.rfftfreq(N, 1 / SR) > 20000] = 0.0
    fh = np.fft.irfft(spec, N)
    write24(os.path.join(outdir, 'fake_hires.wav'), fh, fh.copy())


if __name__ == '__main__':
    main(sys.argv[1] if len(sys.argv) > 1 else 'fixtures')
