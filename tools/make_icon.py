#!/usr/bin/env python3
"""Generate the Argus application icon (dependency-free, pure stdlib).

Argus is the all-seeing watcher of myth, so the mark is an eye whose iris is a
ring of radial spectrogram bars in the app's cyan->orange palette, on a dark
rounded tile. Writes a 1024x1024 master PNG, then (on macOS) the .icns and a
multi-size .ico so CMake can bundle them, and finally emits a small RGB raster of
the mark as a committed C++ source (EmbeddedLogo.{h,cpp}) for the PDF report.

Re-run only when the mark changes; the generated assets are committed.

Usage: python3 tools/make_icon.py [out_dir]
"""
import math
import os
import struct
import subprocess
import sys
import zlib

S = 1024  # master size


def lerp(a, b, t):
    return a + (b - a) * t


def clamp01(t):
    return max(0.0, min(1.0, t))


def smoothstep(edge0, edge1, x):
    if edge0 == edge1:
        return 0.0 if x < edge0 else 1.0
    t = clamp01((x - edge0) / (edge1 - edge0))
    return t * t * (3.0 - 2.0 * t)


def colormap(t):
    """Cyan -> blue -> orange ramp, roughly matching the spectrogram colormap."""
    t = clamp01(t)
    stops = [
        (0.00, (18, 120, 175)),
        (0.28, (40, 165, 215)),
        (0.50, (90, 195, 225)),
        (0.66, (235, 150, 60)),
        (1.00, (255, 205, 110)),
    ]
    for i in range(len(stops) - 1):
        t0, c0 = stops[i]
        t1, c1 = stops[i + 1]
        if t <= t1:
            f = (t - t0) / (t1 - t0) if t1 > t0 else 0.0
            return tuple(lerp(c0[k], c1[k], f) for k in range(3))
    return tuple(float(v) for v in stops[-1][1])


def rounded_alpha(x, y, w, h, r):
    """Coverage (0..1) of a rounded rectangle at pixel (x, y) with ~1px AA."""
    cx = min(max(x, r), w - r)
    cy = min(max(y, r), h - r)
    d = math.hypot(x - cx, y - cy)
    return clamp01(r - d + 0.5)


# --- Eye geometry (all in fractions of S, centred on the tile) ---
CX = S * 0.5
CY = S * 0.5
EW = S * 0.395          # eye half-width
EH = S * 0.250          # eye half-height (lid opening)
IRIS_R = S * 0.224      # iris outer radius
PUPIL_R = S * 0.083     # pupil radius
BAR_INNER = S * 0.112   # inner radius of the spectrogram bars
N_BARS = 46             # number of radial bars around the iris

# Per-bar outer-radius profile: a fixed, organic-looking pattern so the iris edge
# reads as frequency bars of differing heights radiating from the pupil.
_BAR_H = [
    0.62 + 0.38 * clamp01(
        0.5 + 0.34 * math.sin(k * 2.39) + 0.22 * math.sin(k * 0.83 + 1.1)
        + 0.16 * math.sin(k * 5.27 + 0.4))
    for k in range(N_BARS)
]


def lid_half_height(dx):
    """Half-height of the almond eye opening at horizontal offset dx (px)."""
    u = dx / EW
    if abs(u) >= 1.0:
        return 0.0
    # Pointed-corner almond: ellipse raised to a power < 1 sharpens the canthi.
    return EH * (1.0 - u * u) ** 0.80


