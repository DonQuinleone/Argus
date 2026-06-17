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


def pack24(channels):
    """Interleave a list of float arrays (-1..1) into little-endian 24-bit PCM bytes."""
    data = np.stack(channels, axis=1)
    data = np.clip(data, -1.0, 1.0)
    ints = np.round(data * (2 ** 23 - 1)).astype(np.int32)
    raw = bytearray()
    for frame in ints:
        for s in frame:
            raw += struct.pack('<i', int(s))[0:3]
    return bytes(raw)


def write24n(path, channels):
    """Write N float channels as a 24-bit PCM WAV (via the stdlib wave module)."""
    raw = pack24(channels)
    with wave.open(path, 'wb') as w:
        w.setnchannels(len(channels))
        w.setsampwidth(3)
        w.setframerate(SR)
        w.writeframes(raw)
    print('wrote', os.path.basename(path))


def tiny_png(w=96, h=96):
    """A small RGB PNG (gradient) built with stdlib zlib - used as embedded cover art."""
    import zlib
    raw = bytearray()
    for y in range(h):
        raw.append(0)  # filter type 0 per scanline
        for x in range(w):
            raw += bytes((x * 255 // w, y * 255 // h, 128))
    def chunk(tag, data):
        return (struct.pack('>I', len(data)) + tag + data +
                struct.pack('>I', zlib.crc32(tag + data) & 0xffffffff))
    ihdr = struct.pack('>IIBBBBB', w, h, 8, 2, 0, 0, 0)  # 8-bit RGB
    return (b'\x89PNG\r\n\x1a\n' + chunk(b'IHDR', ihdr) +
            chunk(b'IDAT', zlib.compress(bytes(raw), 9)) + chunk(b'IEND', b''))


def write_tagged_wav(path, channels, info, id3_frames, cover_png):
    """Write a 24-bit WAV carrying a bext chunk, a RIFF LIST/INFO block, and an ID3v2.3
    chunk (text frames + an APIC cover) - exercises Argus's embedded-metadata reader."""
    pcm = pack24(channels)
    n = len(channels)

    def even(b):
        return b + (b'\x00' if len(b) & 1 else b'')

    fmt = struct.pack('<HHIIHH', 1, n, SR, SR * n * 3, n * 3, 24)

    # bext (BWF) - 602-byte fixed area + coding history.
    def fixed(s, ln):
        b = s.encode('ascii', 'replace')[:ln]
        return b + b'\x00' * (ln - len(b))
    bext = (fixed('Argus test broadcast description', 256) + fixed('Argus', 32) +
            fixed('ARG-REF-001', 32) + fixed('2026-06-17', 10) + fixed('07:09:29', 8) +
            struct.pack('<IIH', 0, 0, 1) + b'\x00' * 64 + struct.pack('<5h', 0, 0, 0, 0, 0) +
            b'\x00' * 180 + b'A=PCM,F=%d,W=24\r\n' % SR)

    # RIFF LIST/INFO.
    info_body = b'INFO'
    for k, v in info:
        val = v.encode('latin-1', 'replace') + b'\x00'
        info_body += k.encode('ascii') + struct.pack('<I', len(val)) + even(val)

    # ID3v2.3 with text frames (latin-1) + APIC (image/png).
    def id3_text(fid, text):
        body = b'\x00' + text.encode('latin-1', 'replace') + b'\x00'
        return fid + struct.pack('>I', len(body)) + b'\x00\x00' + body

    def id3_txxx(desc, val):
        body = b'\x00' + desc.encode('latin-1') + b'\x00' + val.encode('latin-1') + b'\x00'
        return b'TXXX' + struct.pack('>I', len(body)) + b'\x00\x00' + body

    frames = b''
    for fid, text in id3_frames:
        frames += id3_txxx(fid[4:], text) if fid.startswith('TXXX') else id3_text(fid.encode(), text)
    apic = b'\x00' + b'image/png' + b'\x00' + b'\x03' + b'Cover\x00' + cover_png
    frames += b'APIC' + struct.pack('>I', len(apic)) + b'\x00\x00' + apic

    def synchsafe(v):
        return bytes(((v >> 21) & 0x7f, (v >> 14) & 0x7f, (v >> 7) & 0x7f, v & 0x7f))
    id3 = b'ID3\x03\x00\x00' + synchsafe(len(frames)) + frames

    def ck(tag, data):
        return tag + struct.pack('<I', len(data)) + even(data)
    body = (b'WAVE' + ck(b'fmt ', fmt) + ck(b'bext', bext) + ck(b'data', pcm) +
            ck(b'LIST', info_body) + ck(b'id3 ', id3))
    out = b'RIFF' + struct.pack('<I', len(body)) + body
    with open(path, 'wb') as f:
        f.write(out)
    print('wrote', os.path.basename(path))


def write_adm_wav(path, channels, packs, programme, objects):
    """Write a multichannel WAV with chna + minimal axml + a dbmd stub, i.e. an ADM BWF
    (Dolby Atmos) master, so channel-role resolution is testable offline. `packs` is a list
    of audioPackFormat refs, one per channel (bed channels share one; objects differ)."""
    pcm = pack24(channels)
    n = len(channels)
    fmt = struct.pack('<HHIIHH', 1, n, SR, SR * n * 3, n * 3, 24)

    def even(b):
        return b + (b'\x00' if len(b) & 1 else b'')

    # chna: numTracks, numUIDs, then n x 40-byte entries.
    chna = struct.pack('<HH', n, n)
    for i in range(n):
        uid = ('ATU_%08d' % (i + 1)).encode('ascii').ljust(12, b'\x00')[:12]
        tref = ('AT_%08d_01' % (i + 1)).encode('ascii').ljust(14, b'\x00')[:14]
        pref = packs[i].encode('ascii').ljust(11, b'\x00')[:11]
        chna += struct.pack('<H', i + 1) + uid + tref + pref + b'\x00'

    obj_xml = ''.join('<audioObject audioObjectName="%s"></audioObject>' % o for o in objects)
    axml = (
        '<?xml version="1.0" encoding="UTF-8"?><ebuCoreMain><coreMetadata><format>'
        '<audioFormatExtended>'
        '<audioProgramme audioProgrammeName="%s"></audioProgramme>%s'
        '</audioFormatExtended></format></coreMetadata></ebuCoreMain>' % (programme, obj_xml)
    ).encode('utf-8')

    dbmd = b'\x00' * 64  # Dolby metadata stub (presence is what we detect)

    def ck(tag, data):
        return tag + struct.pack('<I', len(data)) + even(data)
    body = (b'WAVE' + ck(b'fmt ', fmt) + ck(b'data', pcm) + ck(b'chna', chna) +
            ck(b'axml', axml) + ck(b'dbmd', dbmd))
    out = b'RIFF' + struct.pack('<I', len(body)) + body
    with open(path, 'wb') as f:
        f.write(out)
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

    # 11. Baked-in clipping: hard-clip a loud tone, then gain down so the flat tops sit
    #     well below full scale (e.g. a clipped master that was later normalised/limited).
    bc = np.clip(tone(amp=1.6) + noise_floor(), -1.0, 1.0)
    bc *= 10 ** (-3.0 / 20)  # attenuate ~3 dB -> ceiling now ~ -3 dBFS, flat tops intact
    write24(os.path.join(outdir, 'baked_clipping.wav'), bc, bc.copy())

    # 12. Embedded metadata: bext + RIFF INFO + ID3 (text frames + APIC cover art).
    md = base.copy()
    write_tagged_wav(
        os.path.join(outdir, 'metadata.wav'), [md, md.copy()],
        info=[('INAM', 'Argus Test Tone'), ('IART', 'Argus'), ('IPRD', 'Argus QA Suite'),
              ('IGNR', 'Test'), ('ITRK', '01'), ('ICRD', '2026')],
        id3_frames=[('TIT2', 'Argus Test Tone'), ('TPE1', 'Argus'), ('TALB', 'Argus QA Suite'),
                    ('TCON', 'Test'), ('TRCK', '01'), ('TYER', '2026'),
                    ('TXXXISRC', 'GBTEST2600001'), ('TXXXEngineer', 'Josh Quinlan')],
        cover_png=tiny_png())

    # 13. Multichannel (5.1) surround - one height/surround channel left silent on purpose.
    #     Must classify as surround, never FAIL on channel count or "dead channel".
    sl = tone(330.0, 0.2) + noise_floor()
    chans = [base.copy(), base.copy(), tone(440.0, 0.15) + noise_floor(),
             0.05 * noise_floor(), sl, np.zeros(N)]  # ch3 ~LFE quiet, ch5 (Rs) silent
    write24n(os.path.join(outdir, 'surround_51.wav'), chans)

    # 14. Dolby Atmos ADM BWF: 7.1.2 bed (10 ch, incl. height Ltf/Rtf) + 2 objects (one
    #     silent). ~1 s to keep the fixture small. Exercises chna/axml role resolution.
    na = SR  # 1 second
    bedtone = (tone(220.0, 0.2, na) + noise_floor(amp=10 ** (-85 / 20), n=na))
    quiet = 0.02 * noise_floor(n=na)
    atmos_chans = [bedtone, bedtone.copy(), tone(330.0, 0.15, na), tone(40.0, 0.1, na),  # L R C LFE
                   quiet, quiet.copy(), quiet.copy(), quiet.copy(),                       # Lss Rss Lrs Rrs
                   tone(500.0, 0.12, na), tone(500.0, 0.12, na),                          # Ltf Rtf (height)
                   tone(660.0, 0.15, na), np.zeros(na)]                                   # Obj1, Obj2 (silent)
    atmos_packs = (['AP_00010005'] * 10) + ['AP_00031001', 'AP_00031002']
    write_adm_wav(os.path.join(outdir, 'atmos.wav'), atmos_chans, atmos_packs,
                  'Atmos_Master', ['Atmos_Bed', 'Obj1', 'Obj2'])

    # 10. Fake hi-res: a perfect brickwall at 20 kHz (upsample/lossy signature).
    fh = 0.1 * np.random.RandomState(3).randn(N) + tone(440.0, 0.2)
    spec = np.fft.rfft(fh)
    spec[np.fft.rfftfreq(N, 1 / SR) > 20000] = 0.0
    fh = np.fft.irfft(spec, N)
    write24(os.path.join(outdir, 'fake_hires.wav'), fh, fh.copy())


if __name__ == '__main__':
    main(sys.argv[1] if len(sys.argv) > 1 else 'fixtures')
