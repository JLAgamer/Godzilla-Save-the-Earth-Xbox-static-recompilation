"""Count saturated-green pixels below the HUD in a dumped frame (hunting the
"green artifacts on the ground").  Usage: greenscore.py <frame.bmp>"""
import sys, struct
d = open(sys.argv[1], 'rb').read(); off, = struct.unpack_from('<I', d, 10)
w, h = struct.unpack_from('<ii', d, 18); B = struct.unpack_from('<H', d, 28)[0] // 8
row = (w * B + 3) & ~3; H = abs(h); n = 0
for y in range(120, H, 2):
    yy = H - 1 - y if h > 0 else y
    r = d[off + yy * row: off + yy * row + w * B]
    for x in range(0, w, 2):
        b, g, rr = r[x * B], r[x * B + 1], r[x * B + 2]
        if g > 150 and g > rr * 2 and g > b * 2: n += 1
print(n)
