"""bmp2png.py <in.bmp> <out.png> -- full-size 24/32-bit BMP to PNG."""
import struct
import sys
import zlib

d = open(sys.argv[1], 'rb').read()
off = struct.unpack_from('<I', d, 10)[0]
w, h = struct.unpack_from('<ii', d, 18)
bpp = struct.unpack_from('<H', d, 28)[0] // 8
rs = (w * bpp + 3) & ~3
hh = abs(h)
rows = []
for y in range(hh):
    yy = hh - 1 - y if h > 0 else y
    r = d[off + yy * rs: off + yy * rs + w * bpp]
    px = bytearray()
    for x in range(w):
        b, g, rr = r[x * bpp: x * bpp + 3]
        px += bytes((rr, g, b))
    rows.append(b'\0' + bytes(px))


def ch(t, b):
    return struct.pack('>I', len(b)) + t + b + struct.pack('>I', zlib.crc32(t + b))


open(sys.argv[2], 'wb').write(b'\x89PNG\r\n\x1a\n'
                              + ch(b'IHDR', struct.pack('>IIBBBBB', w, hh, 8, 2, 0, 0, 0))
                              + ch(b'IDAT', zlib.compress(b''.join(rows))) + ch(b'IEND', b''))
