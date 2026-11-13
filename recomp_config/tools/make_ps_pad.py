"""Draw a generic gamepad picture for the "PlayStation prompts" texture set.

The Controls screen shows a controller picture with callout lines pointing at
its sticks and buttons, plus a small controller icon. This draws an original,
unbranded gamepad with the controls in the same places (so the game's callout
lines still land on the right parts) and the four face buttons shown as
cross / circle / square / triangle. Output: 32-bit BGRA BMPs named after the
hash of the texture they replace (see the texture replacement notes in
src/gpu_d3d11.c).

Usage: make_ps_pad.py <out dir>

The pictures shipped in textures_ps/ are now made from a controller photo by
make_ps_pad_photo.py (recomp_config/art/ps_pad_photo.png); this script is the
unbranded alternative.

  F1482F1F  256x224  controller picture on Options > Controls
  9B4581E7  72x72    small controller icon (Profiles, Controllers panel)
"""
import math
import os
import struct
import sys

SS = 3


def ell(x, y, cx, cy, rx, ry):
    return ((x - cx) / rx) ** 2 + ((y - cy) / ry) ** 2


def seg(px, py, ax, ay, bx, by):
    dx, dy = bx - ax, by - ay
    t = max(0.0, min(1.0, ((px - ax) * dx + (py - ay) * dy) / (dx * dx + dy * dy)))
    return math.hypot(px - (ax + t * dx), py - (ay + t * dy))


def body(x, y):
    """<1 inside the pad's outline (coordinates in the 256x224 design space)."""
    return min(ell(x, y, 128, 100, 112, 60), ell(x, y, 50, 148, 40, 56), ell(x, y, 206, 148, 40, 56))


FACE = {  # centre, symbol, colour (design space)
    'triangle': ((200, 58), (96, 208, 160)),
    'square': ((182, 75), (224, 140, 196)),
    'circle': ((218, 75), (236, 96, 96)),
    'cross': ((200, 92), (124, 178, 232)),
}


def face_symbol(kind, dx, dy):
    w = 1.4
    if kind == 'cross':
        return min(seg(dx, dy, -3.6, -3.6, 3.6, 3.6), seg(dx, dy, -3.6, 3.6, 3.6, -3.6)) < w
    if kind == 'circle':
        return abs(math.hypot(dx, dy) - 4.2) < w
    if kind == 'square':
        return abs(max(abs(dx), abs(dy)) - 3.7) < w
    pts = [(0.0, -4.8), (4.6, 3.3), (-4.6, 3.3)]
    return min(seg(dx, dy, *pts[i], *pts[(i + 1) % 3]) for i in range(3)) < w


def colour_at(x, y):
    """RGB at a design-space point, or None outside the pad."""
    b = body(x, y)
    if b >= 1.0:
        # shoulder buttons poke out above the body
        for cx in (62, 194):
            if ell(x, y, cx, 44, 30, 12) < 1.0 and y < 50:
                return (70, 74, 84)
        return None
    shade = 1.0 - 0.30 * (y / 224.0)
    base = (int(58 * shade), int(62 * shade), int(72 * shade))
    if b > 0.93:
        return (120, 150, 190)                                   # rim light
    # sticks: left upper-left, right lower-centre (where the callouts point)
    for cx, cy in ((58, 72), (160, 118)):
        d = math.hypot(x - cx, y - cy)
        if d < 13:
            return (34, 36, 42)
        if d < 20:
            return (86, 90, 100)
        if d < 22:
            return (30, 32, 38)
    # d-pad
    dx, dy = x - 92, y - 120
    if max(abs(dx), abs(dy)) < 19 and (abs(dx) < 6.5 or abs(dy) < 6.5):
        return (30, 32, 38)
    if math.hypot(dx, dy) < 23:
        return (74, 78, 88)
    # face buttons
    for kind, ((cx, cy), col) in FACE.items():
        d = math.hypot(x - cx, y - cy)
        if d < 10:
            if face_symbol(kind, x - cx, y - cy):
                return col
            return (30, 32, 38) if d < 8.6 else (110, 114, 124)
    # two small buttons lower right, two lower left (the extra buttons / start, back)
    for cx, cy, r, col in ((203, 126, 6.5, (200, 204, 212)), (221, 109, 5, (28, 30, 34)),
                           (33, 102, 4.5, (28, 30, 34)), (45, 118, 4.5, (28, 30, 34))):
        if math.hypot(x - cx, y - cy) < r:
            return col
    # plain centre disc where a logo would be
    d = math.hypot(x - 128, y - 68)
    if d < 22:
        return (44, 47, 55) if d < 19 else (96, 100, 112)
    return base


def render(w, h, scale, ox, oy):
    """Draw the 256x224 design scaled by `scale` with its origin at (ox, oy)."""
    out = bytearray(w * h * 4)
    for py in range(h):
        for px in range(w):
            r = g = b = n = 0
            for sy in range(SS):
                for sx in range(SS):
                    x = (px + (sx + 0.5) / SS - ox) / scale
                    y = (py + (sy + 0.5) / SS - oy) / scale
                    c = colour_at(x, y)
                    if c:
                        r += c[0]; g += c[1]; b += c[2]; n += 1
            o = (py * w + px) * 4
            if n:
                out[o] = b // n; out[o + 1] = g // n; out[o + 2] = r // n
                out[o + 3] = 255 * n // (SS * SS)
    return out


def write_bmp(path, w, h, px):
    hdr = bytearray(54)
    hdr[0:2] = b'BM'
    struct.pack_into('<I', hdr, 2, 54 + len(px))
    struct.pack_into('<I', hdr, 10, 54)
    # Bottom-up rows (positive height), as the game's own texture dumps are:
    # the recomp shows top-down BMPs upside down.
    struct.pack_into('<IiiHH', hdr, 14, 40, w, h, 1, 32)
    row = w * 4
    rows = [bytes(px[r * row:(r + 1) * row]) for r in range(h)]
    open(path, 'wb').write(bytes(hdr) + b''.join(reversed(rows)))


out_dir = sys.argv[1]
os.makedirs(out_dir, exist_ok=True)
# Controls picture: the design space is the original texture's 256x224, drawn at 2x.
write_bmp(os.path.join(out_dir, 'F1482F1F_pad.bmp'), 512, 448, render(512, 448, 2.0, 0, 0))
# Small icon: 72x72 cell with the pad centred, drawn at 2x (144x144).
s = 0.54
write_bmp(os.path.join(out_dir, '9B4581E7_padicon.bmp'), 144, 144, render(144, 144, s, 72 - 128 * s, 72 - 112 * s))
print('pad pictures written')
