"""Fit a controller photo (RGBA PNG with a transparent background) into the
two "PlayStation prompts" controller textures, in place of the drawn pad from
make_ps_pad.py:

  F1482F1F  controller picture on Options > Controls (512x448, 2x the original)
  9B4581E7  small controller icon on Profiles / Controllers (144x144)

The photo is scaled to fill the same box the drawn pad fills, so the game's
callout lines on the Controls screen point at roughly the same places.
Output: 32-bit BGRA BMPs, bottom-up (positive height) -- the recomp shows
top-down BMPs upside down.

Usage: make_ps_pad_photo.py <photo.png> <out dir>
"""
import os
import struct
import sys

from PIL import Image

# (hash, name, canvas w, h, box left, top, right, bottom): the drawn pad's extent.
TARGETS = [
    ('F1482F1F', 'pad', 512, 448, 20, 64, 492, 408),
    ('9B4581E7', 'padicon', 144, 144, 8, 28, 136, 122),
]


def write_bmp(path, im):
    w, h = im.size
    px = im.tobytes('raw', 'BGRA')
    row = w * 4
    rows = [px[r * row:(r + 1) * row] for r in range(h)]
    hdr = bytearray(54)
    hdr[0:2] = b'BM'
    struct.pack_into('<I', hdr, 2, 54 + len(px))
    struct.pack_into('<I', hdr, 10, 54)
    struct.pack_into('<IiiHH', hdr, 14, 40, w, h, 1, 32)
    open(path, 'wb').write(bytes(hdr) + b''.join(reversed(rows)))


def main():
    photo = Image.open(sys.argv[1]).convert('RGBA')
    photo = photo.crop(photo.getchannel('A').getbbox())
    out_dir = sys.argv[2]
    os.makedirs(out_dir, exist_ok=True)
    for h, name, cw, ch, l, t, r, b in TARGETS:
        bw, bh = r - l, b - t
        s = min(bw / photo.width, bh / photo.height)
        w, hgt = max(1, round(photo.width * s)), max(1, round(photo.height * s))
        # Premultiply before resampling so the transparent edge does not bleed dark fringes.
        pm = photo.convert('RGBa').resize((w, hgt), Image.LANCZOS).convert('RGBA')
        canvas = Image.new('RGBA', (cw, ch), (0, 0, 0, 0))
        canvas.alpha_composite(pm, (l + (bw - w) // 2, t + (bh - hgt) // 2))
        write_bmp(os.path.join(out_dir, '%s_%s.bmp' % (h, name)), canvas)
        print(h, name, '%dx%d' % (w, hgt))


main()
