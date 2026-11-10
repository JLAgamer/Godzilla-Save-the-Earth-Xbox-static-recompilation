"""skyblack.py <frame.bmp> -- percent of near-black pixels in the sky band
(top 200 rows, HUD bars skipped). Prints an integer."""
import struct
import sys

d = open(sys.argv[1], 'rb').read()
off = struct.unpack_from('<I', d, 10)[0]
w, h = struct.unpack_from('<ii', d, 18)
bpp = struct.unpack_from('<H', d, 28)[0] // 8
rs = (w * bpp + 3) & ~3
hh = abs(h)
blk = tot = 0
for y in range(0, 200, 4):
    yy = hh - 1 - y if h > 0 else y
    row = d[off + yy * rs: off + yy * rs + w * bpp]
    for x in range(0, w, 4):
        if 40 <= y <= 120 and (40 <= x <= 300 or 350 <= x <= 600):
            continue
        b, g, r = row[x * bpp: x * bpp + 3]
        tot += 1
        if r < 10 and g < 10 and b < 10:
            blk += 1
print(int(blk * 100 / max(tot, 1)))
