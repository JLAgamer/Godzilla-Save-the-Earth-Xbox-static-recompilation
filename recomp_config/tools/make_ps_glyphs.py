"""Draw replacement button glyphs for the "PlayStation layout" option: plain
geometric symbols (cross, circle, square, triangle) on a dark disc, written as
32-bit BGRA BMPs named after the hash of the texture they replace (see the
texture replacement notes in src/gpu_d3d11.c).

Usage: make_ps_glyphs.py <out dir>

Xbox glyph textures in the menus (32x32, found with GZ_TEX_DUMP; names are the decoded-pixel hash):
  A FB05CA1E -> cross      B 80F0EF50 -> circle
  X 7E614D17 -> square     Y 6D097B4C -> triangle
"""
import math
import os
import struct
import sys

N = 128          # output size
SS = 3           # supersampling per axis
SCALE = 0.80     # the original glyphs leave a margin inside their cell

GLYPHS = {
    'FB05CA1E': ('cross', (124, 178, 232)),
    '80F0EF50': ('circle', (236, 96, 96)),
    '7E614D17': ('square', (224, 140, 196)),
    '6D097B4C': ('triangle', (96, 208, 160)),
}


def seg_dist(px, py, ax, ay, bx, by):
    dx, dy = bx - ax, by - ay
    t = ((px - ax) * dx + (py - ay) * dy) / (dx * dx + dy * dy)
    t = max(0.0, min(1.0, t))
    return math.hypot(px - (ax + t * dx), py - (ay + t * dy))


def symbol(kind, x, y):
    """True when (x, y) in [-1, 1]^2 lies on the symbol's stroke."""
    w = 0.085
    if kind == 'cross':
        return min(seg_dist(x, y, -0.36, -0.36, 0.36, 0.36), seg_dist(x, y, -0.36, 0.36, 0.36, -0.36)) < w
    if kind == 'circle':
        return abs(math.hypot(x, y) - 0.40) < w
    if kind == 'square':
        return abs(max(abs(x), abs(y)) - 0.36) < w
    pts = [(0.0, -0.46), (0.44, 0.32), (-0.44, 0.32)]
    return min(seg_dist(x, y, *pts[i], *pts[(i + 1) % 3]) for i in range(3)) < w


def render(kind, colour):
    out = bytearray(N * N * 4)
    for py in range(N):
        for px in range(N):
            r = g = b = a = 0.0
            for sy in range(SS):
                for sx in range(SS):
                    x = (((px + (sx + 0.5) / SS) / N) * 2 - 1) / SCALE
                    y = (((py + (sy + 0.5) / SS) / N) * 2 - 1) / SCALE
                    d = math.hypot(x, y)
                    if d > 0.92:
                        continue
                    if symbol(kind, x, y):
                        c = colour
                    elif d > 0.84:
                        c = (150, 156, 168)          # rim
                    else:
                        k = 1.0 - 0.35 * (y * 0.5 + 0.5)
                        c = (int(44 * k), int(48 * k), int(58 * k))
                    r += c[0]; g += c[1]; b += c[2]; a += 255
            n = SS * SS
            cov = a / n
            o = (py * N + px) * 4
            if cov > 0:
                out[o] = int(b / (a / 255)); out[o + 1] = int(g / (a / 255)); out[o + 2] = int(r / (a / 255))
            out[o + 3] = int(cov)
    return out


def write_bmp(path, px):
    hdr = bytearray(54)
    hdr[0:2] = b'BM'
    struct.pack_into('<I', hdr, 2, 54 + len(px))
    struct.pack_into('<I', hdr, 10, 54)
    # Bottom-up rows (positive height), as the game's own texture dumps are:
    # the recomp shows top-down BMPs upside down.
    struct.pack_into('<IiiHH', hdr, 14, 40, N, N, 1, 32)
    row = N * 4
    rows = [bytes(px[r * row:(r + 1) * row]) for r in range(N)]
    open(path, 'wb').write(bytes(hdr) + b''.join(reversed(rows)))


out_dir = sys.argv[1]
os.makedirs(out_dir, exist_ok=True)
for h, (kind, colour) in GLYPHS.items():
    write_bmp(os.path.join(out_dir, '%s_%s.bmp' % (h, kind)), render(kind, colour))
    print(h, kind)