def shade(x, y):
    """Return (r, g, b) in 0..255 for one pixel of the mark (pre-tile-alpha)."""
    # Background vertical gradient (dark slate -> near-black).
    g = y / S
    bg = (lerp(17, 8, g), lerp(22, 11, g), lerp(33, 16, g))

    dx = x - CX
    dy = y - CY
    lid = lid_half_height(dx)
    if lid <= 0.0:
        return bg

    # Eye-opening coverage (soft top/bottom lids).
    cover = smoothstep(lid + 1.2, lid - 1.2, abs(dy))
    if cover <= 0.0:
        return bg

    r = math.hypot(dx, dy)
    theta = math.atan2(dy, dx)

    # Sclera inside the lids but outside the iris: faint cool glow.
    sclera = (lerp(20, 10, g), lerp(34, 20, g), lerp(52, 32, g))
    col = sclera

    # Iris ring of radial spectrogram bars.
    if r <= IRIS_R + 2.0 and r >= BAR_INNER - 2.0:
        k = int((theta + math.pi) / (2.0 * math.pi) * N_BARS) % N_BARS
        bar_outer = BAR_INNER + (IRIS_R - BAR_INNER) * _BAR_H[k]
        # Angular gap between bars.
        frac = ((theta + math.pi) / (2.0 * math.pi) * N_BARS) % 1.0
        gap = min(smoothstep(0.0, 0.10, frac), smoothstep(1.0, 0.90, frac))
        radial = smoothstep(BAR_INNER - 1.5, BAR_INNER + 1.5, r) * \
            smoothstep(bar_outer + 1.5, bar_outer - 1.5, r)
        amt = gap * radial
        if amt > 0.0:
            t = clamp01((r - BAR_INNER) / (IRIS_R - BAR_INNER))
            c = colormap(t)
            col = tuple(lerp(col[i], c[i], amt) for i in range(3))

    # Iris base tint (keeps gaps from looking like dead sclera).
    iris_base = smoothstep(IRIS_R + 1.0, IRIS_R - 6.0, r) * \
        smoothstep(PUPIL_R - 2.0, PUPIL_R + 8.0, r)
    if iris_base > 0.0:
        tint = colormap(clamp01((r - PUPIL_R) / (IRIS_R - PUPIL_R)) * 0.6)
        col = tuple(lerp(col[i], lerp(col[i], tint[i], 0.35), iris_base) for i in range(3))

    # Pupil (dark, with a soft outer ramp).
    if r <= PUPIL_R + 6.0:
        pupil = smoothstep(PUPIL_R + 4.0, PUPIL_R - 4.0, r)
        dark = (10.0, 14.0, 22.0)
        col = tuple(lerp(col[i], dark[i], pupil) for i in range(3))
    # Catch-light: small bright dot upper-left of pupil for liveliness.
    gx, gy = CX - PUPIL_R * 0.5, CY - PUPIL_R * 0.7
    gd = math.hypot(x - gx, y - gy)
    gleam = smoothstep(PUPIL_R * 0.42, 0.0, gd)
    col = tuple(lerp(col[i], 245.0, 0.85 * gleam) for i in range(3))

    # Eye outline: thin cyan rim along the lid edge.
    rim = smoothstep(3.4, 1.4, abs(abs(dy) - lid))
    if rim > 0.0:
        col = tuple(lerp(col[i], (70.0, 200.0, 225.0)[i], rim * 0.9) for i in range(3))

    # Apply lid coverage against the background.
    return tuple(lerp(bg[i], col[i], cover) for i in range(3))


def make_pixels():
    px = bytearray(S * S * 4)
    radius = int(S * 0.222)
    for y in range(S):
        for x in range(S):
            a = rounded_alpha(x + 0.5, y + 0.5, S, S, radius)
            r, gg, b = shade(x + 0.5, y + 0.5)
            idx = (y * S + x) * 4
            px[idx + 0] = int(clamp01(r / 255.0) * 255)
            px[idx + 1] = int(clamp01(gg / 255.0) * 255)
            px[idx + 2] = int(clamp01(b / 255.0) * 255)
            px[idx + 3] = int(255 * a)
    return bytes(px)


def write_png(path, width, height, rgba):
    def chunk(tag, data):
        return (struct.pack(">I", len(data)) + tag + data +
                struct.pack(">I", zlib.crc32(tag + data) & 0xFFFFFFFF))

    raw = bytearray()
    for y in range(height):
        raw.append(0)  # filter type 0
        raw.extend(rgba[y * width * 4:(y + 1) * width * 4])
    ihdr = struct.pack(">IIBBBBB", width, height, 8, 6, 0, 0, 0)
    with open(path, "wb") as f:
        f.write(b"\x89PNG\r\n\x1a\n")
        f.write(chunk(b"IHDR", ihdr))
        f.write(chunk(b"IDAT", zlib.compress(bytes(raw), 9)))
        f.write(chunk(b"IEND", b""))


def box_downscale(rgba, sw, sh, dw, dh):
    """Average-downscale an RGBA master to a dw x dh list of RGBA tuples."""
    out = []
    for y in range(dh):
        sy0, sy1 = y * sh // dh, max(y * sh // dh + 1, (y + 1) * sh // dh)
        for x in range(dw):
            sx0, sx1 = x * sw // dw, max(x * sw // dw + 1, (x + 1) * sw // dw)
            r = gg = b = a = n = 0
            for sy in range(sy0, sy1):
                for sx in range(sx0, sx1):
                    p = (sy * sw + sx) * 4
                    r += rgba[p]; gg += rgba[p + 1]; b += rgba[p + 2]; a += rgba[p + 3]
                    n += 1
            out.append((r / n, gg / n, b / n, a / n))
    return out


def write_embedded_logo(rgba, master_w, master_h, out_dir):
    """Emit a small composited-on-panel RGB raster of the mark as C++ source."""
    dw = dh = 160
    panel = (242.0, 242.0, 246.0)  # matches PdfExport kPanel
    px = box_downscale(rgba, master_w, master_h, dw, dh)
    data = bytearray()
    for (r, g, b, a) in px:
        af = a / 255.0
        data.append(int(clamp01((r * af + panel[0] * (1 - af)) / 255.0) * 255))
        data.append(int(clamp01((g * af + panel[1] * (1 - af)) / 255.0) * 255))
        data.append(int(clamp01((b * af + panel[2] * (1 - af)) / 255.0) * 255))

    core = os.path.abspath(os.path.join(out_dir, "..", "..", "core", "export"))
    os.makedirs(core, exist_ok=True)
    h_path = os.path.join(core, "EmbeddedLogo.h")
    c_path = os.path.join(core, "EmbeddedLogo.cpp")
    with open(h_path, "w") as f:
        f.write("// Argus mark as a composited RGB raster for the PDF report header.\n"
                "// Generated by tools/make_icon.py; committed so the build needs no tooling.\n"
                "#pragma once\n\nnamespace argus {\n\n"
                "extern const int kArgusLogoW;\n"
                "extern const int kArgusLogoH;\n"
                "extern const unsigned char kArgusLogoRGB[];  // W*H*3, DeviceRGB\n\n"
                "}  // namespace argus\n")
    with open(c_path, "w") as f:
        f.write("// Generated by tools/make_icon.py. Do not edit by hand.\n"
                '#include "EmbeddedLogo.h"\n\nnamespace argus {\n\n')
        f.write("const int kArgusLogoW = %d;\n" % dw)
        f.write("const int kArgusLogoH = %d;\n" % dh)
        f.write("const unsigned char kArgusLogoRGB[] = {\n")
        for i in range(0, len(data), 20):
            f.write("    " + ",".join(str(v) for v in data[i:i + 20]) + ",\n")
        f.write("};\n\n}  // namespace argus\n")
    print("wrote", h_path)
    print("wrote", c_path)


def main():
    out = sys.argv[1] if len(sys.argv) > 1 else os.path.join(
        os.path.dirname(__file__), "..", "src", "ui", "resources")
    out = os.path.abspath(out)
    os.makedirs(out, exist_ok=True)
    master = os.path.join(out, "argus_1024.png")
    print("rendering", master)
    rgba = make_pixels()
    write_png(master, S, S, rgba)

    # Smaller PNGs for Linux desktop icons (linuxdeploy rejects >512px). Pure
    # stdlib box-downscale so this works on any platform.
    for sz in (512, 256):
        px = box_downscale(rgba, S, S, sz, sz)
        flat = bytearray()
        for (r, g, b, a) in px:
            flat += bytes((int(r), int(g), int(b), int(a)))
        path = os.path.join(out, "argus_%d.png" % sz)
        write_png(path, sz, sz, bytes(flat))
        print("wrote", path)

    # Embedded PDF logo (pure stdlib, works on any platform).
    write_embedded_logo(rgba, S, S, out)

    # macOS .icns via iconutil (needs sips to downscale) + a simple .ico.
    if sys.platform == "darwin":
        iconset = os.path.join(out, "argus.iconset")
        os.makedirs(iconset, exist_ok=True)
        sizes = [16, 32, 64, 128, 256, 512, 1024]
        for s in sizes:
            for scale, suffix in ((1, ""), (2, "@2x")):
                px = s * scale
                if px > 1024:
                    continue
                name = "icon_{0}x{0}{1}.png".format(s, suffix)
                subprocess.run(["sips", "-z", str(px), str(px), master, "--out",
                                os.path.join(iconset, name)],
                               check=True, stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
        icns = os.path.join(out, "argus.icns")
        subprocess.run(["iconutil", "-c", "icns", iconset, "-o", icns], check=True)
        print("wrote", icns)
        ico = os.path.join(out, "argus.ico")
        write_ico_from_master(master, ico)
        print("wrote", ico)


def write_ico_from_master(master_png, ico_path):
    """Build a single-image PNG-compressed .ico (256x256) using sips + raw wrap."""
    tmp = master_png + ".256.png"
    subprocess.run(["sips", "-z", "256", "256", master_png, "--out", tmp],
                   check=True, stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
    with open(tmp, "rb") as f:
        png = f.read()
    os.remove(tmp)
    header = struct.pack("<HHH", 0, 1, 1)
    entry = struct.pack("<BBBBHHII", 0, 0, 0, 0, 1, 32, len(png), 6 + 16)
    with open(ico_path, "wb") as f:
        f.write(header + entry + png)


if __name__ == "__main__":
    main()
